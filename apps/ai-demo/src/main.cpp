// SPDX-License-Identifier: MIT
#include "core.hpp"
#include "qnn.hpp"
#include <gtk/gtk.h>
#include <gst/app/gstappsink.h>
#include <gst/video/video.h>
#include <gst/pbutils/pbutils.h>
#include <atomic>
#include <chrono>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <thread>
#include <sys/resource.h>

namespace {
using Clock=std::chrono::steady_clock;
double elapsed(Clock::time_point from) {return std::chrono::duration<double>(Clock::now()-from).count();}
volatile std::sig_atomic_t interrupted=0;
void signal_handler(int) {interrupted=1;}
std::string quote(const std::string& s) {
    std::ostringstream out; out<<'"';
    for(unsigned char c:s) {
        if(c=='"' || c=='\\') out<<'\\'<<c;
        else if(c<32) out<<"\\u"<<std::hex<<std::setw(4)<<std::setfill('0')<<int(c)<<std::dec;
        else out<<c;
    }
    out<<'"';return out.str();
}
struct Frame {
    int width=0,height=0;
    std::vector<uint8_t> rgb;
    std::vector<ai::Box> boxes;
    double inference_ms=0;
};
struct Options {
    std::string input,model,report,dump,dump_input,snapshot,dsp_dir;
    float threshold=.25f;
    double duration=0;
    int repeat=1;
    bool gui=false;
};
struct State {
    std::mutex mutex;
    std::shared_ptr<Frame> latest;
    std::string error,profile="[]",input;
    std::vector<double> latencies;
    double load_ms=0,seconds=0;
    size_t frames=0;
    std::atomic<bool> stop{false},done{false};
    std::atomic<float> threshold{.25f};
};
Frame from_pixbuf(GdkPixbuf* image) {
    Frame f; f.width=gdk_pixbuf_get_width(image); f.height=gdk_pixbuf_get_height(image);
    if(f.width>16384 || f.height>16384) throw std::runtime_error("Image exceeds 16384 pixels per dimension");
    f.rgb.resize(size_t(f.width)*f.height*3);
    const auto* src=gdk_pixbuf_get_pixels(image);int stride=gdk_pixbuf_get_rowstride(image),n=gdk_pixbuf_get_n_channels(image);
    for(int y=0;y<f.height;++y) for(int x=0;x<f.width;++x) for(int c=0;c<3;++c)
        f.rgb[(size_t(y)*f.width+x)*3+c]=src[y*stride+x*n+c];
    return f;
}
std::vector<float> preprocess(const Frame& frame) {
    ai::Letterbox shape(frame.width,frame.height);
    auto* image=gdk_pixbuf_new_from_data(frame.rgb.data(),GDK_COLORSPACE_RGB,FALSE,8,frame.width,frame.height,frame.width*3,nullptr,nullptr);
    auto* resized=gdk_pixbuf_scale_simple(image,shape.resized_width,shape.resized_height,GDK_INTERP_BILINEAR);
    g_object_unref(image);
    if(!resized) throw std::runtime_error("Image resize failed");
    const auto* src=gdk_pixbuf_get_pixels(resized); int stride=gdk_pixbuf_get_rowstride(resized);
    std::vector<float> result(640*640*3,114.f/255);
    for(int y=0;y<shape.resized_height;++y) for(int x=0;x<shape.resized_width;++x) for(int c=0;c<3;++c)
        result[((y+shape.top)*640+x+shape.left)*3+c]=src[y*stride+x*3+c]/255.f;
    g_object_unref(resized);return result;
}
class Video {
    GstElement* pipeline=nullptr;
    GstAppSink* sink=nullptr;
    GstBus* bus=nullptr;
public:
    ~Video() {if(pipeline) {gst_element_set_state(pipeline,GST_STATE_NULL);gst_object_unref(pipeline);} if(sink) gst_object_unref(sink); if(bus) gst_object_unref(bus);}
    explicit Video(const std::string& path) {
        GError* error=nullptr;
        gchar* uri=g_filename_to_uri(std::filesystem::absolute(path).c_str(),nullptr,nullptr);
        auto* discoverer=gst_discoverer_new(5*GST_SECOND,&error);
        if(!discoverer) {std::string message=error?error->message:"Cannot create media discoverer";g_clear_error(&error);g_free(uri);throw std::runtime_error(message);}
        auto* info=gst_discoverer_discover_uri(discoverer,uri,&error);
        std::string codec;
        if(info) {
            auto* videos=gst_discoverer_info_get_video_streams(info);
            if(videos) {
                auto* caps=gst_discoverer_stream_info_get_caps(GST_DISCOVERER_STREAM_INFO(videos->data));
                if(caps) {codec=gst_structure_get_name(gst_caps_get_structure(caps,0));gst_caps_unref(caps);}
            }
            gst_discoverer_stream_info_list_free(videos);g_object_unref(info);
        }
        g_clear_error(&error);g_object_unref(discoverer);
        if(codec!="video/x-h264" && codec!="video/x-h265") {g_free(uri);throw std::runtime_error("Video requires an H.264 or H.265 stream");}
        const std::string suffix=codec=="video/x-h264"?"h264":"h265";
        // Require linear system-memory frames; Qualcomm UBWC DMABufs cannot be
        // interpreted by the CPU RGB preprocessor. Link the decoder explicitly
        // before negotiation so decodebin cannot select UBWC during autoplugging.
        std::string description="uridecodebin name=source caps="+codec+" ! "+suffix+"parse ! v4l2"+suffix+"dec capture-io-mode=mmap ! video/x-raw,format=NV12 ! videoconvert ! video/x-raw,format=RGB ! appsink name=frames sync=true max-buffers=2 drop=true";
        pipeline=gst_parse_launch(description.c_str(),&error);
        if(error) {std::string message=error->message;g_error_free(error);g_free(uri);if(pipeline) gst_object_unref(pipeline);pipeline=nullptr;throw std::runtime_error(message);}
        auto* source=gst_bin_get_by_name(GST_BIN(pipeline),"source");
        g_object_set(source,"uri",uri,nullptr);g_free(uri);gst_object_unref(source);
        sink=GST_APP_SINK(gst_bin_get_by_name(GST_BIN(pipeline),"frames"));bus=gst_element_get_bus(pipeline);
        if(gst_element_set_state(pipeline,GST_STATE_PLAYING)==GST_STATE_CHANGE_FAILURE) {
            gst_element_set_state(pipeline,GST_STATE_NULL);gst_object_unref(pipeline);gst_object_unref(sink);gst_object_unref(bus);
            pipeline=nullptr;sink=nullptr;bus=nullptr;throw std::runtime_error("Cannot start video decoder");
        }
    }
    enum class Read { Frame, Wait, End };
    Read next(Frame& frame) {
        auto* sample=gst_app_sink_try_pull_sample(sink,100*GST_MSECOND);
        if(!sample) {
            auto* message=gst_bus_pop_filtered(bus,GST_MESSAGE_ERROR);
            if(message) {
                GError* error=nullptr;gchar* debug=nullptr;gst_message_parse_error(message,&error,&debug);
                std::string text=error->message;g_error_free(error);g_free(debug);gst_message_unref(message);throw std::runtime_error(text);
            }
            return gst_app_sink_is_eos(sink) ? Read::End : Read::Wait;
        }
        GstVideoInfo info;gst_video_info_init(&info);
        GstVideoFrame mapped;
        if(!gst_video_info_from_caps(&info,gst_sample_get_caps(sample)) || !gst_video_frame_map(&mapped,&info,gst_sample_get_buffer(sample),GST_MAP_READ)) {
            gst_sample_unref(sample);throw std::runtime_error("Cannot map decoded video frame");
        }
        frame.width=GST_VIDEO_INFO_WIDTH(&info);frame.height=GST_VIDEO_INFO_HEIGHT(&info);
        frame.rgb.resize(size_t(frame.width)*frame.height*3);
        const auto* data=static_cast<const uint8_t*>(GST_VIDEO_FRAME_PLANE_DATA(&mapped,0));
        for(int y=0;y<frame.height;++y) std::copy_n(data+y*GST_VIDEO_FRAME_PLANE_STRIDE(&mapped,0),frame.width*3,frame.rgb.data()+size_t(y)*frame.width*3);
        gst_video_frame_unmap(&mapped);gst_sample_unref(sample);return Read::Frame;
    }
};
void run(Options opt,State& state) {
    auto start=Clock::now();
    try {
        if(!std::filesystem::is_regular_file(opt.input)) throw std::runtime_error("Input file does not exist: "+opt.input);
        auto model_start=Clock::now();ai::Qnn qnn(opt.model);
        {std::lock_guard<std::mutex> lock(state.mutex);state.load_ms=elapsed(model_start)*1000;}
        GError* error=nullptr; auto* image=gdk_pixbuf_new_from_file(opt.input.c_str(),&error);
        if(error) g_error_free(error);
        Frame still;
        if(image) {still=from_pixbuf(image);g_object_unref(image);}
        std::unique_ptr<Video> video;
        if(still.rgb.empty()) video=std::make_unique<Video>(opt.input);
        start=Clock::now();auto last_frame=start;
        size_t count=0;
        while(!state.stop && !interrupted) {
            if(opt.duration>0 && elapsed(start)>=opt.duration) break;
            Frame frame;
            if(video) {
                auto status=video->next(frame);
                if(status==Video::Read::Wait) {
                    if(elapsed(last_frame)>15) throw std::runtime_error("Video produced no frame for 15 seconds");
                    continue;
                }
                if(status==Video::Read::End) {
                    if(!count) throw std::runtime_error("Video contains no decodable frames");
                    if(opt.duration<=0) break;
                    video=std::make_unique<Video>(opt.input);continue;
                }
            } else {
                if(opt.duration<=0 && count>=size_t(opt.repeat)) break;
                frame=still;
            }
            last_frame=Clock::now();
            auto input=preprocess(frame);
            if(count==0 && !opt.dump_input.empty()) {
                std::ofstream dump(opt.dump_input,std::ios::binary);dump.write(reinterpret_cast<const char*>(input.data()),input.size()*sizeof(float));
                if(!dump) throw std::runtime_error("Cannot write input dump");
            }
            auto inference=Clock::now();auto output=qnn.execute(input);
            frame.inference_ms=elapsed(inference)*1000;
            frame.boxes=ai::decode(output,qnn.channels_last(),ai::Letterbox(frame.width,frame.height),state.threshold.load());
            if(count==0 && !opt.dump.empty()) {
                std::ofstream dump(opt.dump,std::ios::binary);dump.write(reinterpret_cast<const char*>(output.data()),output.size()*sizeof(float));
                if(!dump) throw std::runtime_error("Cannot write tensor dump");
            }
            auto profile=qnn.profile_json();++count;
            {std::lock_guard<std::mutex> lock(state.mutex);
                state.latencies.push_back(frame.inference_ms);state.frames=count;state.seconds=elapsed(start);
                state.latest=std::make_shared<Frame>(std::move(frame));state.profile=std::move(profile);
            }
        }
        if(count==0 && !state.stop && !interrupted) throw std::runtime_error("No frames processed");
    } catch(const std::exception& e) {std::lock_guard<std::mutex> lock(state.mutex);state.error=e.what();}
    state.done=true;
}
void draw_frame(cairo_t* cr,const Frame& frame,double width,double height) {
    cairo_set_source_rgb(cr,.035,.055,.085);cairo_paint(cr);
    double scale=std::min(width/frame.width,height/frame.height);
    cairo_save(cr);cairo_translate(cr,(width-frame.width*scale)/2,(height-frame.height*scale)/2);cairo_scale(cr,scale,scale);
    auto* image=gdk_pixbuf_new_from_data(frame.rgb.data(),GDK_COLORSPACE_RGB,FALSE,8,frame.width,frame.height,frame.width*3,nullptr,nullptr);
    gdk_cairo_set_source_pixbuf(cr,image,0,0);cairo_paint(cr);g_object_unref(image);
    for(auto b:frame.boxes) {
        cairo_set_source_rgb(cr,.15,.95,.65);cairo_set_line_width(cr,2/scale);cairo_rectangle(cr,b.x1,b.y1,b.x2-b.x1,b.y2-b.y1);cairo_stroke(cr);
        std::ostringstream text;text<<ai::label(b.category)<<" "<<int(b.score*100)<<"%";
        cairo_set_font_size(cr,15/scale);cairo_text_extents_t ext;cairo_text_extents(cr,text.str().c_str(),&ext);
        double y=std::max(double(b.y1),22/scale);
        cairo_rectangle(cr,b.x1,y-22/scale,(ext.width+10/scale),22/scale);cairo_fill(cr);
        cairo_set_source_rgb(cr,.015,.08,.06);cairo_move_to(cr,b.x1+5/scale,y-6/scale);cairo_show_text(cr,text.str().c_str());
    }
    cairo_restore(cr);
}
void report(const Options& opt,State& state) {
    std::lock_guard<std::mutex> lock(state.mutex);
    auto sorted=state.latencies;std::sort(sorted.begin(),sorted.end());
    auto percentile=[&](double p){return sorted.empty()?0:sorted[size_t(std::ceil(p*sorted.size()))-1];};
    struct rusage usage{};getrusage(RUSAGE_SELF,&usage);
    std::ostringstream out;out<<std::setprecision(9)<<"{\"schema_version\":1,\"status\":"<<quote(state.error.empty()?(state.stop||interrupted?"CANCELLED":"PASS"):"FAIL")
        <<",\"error\":"<<quote(state.error)<<",\"backend\":\"QNN_HTP\",\"input\":"<<quote(opt.input)
        <<",\"model\":"<<quote(opt.model)<<",\"frames\":"<<state.frames<<",\"load_ms\":"<<state.load_ms
        <<",\"elapsed_seconds\":"<<state.seconds<<",\"fps\":"<<(state.seconds>0?state.frames/state.seconds:0)
        <<",\"inference_p50_ms\":"<<percentile(.5)<<",\"inference_p95_ms\":"<<percentile(.95)
        <<",\"peak_rss_kib\":"<<usage.ru_maxrss<<",\"timing_definition\":\"QNN execute including I/O quantization and dequantization; excludes image preprocessing and NMS\",\"detections\":[";
    if(state.latest) for(size_t i=0;i<state.latest->boxes.size();++i) {
        auto b=state.latest->boxes[i];if(i) out<<",";
        out<<"{\"class_id\":"<<b.category<<",\"label\":"<<quote(ai::label(b.category))<<",\"score\":"<<b.score<<",\"box\":["<<b.x1<<","<<b.y1<<","<<b.x2<<","<<b.y2<<"]}";
    }
    out<<"],\"qnn_profile\":"<<state.profile<<"}\n";
    if(!opt.report.empty()) {
        std::ofstream file(opt.report+".tmp");file<<out.str();file.close();
        if(!file) throw std::runtime_error("Cannot write report: "+opt.report);
        std::filesystem::rename(opt.report+".tmp",opt.report);
    }
    if(!opt.snapshot.empty() && state.latest) {
        auto& f=*state.latest;auto* surface=cairo_image_surface_create(CAIRO_FORMAT_RGB24,f.width,f.height);auto* cr=cairo_create(surface);
        draw_frame(cr,f,f.width,f.height);cairo_destroy(cr);auto status=cairo_surface_write_to_png(surface,opt.snapshot.c_str());cairo_surface_destroy(surface);
        if(status!=CAIRO_STATUS_SUCCESS) throw std::runtime_error("Cannot write snapshot");
    }
    std::cout<<out.str();
}
struct Ui {
    Options opt;State state;std::thread worker;
    GtkWidget *window=nullptr,*canvas=nullptr,*status=nullptr,*file_label=nullptr,*start=nullptr,*open=nullptr,*demo=nullptr,*demo_video=nullptr;
    bool reported=false;
    explicit Ui(Options options):opt(std::move(options)) {state.threshold=opt.threshold;}
    void stop() {state.stop=true;if(worker.joinable()) worker.join();}
    ~Ui(){stop();}
    void begin() {
        if(worker.joinable()) worker.join();
        {std::lock_guard<std::mutex> lock(state.mutex);state.latest.reset();state.error.clear();state.profile="[]";state.frames=0;state.latencies.clear();state.seconds=0;state.load_ms=0;}
        state.done=false;state.stop=false;reported=false;
        gtk_label_set_text(GTK_LABEL(file_label),std::filesystem::path(opt.input).filename().c_str());
        gtk_label_set_text(GTK_LABEL(status),"Loading model on NPU…");
        gtk_widget_set_sensitive(start,FALSE);gtk_widget_set_sensitive(open,FALSE);gtk_widget_set_sensitive(demo,FALSE);gtk_widget_set_sensitive(demo_video,FALSE);
        worker=std::thread(run,opt,std::ref(state));
    }
    void create() {
        window=gtk_window_new(GTK_WINDOW_TOPLEVEL);gtk_window_set_title(GTK_WINDOW(window),"Q6A AI · Object Detection");gtk_window_set_default_size(GTK_WINDOW(window),960,540);
        auto* header=gtk_header_bar_new();gtk_header_bar_set_title(GTK_HEADER_BAR(header),"Q6A AI");gtk_header_bar_set_show_close_button(GTK_HEADER_BAR(header),TRUE);gtk_window_set_titlebar(GTK_WINDOW(window),header);
        auto* css=gtk_css_provider_new();gtk_css_provider_load_from_data(css,
            "window { background: #101b2b; color: #e6edf5; } headerbar { background: #152238; color: #e6edf5; } button { background-image: none; background-color: #23374e; border: 1px solid #425873; padding: 7px 12px; color: #e6edf5; } button:hover { background-color: #304b65; } button:disabled { background-color: #172639; color: #8190a3; } button label { color: inherit; } label { color: #e6edf5; } .heading { font-size: 22px; font-weight: bold; } .muted { color: #9aafc5; }",-1,nullptr);
        gtk_style_context_add_provider_for_screen(gdk_screen_get_default(),GTK_STYLE_PROVIDER(css),GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);g_object_unref(css);
        auto* root=gtk_box_new(GTK_ORIENTATION_VERTICAL,8);gtk_container_set_border_width(GTK_CONTAINER(root),16);gtk_container_add(GTK_CONTAINER(window),root);
        auto* title=gtk_label_new("Q6A AI   /   Object Detection");gtk_widget_set_halign(title,GTK_ALIGN_START);gtk_style_context_add_class(gtk_widget_get_style_context(title),"heading");gtk_box_pack_start(GTK_BOX(root),title,FALSE,FALSE,0);
        auto* subtitle=gtk_label_new("YOLOv8n  ·  Qualcomm Hexagon NPU  ·  Offline inference");gtk_widget_set_halign(subtitle,GTK_ALIGN_START);gtk_style_context_add_class(gtk_widget_get_style_context(subtitle),"muted");gtk_box_pack_start(GTK_BOX(root),subtitle,FALSE,FALSE,0);
        auto* controls=gtk_box_new(GTK_ORIENTATION_HORIZONTAL,8);gtk_box_pack_start(GTK_BOX(root),controls,FALSE,FALSE,0);
        demo=gtk_button_new_with_label("Demo image");demo_video=gtk_button_new_with_label("Demo video");open=gtk_button_new_with_label("Open file…");start=gtk_button_new_with_label("Start");auto* halt=gtk_button_new_with_label("Stop");
        for(auto* w:{demo,demo_video,open,start,halt}) gtk_box_pack_start(GTK_BOX(controls),w,FALSE,FALSE,0);
        auto* threshold=gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL,.05,.95,.05);gtk_range_set_value(GTK_RANGE(threshold),opt.threshold);gtk_widget_set_size_request(threshold,170,-1);
        gtk_box_pack_end(GTK_BOX(controls),threshold,FALSE,FALSE,0);gtk_box_pack_end(GTK_BOX(controls),gtk_label_new("Confidence"),FALSE,FALSE,0);
        file_label=gtk_label_new("Choose an image or video to begin");gtk_widget_set_halign(file_label,GTK_ALIGN_START);gtk_box_pack_start(GTK_BOX(root),file_label,FALSE,FALSE,0);
        canvas=gtk_drawing_area_new();gtk_widget_set_size_request(canvas,480,180);gtk_box_pack_start(GTK_BOX(root),canvas,TRUE,TRUE,0);
        status=gtk_label_new("Ready · NPU required");gtk_widget_set_halign(status,GTK_ALIGN_START);gtk_label_set_line_wrap(GTK_LABEL(status),TRUE);gtk_box_pack_start(GTK_BOX(root),status,FALSE,FALSE,0);
        g_signal_connect(window,"destroy",G_CALLBACK(+[](GtkWidget*,gpointer p){auto& u=*static_cast<Ui*>(p);u.stop();gtk_main_quit();}),this);
        g_signal_connect(canvas,"draw",G_CALLBACK(+[](GtkWidget* w,cairo_t* cr,gpointer p)->gboolean {
            auto& s=static_cast<Ui*>(p)->state;std::lock_guard<std::mutex> lock(s.mutex);
            if(s.latest) draw_frame(cr,*s.latest,gtk_widget_get_allocated_width(w),gtk_widget_get_allocated_height(w));
            else {cairo_set_source_rgb(cr,.035,.055,.085);cairo_paint(cr);}
            return TRUE;
        }),this);
        g_signal_connect(start,"clicked",G_CALLBACK(+[](GtkButton*,gpointer p){static_cast<Ui*>(p)->begin();}),this);
        g_signal_connect(halt,"clicked",G_CALLBACK(+[](GtkButton*,gpointer p){static_cast<Ui*>(p)->state.stop=true;}),this);
        g_signal_connect(threshold,"value-changed",G_CALLBACK(+[](GtkRange* w,gpointer p){static_cast<Ui*>(p)->state.threshold=float(gtk_range_get_value(w));}),this);
        g_signal_connect(demo,"clicked",G_CALLBACK(+[](GtkButton*,gpointer p){auto& u=*static_cast<Ui*>(p);u.opt.input=(std::filesystem::path(u.opt.model).parent_path().parent_path()/"data/bus.jpg").string();u.begin();}),this);
        g_signal_connect(demo_video,"clicked",G_CALLBACK(+[](GtkButton*,gpointer p){auto& u=*static_cast<Ui*>(p);u.opt.input=(std::filesystem::path(u.opt.model).parent_path().parent_path()/"data/demo.mp4").string();u.begin();}),this);
        g_signal_connect(open,"clicked",G_CALLBACK(+[](GtkButton*,gpointer p){
            auto& u=*static_cast<Ui*>(p);auto* dialog=gtk_file_chooser_dialog_new("Open image or video",GTK_WINDOW(u.window),GTK_FILE_CHOOSER_ACTION_OPEN,"Cancel",GTK_RESPONSE_CANCEL,"Open",GTK_RESPONSE_ACCEPT,nullptr);
            if(gtk_dialog_run(GTK_DIALOG(dialog))==GTK_RESPONSE_ACCEPT) {char* path=gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));u.opt.input=path;g_free(path);gtk_widget_destroy(dialog);u.begin();}
            else gtk_widget_destroy(dialog);
        }),this);
        gtk_widget_show_all(window);gtk_window_maximize(GTK_WINDOW(window));
        g_timeout_add(100,+[](gpointer p)->gboolean {
            auto& u=*static_cast<Ui*>(p);
            {std::lock_guard<std::mutex> lock(u.state.mutex);
                if(!u.state.error.empty()) gtk_label_set_text(GTK_LABEL(u.status),("Error: "+u.state.error).c_str());
                else if(u.state.latest) {
                    std::ostringstream text;text<<"NPU  ·  "<<u.state.latest->boxes.size()<<" objects  ·  "<<std::fixed<<std::setprecision(1)<<u.state.latest->inference_ms<<" ms  ·  "<<(u.state.seconds>0?u.state.frames/u.state.seconds:0)<<" FPS  ·  "<<u.state.frames<<" frames"<<(u.state.done?"  ·  Finished":"");
                    gtk_label_set_text(GTK_LABEL(u.status),text.str().c_str());gtk_widget_queue_draw(u.canvas);
                }
            }
            if(u.state.done && !u.reported) {
                if(u.worker.joinable()) u.worker.join();
                try {report(u.opt,u.state);} catch(const std::exception& e){gtk_label_set_text(GTK_LABEL(u.status),e.what());}
                u.reported=true;gtk_widget_set_sensitive(u.start,TRUE);gtk_widget_set_sensitive(u.open,TRUE);gtk_widget_set_sensitive(u.demo,TRUE);gtk_widget_set_sensitive(u.demo_video,TRUE);
            }
            if(interrupted) {gtk_widget_destroy(u.window);return G_SOURCE_REMOVE;}
            return G_SOURCE_CONTINUE;
        },this);
        if(!opt.input.empty()) begin();
        gtk_main();
        if(state.done && !reported) report(opt,state);
    }
};
void self_test() {
    ai::Letterbox shape(1280,720);auto b=shape.restore({0,140,640,500,.9,0});
    if(b.x2!=1280 || b.y1!=0 || b.y2!=720 || ai::quantize(.5f,.01f,-10,255)!=60) throw std::runtime_error("Core self-test failed");
    std::cout<<"AI demo hardware-independent self-test PASS\n";
}
}
int main(int argc,char** argv) {
    Options opt;
    try {
        std::filesystem::path executable=std::filesystem::canonical("/proc/self/exe");
        opt.model=(executable.parent_path().parent_path()/"share/ai-demo/models/yolov8n.bin").string();
        for(int i=1;i<argc;++i) {
            std::string key=argv[i];
            if(key=="--self-test") {self_test();return 0;}
            if(key=="--gui") {opt.gui=true;continue;}
            if(key=="--help") {std::cout<<"ai-demo [--gui] --input FILE [--model CONTEXT.bin] [--report JSON] [--snapshot PNG] [--dump-output RAW] [--confidence 0.25] [--duration SECONDS] [--repeat COUNT]\n";return 0;}
            if(i+1>=argc) throw std::runtime_error("Missing value for "+key);
            std::string value=argv[++i];
            if(key=="--input") opt.input=value;else if(key=="--model") opt.model=value;
            else if(key=="--report") opt.report=value;else if(key=="--snapshot") opt.snapshot=value;
            else if(key=="--dump-output") opt.dump=value;
            else if(key=="--dump-input") opt.dump_input=value;
            else if(key=="--dsp-dir") opt.dsp_dir=value;
            else if(key=="--confidence" || key=="--duration" || key=="--repeat") {
                size_t used=0;double n=std::stod(value,&used);
                if(used!=value.size() || !std::isfinite(n)) throw std::runtime_error("Invalid numeric argument");
                if(key=="--confidence") {if(n<=0 || n>=1) throw std::runtime_error("Confidence must be between 0 and 1");opt.threshold=n;}
                if(key=="--duration") {if(n<0 || n>86400) throw std::runtime_error("Duration must be 0..86400 seconds");opt.duration=n;}
                if(key=="--repeat") {if(n<1 || n>1000000 || n!=std::floor(n)) throw std::runtime_error("Repeat must be 1..1000000");opt.repeat=n;}
            } else throw std::runtime_error("Unknown option: "+key);
        }
        // FastRPC 1.0.7 open_shell ignores ADSP_LIBRARY_PATH, but searches cwd.
        // Resolve user paths before selecting the private DSP shell directory.
        for(auto* path:{&opt.input,&opt.model,&opt.report,&opt.dump,&opt.dump_input,&opt.snapshot,&opt.dsp_dir})
            if(!path->empty()) *path=std::filesystem::absolute(*path).string();
        if(!opt.dsp_dir.empty()) std::filesystem::current_path(opt.dsp_dir);
        std::signal(SIGINT,signal_handler);std::signal(SIGTERM,signal_handler);gst_init(nullptr,nullptr);
        if(opt.gui) {
            if(!gtk_init_check(nullptr,nullptr)) throw std::runtime_error("Cannot connect to desktop; set XDG_RUNTIME_DIR and WAYLAND_DISPLAY");
            Ui ui(opt);ui.create();return ui.state.error.empty()?0:1;
        }
        if(opt.input.empty()) throw std::runtime_error("Use --input FILE or --gui");
        State state;state.threshold=opt.threshold;run(opt,state);report(opt,state);return state.error.empty()?(interrupted?130:0):1;
    } catch(const std::exception& e) {std::cerr<<"ai-demo: "<<e.what()<<"\n";return 1;}
}
