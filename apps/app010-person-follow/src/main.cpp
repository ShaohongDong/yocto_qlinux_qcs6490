// SPDX-License-Identifier: MIT
#include "core.hpp"
#include "tracker.hpp"
#include "follow.hpp"
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
    ai::FollowResult follow;
    double inference_ms=0, pipeline_ms=0, pts_seconds=0;
    uint64_t source_index=0, loop=0;
    std::string input;
};
struct Options {
    std::string input,model,report,dump,dump_input,snapshot,dsp_dir,tracks,batch,detections;
    float threshold=.25f;
    double duration=0;
    int repeat=1;
    uint64_t target_id=0;
    bool gui=false,exact=false;
};
struct State {
    std::mutex mutex;
    std::shared_ptr<Frame> latest;
    ai::Follow follow;
    ai::FollowSelection selection;
    std::string error,profile="[]",input,model_sha256;
    std::vector<double> latencies,pipeline_latencies;
    double load_ms=0,seconds=0;
    size_t frames=0,decoded=0,dropped=0;
    int input_side=0;
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
std::vector<float> preprocess(const Frame& frame,int input_side) {
    return ai::preprocess(frame.rgb,frame.width,frame.height,input_side);
}
std::string boxes_json(const std::vector<ai::Box>& boxes) {
    std::ostringstream out;out<<std::setprecision(9)<<"[";
    for(size_t i=0;i<boxes.size();++i) {
        const auto& b=boxes[i];if(i) out<<",";
        out<<"{\"track_id\":"<<b.track_id<<",\"score\":"<<b.score
           <<",\"box\":["<<b.x1<<","<<b.y1<<","<<b.x2<<","<<b.y2<<"],\"keypoints\":[";
        for(size_t k=0;k<17;++k) {if(k) out<<",";const auto& p=b.keypoints[k];out<<"["<<p.x<<","<<p.y<<","<<p.confidence<<"]";}
        out<<"]}";
    }
    out<<"]";return out.str();
}
std::string follow_json(const ai::FollowResult& f) {
    std::ostringstream out;out<<"{\"id\":"<<f.id<<",\"state\":"<<quote(f.state)<<",\"offset\":";
    if(f.offset) out<<"["<<(*f.offset)[0]<<","<<(*f.offset)[1]<<"]";else out<<"null";
    out<<"}";return out.str();
}
std::string sha256(const std::string& path) {
    std::ifstream in(path,std::ios::binary);if(!in) throw std::runtime_error("Cannot open model");
    GChecksum* sum=g_checksum_new(G_CHECKSUM_SHA256);char data[65536];
    while(in) {in.read(data,sizeof(data));g_checksum_update(sum,reinterpret_cast<const guchar*>(data),in.gcount());}
    std::string result=g_checksum_get_string(sum);g_checksum_free(sum);return result;
}

class Video {
    GstElement* pipeline=nullptr;
    GstAppSink* sink=nullptr;
    GstBus* bus=nullptr;
    std::atomic<uint64_t> decoded{0};
    static GQuark index_key() {return g_quark_from_static_string("person-follow-source-index");}
public:
    ~Video() {if(pipeline) {gst_element_set_state(pipeline,GST_STATE_NULL);gst_object_unref(pipeline);} if(sink) gst_object_unref(sink); if(bus) gst_object_unref(bus);}
    uint64_t decoded_count() const {return decoded.load();}
    explicit Video(const std::string& path,bool realtime) {
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
        std::string description="uridecodebin name=source caps="+codec+" ! "+suffix+"parse ! v4l2"+suffix+"dec capture-io-mode=mmap ! video/x-raw,format=NV12 ! videoconvert ! video/x-raw,format=RGB ! appsink name=frames max-buffers=2 "+std::string(realtime?"sync=true drop=true":"sync=false drop=false");
        pipeline=gst_parse_launch(description.c_str(),&error);
        if(error) {std::string message=error->message;g_error_free(error);g_free(uri);if(pipeline) gst_object_unref(pipeline);pipeline=nullptr;throw std::runtime_error(message);}
        auto* source=gst_bin_get_by_name(GST_BIN(pipeline),"source");
        g_object_set(source,"uri",uri,nullptr);g_free(uri);gst_object_unref(source);
        sink=GST_APP_SINK(gst_bin_get_by_name(GST_BIN(pipeline),"frames"));bus=gst_element_get_bus(pipeline);
        auto* pad=gst_element_get_static_pad(GST_ELEMENT(sink),"sink");
        gst_pad_add_probe(pad,GST_PAD_PROBE_TYPE_BUFFER,+[](GstPad*,GstPadProbeInfo* info,gpointer user)->GstPadProbeReturn {
            auto* self=static_cast<Video*>(user);auto* b=GST_PAD_PROBE_INFO_BUFFER(info);
            gst_mini_object_set_qdata(GST_MINI_OBJECT(b),index_key(),reinterpret_cast<gpointer>(++self->decoded),nullptr);
            return GST_PAD_PROBE_OK;
        },this,nullptr);gst_object_unref(pad);
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
        auto* buffer=gst_sample_get_buffer(sample);
        frame.source_index=reinterpret_cast<uintptr_t>(gst_mini_object_get_qdata(GST_MINI_OBJECT(buffer),index_key()))-1;
        if(!GST_BUFFER_PTS_IS_VALID(buffer)) {gst_video_frame_unmap(&mapped);gst_sample_unref(sample);throw std::runtime_error("Video frame has no timestamp");}
        frame.pts_seconds=double(GST_BUFFER_PTS(buffer))/GST_SECOND;
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
        if(opt.batch.empty()&&!std::filesystem::is_regular_file(opt.input)) throw std::runtime_error("Input file does not exist: "+opt.input);
        std::vector<std::string> batch;
        if(!opt.batch.empty()) {
            std::ifstream list(opt.batch);if(!list) throw std::runtime_error("Cannot read batch list");
            std::string line;while(std::getline(list,line)) {
                if(!line.empty()&&line.back()=='\r') line.pop_back();
                if(!line.empty()) batch.push_back((std::filesystem::path(opt.batch).parent_path()/line).string());
            }
            if(batch.empty()) throw std::runtime_error("Batch list is empty");
        }
        std::ofstream tracks;
        if(!opt.tracks.empty()) {tracks.open(opt.tracks);if(!tracks) throw std::runtime_error("Cannot open tracks JSONL");}
        std::ofstream detections;
        if(!opt.detections.empty()) {detections.open(opt.detections);if(!detections) throw std::runtime_error("Cannot open detections JSONL");}
        auto model_start=Clock::now();state.model_sha256=sha256(opt.model);ai::Qnn qnn(opt.model);
        auto tracker=std::make_unique<ai::Tracker>();
        {std::lock_guard<std::mutex> lock(state.mutex);state.follow.select(opt.target_id);}
        {std::lock_guard<std::mutex> lock(state.mutex);state.load_ms=elapsed(model_start)*1000;state.input_side=qnn.input_side();}
        GError* error=nullptr; auto* image=opt.batch.empty()?gdk_pixbuf_new_from_file(opt.input.c_str(),&error):nullptr;
        if(error) g_error_free(error);
        Frame still;
        if(image) {still=from_pixbuf(image);g_object_unref(image);}
        std::unique_ptr<Video> video;
        if(still.rgb.empty()&&batch.empty()) video=std::make_unique<Video>(opt.input,opt.gui&&!opt.exact);
        start=Clock::now();auto last_frame=start;
        size_t count=0,loop=0,last_source=0,loop_processed=0,decoded_previous=0,dropped_count=0;
        while(!state.stop && !interrupted) {
            if(opt.duration>0 && elapsed(start)>=opt.duration) break;
            Frame frame;auto pipeline_start=Clock::now();
            if(!batch.empty()) {
                if(count>=batch.size()) break;
                auto* im=gdk_pixbuf_new_from_file(batch[count].c_str(),&error);
                if(!im) {std::string text=error?error->message:"Cannot read batch image";g_clear_error(&error);throw std::runtime_error(text);}
                frame=from_pixbuf(im);g_object_unref(im);frame.input=batch[count];frame.source_index=count;
            } else if(video) {
                auto status=video->next(frame);
                if(status==Video::Read::Wait) {
                    if(elapsed(last_frame)>15) throw std::runtime_error("Video produced no frame for 15 seconds");
                    continue;
                }
                if(status==Video::Read::End) {
                    if(!count) throw std::runtime_error("Video contains no decodable frames");
                    if(opt.duration<=0) break;
                    decoded_previous+=video->decoded_count();video=std::make_unique<Video>(opt.input,opt.gui&&!opt.exact);tracker=std::make_unique<ai::Tracker>();++loop;loop_processed=0;
                    {std::lock_guard<std::mutex> lock(state.mutex);state.follow.select(0);}
                    continue;
                }
            } else {
                if(opt.duration<=0 && count>=size_t(opt.repeat)) break;
                frame=still;frame.source_index=count;
            }
            frame.loop=loop;if(frame.input.empty()) frame.input=opt.input;
            if(video) {
                dropped_count+=loop_processed?frame.source_index-last_source-1:frame.source_index;
                last_source=frame.source_index;++loop_processed;
            }
            last_frame=Clock::now();
            auto input=preprocess(frame,qnn.input_side());
            if(count==0 && !opt.dump_input.empty()) {
                std::ofstream dump(opt.dump_input,std::ios::binary);dump.write(reinterpret_cast<const char*>(input.data()),input.size()*sizeof(float));
                if(!dump) throw std::runtime_error("Cannot write input dump");
            }
            auto inference=Clock::now();auto output=qnn.execute(input);
            frame.inference_ms=elapsed(inference)*1000;
            frame.boxes=ai::decode(output,qnn.channels_last(),ai::Letterbox(frame.width,frame.height,qnn.input_side()),video?.1f:state.threshold.load());
            if(detections.is_open()) {
                detections<<std::setprecision(9)<<"{\"frame\":"<<count<<",\"source_frame\":"<<frame.source_index
                          <<",\"loop\":"<<loop<<",\"pts_seconds\":"<<frame.pts_seconds<<",\"input\":"<<quote(frame.input)
                          <<",\"width\":"<<frame.width<<",\"height\":"<<frame.height
                          <<",\"model_sha256\":"<<quote(state.model_sha256)<<",\"persons\":"<<boxes_json(frame.boxes)<<"}\n";
                if(!detections) throw std::runtime_error("Cannot write detections JSONL");
            }
            if(video) frame.boxes=tracker->update(frame.boxes,frame.source_index);
            {std::lock_guard<std::mutex> lock(state.mutex);
                state.selection.apply(state.follow,frame.loop);
                frame.follow=state.follow.update(frame.boxes,frame.pts_seconds,frame.width,frame.height);
            }
            frame.pipeline_ms=elapsed(pipeline_start)*1000;
            if(tracks.is_open()) {
                tracks<<std::setprecision(9)<<"{\"frame\":"<<count<<",\"source_frame\":"<<frame.source_index
                      <<",\"loop\":"<<loop<<",\"pts_seconds\":"<<frame.pts_seconds<<",\"input\":"<<quote(frame.input)
                      <<",\"width\":"<<frame.width<<",\"height\":"<<frame.height<<",\"inference_ms\":"<<frame.inference_ms
                      <<",\"pipeline_ms\":"<<frame.pipeline_ms<<",\"target\":"<<follow_json(frame.follow)<<",\"persons\":"<<boxes_json(frame.boxes)<<"}\n";
                if(!tracks) throw std::runtime_error("Cannot write tracks JSONL");
            }
            if(count==0 && !opt.dump.empty()) {
                std::ofstream dump(opt.dump,std::ios::binary);dump.write(reinterpret_cast<const char*>(output.data()),output.size()*sizeof(float));
                if(!dump) throw std::runtime_error("Cannot write tensor dump");
            }
            auto profile=qnn.profile_json();++count;
            {std::lock_guard<std::mutex> lock(state.mutex);
                state.latencies.push_back(frame.inference_ms);state.pipeline_latencies.push_back(frame.pipeline_ms);state.dropped=dropped_count;state.decoded=video?decoded_previous+video->decoded_count():count;state.frames=count;state.seconds=elapsed(start);
                state.latest=std::make_shared<Frame>(std::move(frame));state.profile=std::move(profile);
            }
        }
        if(tracks.is_open()) {tracks.close();if(!tracks) throw std::runtime_error("Cannot flush tracks JSONL");}
        if(detections.is_open()) {detections.close();if(!detections) throw std::runtime_error("Cannot flush detections JSONL");}
        if(count==0 && !state.stop && !interrupted) throw std::runtime_error("No frames processed");
    } catch(const std::exception& e) {std::lock_guard<std::mutex> lock(state.mutex);state.error=e.what();}
    state.done=true;
}
void draw_frame(cairo_t* cr,const Frame& frame,double width,double height,float threshold=.1f) {
    cairo_set_source_rgb(cr,.035,.055,.085);cairo_paint(cr);
    double scale=std::min(width/frame.width,height/frame.height);
    cairo_save(cr);cairo_translate(cr,(width-frame.width*scale)/2,(height-frame.height*scale)/2);cairo_scale(cr,scale,scale);
    auto* image=gdk_pixbuf_new_from_data(frame.rgb.data(),GDK_COLORSPACE_RGB,FALSE,8,frame.width,frame.height,frame.width*3,nullptr,nullptr);
    gdk_cairo_set_source_pixbuf(cr,image,0,0);cairo_paint(cr);g_object_unref(image);
    std::vector<std::array<double,4>> labels;
    for(auto b:frame.boxes) {
        if(b.score<threshold && !(frame.follow.offset && b.track_id==frame.follow.id)) continue;
        static constexpr double colors[][3]={{.15,.95,.65},{.25,.8,1},{1,.75,.25},{1,.45,.65},{.7,.6,1},{.9,.95,.3}};
        const auto& color=colors[b.track_id%6];
        bool selected=frame.follow.offset && b.track_id==frame.follow.id;
        if(selected) cairo_set_source_rgb(cr,1,.85,.1);else cairo_set_source_rgb(cr,color[0],color[1],color[2]);
        cairo_set_line_width(cr,(selected?5:2)/scale);cairo_rectangle(cr,b.x1,b.y1,b.x2-b.x1,b.y2-b.y1);cairo_stroke(cr);
        if(!b.trail.empty()) {cairo_move_to(cr,b.trail[0].x,b.trail[0].y);for(const auto& p:b.trail) cairo_line_to(cr,p.x,p.y);cairo_stroke(cr);}
        std::ostringstream text;if(b.track_id) text<<"ID "<<b.track_id;else text<<"person";text<<"  "<<int(b.score*100)<<"%";
        cairo_set_font_size(cr,15/scale);cairo_text_extents_t ext;cairo_text_extents(cr,text.str().c_str(),&ext);
        double label_w=ext.width+10/scale,label_h=22/scale;
        double x=std::clamp(double(b.x1),0.,std::max(0.,frame.width-label_w));
        double y=std::max(0.,double(b.y1)-label_h);
        for(int attempt=0;attempt<10;++attempt) {
            bool overlaps=false;
            for(const auto& placed:labels) if(x<placed[0]+placed[2]&&x+label_w>placed[0]&&y<placed[1]+placed[3]&&y+label_h>placed[1]) {overlaps=true;break;}
            if(!overlaps) break;
            y=std::max(0.,double(b.y1)-label_h*(attempt+2));
            if(y==0) y=std::min(double(frame.height)-label_h,double(b.y1)+label_h*(attempt+1));
        }
        labels.push_back({x,y,label_w,label_h});
        cairo_rectangle(cr,x,y,label_w,label_h);cairo_fill(cr);
        cairo_set_source_rgb(cr,.015,.08,.06);cairo_move_to(cr,x+5/scale,y+label_h-6/scale);cairo_show_text(cr,text.str().c_str());

    }
    cairo_set_source_rgba(cr,1,1,1,.8);cairo_set_line_width(cr,1/scale);
    double cx=frame.width/2.,cy=frame.height/2.;
    cairo_move_to(cr,cx-10/scale,cy);cairo_line_to(cr,cx+10/scale,cy);
    cairo_move_to(cr,cx,cy-10/scale);cairo_line_to(cr,cx,cy+10/scale);cairo_stroke(cr);
    if(frame.follow.offset) {
        cairo_set_source_rgb(cr,1,.85,.1);cairo_set_line_width(cr,2/scale);
        cairo_move_to(cr,cx,cy);cairo_line_to(cr,cx*(1+(*frame.follow.offset)[0]),cy*(1+(*frame.follow.offset)[1]));cairo_stroke(cr);
    }
    cairo_restore(cr);
}
void report(const Options& opt,State& state) {
    std::lock_guard<std::mutex> lock(state.mutex);
    auto sorted=state.latencies;std::sort(sorted.begin(),sorted.end());
    auto pipe=state.pipeline_latencies;std::sort(pipe.begin(),pipe.end());
    auto pipe_percentile=[&](double p){return pipe.empty()?0:pipe[size_t(std::ceil(p*pipe.size()))-1];};
    auto percentile=[&](double p){return sorted.empty()?0:sorted[size_t(std::ceil(p*sorted.size()))-1];};
    struct rusage usage{};getrusage(RUSAGE_SELF,&usage);
    std::ostringstream out;out<<std::setprecision(9)<<"{\"schema_version\":1,\"status\":"<<quote(state.error.empty()?(state.stop||interrupted?"CANCELLED":"PASS"):"FAIL")
        <<",\"tracker\":{\"name\":\"ByteTrack\",\"revision\":2,\"motion_prediction\":true,\"fuse_score\":true,\"lost_source_frames\":30,\"high\":0.25,\"new\":0.35,\"match\":0.8}"
        <<",\"target\":"<<(state.latest?follow_json(state.latest->follow):"null")
        <<",\"error\":"<<quote(state.error)<<",\"backend\":\"QNN_HTP\",\"input\":"<<quote(opt.input)
        <<",\"model_sha256\":"<<quote(state.model_sha256)<<",\"decoded_frames\":"<<state.decoded<<",\"dropped_frames\":"<<state.dropped
        <<",\"unprocessed_decoded_frames\":"<<(state.decoded-state.frames)<<",\"pipeline_p50_ms\":"<<pipe_percentile(.5)<<",\"pipeline_p95_ms\":"<<pipe_percentile(.95)
        <<",\"model\":"<<quote(opt.model)<<",\"input_side\":"<<state.input_side<<",\"frames\":"<<state.frames<<",\"load_ms\":"<<state.load_ms
        <<",\"elapsed_seconds\":"<<state.seconds<<",\"fps\":"<<(state.seconds>0?state.frames/state.seconds:0)
        <<",\"inference_p50_ms\":"<<percentile(.5)<<",\"inference_p95_ms\":"<<percentile(.95)
        <<",\"peak_rss_kib\":"<<usage.ru_maxrss<<",\"timing_definition\":\"QNN execute including I/O quantization and dequantization; excludes image preprocessing and NMS\",\"detections\":[";
    std::string rendered=out.str();rendered.pop_back();
    out.str("");out.clear();out<<rendered<<(state.latest?boxes_json(state.latest->boxes):"[]")<<",\"qnn_profile\":"<<state.profile<<"}\n";
    if(!opt.report.empty()) {
        std::ofstream file(opt.report+".tmp");file<<out.str();file.close();
        if(!file) throw std::runtime_error("Cannot write report: "+opt.report);
        std::filesystem::rename(opt.report+".tmp",opt.report);
    }
    if(!opt.snapshot.empty() && state.latest) {
        auto& f=*state.latest;auto* surface=cairo_image_surface_create(CAIRO_FORMAT_RGB24,f.width,f.height);auto* cr=cairo_create(surface);
        draw_frame(cr,f,f.width,f.height,opt.threshold);cairo_destroy(cr);auto status=cairo_surface_write_to_png(surface,opt.snapshot.c_str());cairo_surface_destroy(surface);
        if(status!=CAIRO_STATUS_SUCCESS) throw std::runtime_error("Cannot write snapshot");
    }
    std::cout<<out.str();
}
struct Ui {
    Options opt;State state;std::thread worker;
    GtkWidget *window=nullptr,*canvas=nullptr,*status=nullptr,*file_label=nullptr,*start=nullptr,*open=nullptr,*demo_video=nullptr;
    bool reported=false;
    explicit Ui(Options options):opt(std::move(options)) {state.threshold=opt.threshold;}
    void stop() {state.stop=true;if(worker.joinable()) worker.join();}
    ~Ui(){stop();}
    void begin() {
        if(worker.joinable()) worker.join();
        {std::lock_guard<std::mutex> lock(state.mutex);state.latest.reset();state.follow.select(0);state.selection.reset();state.error.clear();state.profile="[]";state.frames=0;state.latencies.clear();state.pipeline_latencies.clear();state.decoded=0;state.dropped=0;state.seconds=0;state.load_ms=0;state.input_side=0;state.model_sha256.clear();}
        state.done=false;state.stop=false;reported=false;
        gtk_label_set_text(GTK_LABEL(file_label),std::filesystem::path(opt.input).filename().c_str());
        gtk_label_set_text(GTK_LABEL(status),"正在加载模型…");
        gtk_widget_set_sensitive(start,FALSE);gtk_widget_set_sensitive(open,FALSE);gtk_widget_set_sensitive(demo_video,FALSE);
        worker=std::thread(run,opt,std::ref(state));
    }
    void create() {
        window=gtk_window_new(GTK_WINDOW_TOPLEVEL);gtk_window_set_title(GTK_WINDOW(window),"Q6A 人物跟随");gtk_window_set_default_size(GTK_WINDOW(window),960,540);
        auto* header=gtk_header_bar_new();gtk_header_bar_set_title(GTK_HEADER_BAR(header),"人物运动跟踪");gtk_header_bar_set_show_close_button(GTK_HEADER_BAR(header),TRUE);gtk_window_set_titlebar(GTK_WINDOW(window),header);
        auto* css=gtk_css_provider_new();gtk_css_provider_load_from_data(css,
            "window { background: #101b2b; color: #e6edf5; } headerbar { background: #152238; color: #e6edf5; } button { background-image: none; background-color: #23374e; border: 1px solid #425873; padding: 7px 12px; color: #e6edf5; } button:hover { background-color: #304b65; } button:disabled { background-color: #172639; color: #8190a3; } button label { color: inherit; } label { color: #e6edf5; } .heading { font-size: 22px; font-weight: bold; } .muted { color: #9aafc5; }",-1,nullptr);
        gtk_style_context_add_provider_for_screen(gdk_screen_get_default(),GTK_STYLE_PROVIDER(css),GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);g_object_unref(css);
        auto* root=gtk_box_new(GTK_ORIENTATION_VERTICAL,8);gtk_container_set_border_width(GTK_CONTAINER(root),16);gtk_container_add(GTK_CONTAINER(window),root);
        auto* title=gtk_label_new("人物运动跟踪");gtk_widget_set_halign(title,GTK_ALIGN_START);gtk_style_context_add_class(gtk_widget_get_style_context(title),"heading");gtk_box_pack_start(GTK_BOX(root),title,FALSE,FALSE,0);
        auto* subtitle=gtk_label_new("多人轨迹 · 点击锁定人物 · 遮挡超过 1 秒后重新选择");gtk_widget_set_halign(subtitle,GTK_ALIGN_START);gtk_style_context_add_class(gtk_widget_get_style_context(subtitle),"muted");gtk_box_pack_start(GTK_BOX(root),subtitle,FALSE,FALSE,0);
        auto* controls=gtk_box_new(GTK_ORIENTATION_HORIZONTAL,8);gtk_box_pack_start(GTK_BOX(root),controls,FALSE,FALSE,0);
        demo_video=gtk_button_new_with_label("示例视频");open=gtk_button_new_with_label("打开视频…");start=gtk_button_new_with_label("开始 / 重播");auto* halt=gtk_button_new_with_label("停止");auto* clear=gtk_button_new_with_label("取消锁定");
        for(auto* w:{demo_video,open,start,halt,clear}) gtk_box_pack_start(GTK_BOX(controls),w,FALSE,FALSE,0);
        auto* threshold=gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL,.05,.95,.05);gtk_range_set_value(GTK_RANGE(threshold),opt.threshold);gtk_widget_set_size_request(threshold,170,-1);
        gtk_box_pack_end(GTK_BOX(controls),threshold,FALSE,FALSE,0);gtk_box_pack_end(GTK_BOX(controls),gtk_label_new("显示阈值"),FALSE,FALSE,0);
        file_label=gtk_label_new("选择视频后，点击人物进行锁定");gtk_widget_set_halign(file_label,GTK_ALIGN_START);gtk_box_pack_start(GTK_BOX(root),file_label,FALSE,FALSE,0);
        canvas=gtk_drawing_area_new();gtk_widget_set_size_request(canvas,480,180);gtk_box_pack_start(GTK_BOX(root),canvas,TRUE,TRUE,0);
        status=gtk_label_new("就绪 · 点击人物锁定目标");gtk_widget_set_halign(status,GTK_ALIGN_START);gtk_label_set_line_wrap(GTK_LABEL(status),TRUE);gtk_box_pack_start(GTK_BOX(root),status,FALSE,FALSE,0);
        g_signal_connect(window,"destroy",G_CALLBACK(+[](GtkWidget*,gpointer p){auto& u=*static_cast<Ui*>(p);u.stop();gtk_main_quit();}),this);
        g_signal_connect(canvas,"draw",G_CALLBACK(+[](GtkWidget* w,cairo_t* cr,gpointer p)->gboolean {
            auto& s=static_cast<Ui*>(p)->state;std::lock_guard<std::mutex> lock(s.mutex);
            if(s.latest) draw_frame(cr,*s.latest,gtk_widget_get_allocated_width(w),gtk_widget_get_allocated_height(w),s.threshold.load());
            else {cairo_set_source_rgb(cr,.035,.055,.085);cairo_paint(cr);}
            return TRUE;
        }),this);
        g_signal_connect(start,"clicked",G_CALLBACK(+[](GtkButton*,gpointer p){static_cast<Ui*>(p)->begin();}),this);
        g_signal_connect(halt,"clicked",G_CALLBACK(+[](GtkButton*,gpointer p){static_cast<Ui*>(p)->state.stop=true;}),this);
        g_signal_connect(threshold,"value-changed",G_CALLBACK(+[](GtkRange* w,gpointer p){static_cast<Ui*>(p)->state.threshold=float(gtk_range_get_value(w));}),this);
        g_signal_connect(demo_video,"clicked",G_CALLBACK(+[](GtkButton*,gpointer p){auto& u=*static_cast<Ui*>(p);u.opt.input=(std::filesystem::path(u.opt.model).parent_path().parent_path()/"data/sample.mp4").string();u.begin();}),this);
        g_signal_connect(open,"clicked",G_CALLBACK(+[](GtkButton*,gpointer p){
            auto& u=*static_cast<Ui*>(p);auto* dialog=gtk_file_chooser_dialog_new("打开视频",GTK_WINDOW(u.window),GTK_FILE_CHOOSER_ACTION_OPEN,"取消",GTK_RESPONSE_CANCEL,"打开",GTK_RESPONSE_ACCEPT,nullptr);
            if(gtk_dialog_run(GTK_DIALOG(dialog))==GTK_RESPONSE_ACCEPT) {char* path=gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));u.opt.input=path;g_free(path);gtk_widget_destroy(dialog);u.begin();}
            else gtk_widget_destroy(dialog);
        }),this);
        gtk_widget_add_events(canvas,GDK_BUTTON_PRESS_MASK);
        g_signal_connect(canvas,"button-press-event",G_CALLBACK(+[](GtkWidget* w,GdkEventButton* e,gpointer p)->gboolean {
            if(e->button!=1) return FALSE;
            auto& s=static_cast<Ui*>(p)->state;std::lock_guard<std::mutex> lock(s.mutex);
            if(!s.latest) return FALSE;
            auto& f=*s.latest;
            auto id=ai::hit_test(f.boxes,f.width,f.height,gtk_widget_get_allocated_width(w),gtk_widget_get_allocated_height(w),e->x,e->y,s.threshold.load());
            if(id) {s.selection.queue(id,f.loop,f.pts_seconds);ai::Follow preview;preview.select(id);f.follow=preview.update(f.boxes,f.pts_seconds,f.width,f.height);gtk_widget_queue_draw(w);}
            return TRUE;
        }),this);
        g_signal_connect(clear,"clicked",G_CALLBACK(+[](GtkButton*,gpointer p){
            auto& u=*static_cast<Ui*>(p);std::lock_guard<std::mutex> lock(u.state.mutex);
            if(u.state.latest) {u.state.selection.queue(0,u.state.latest->loop);u.state.latest->follow={};}gtk_widget_queue_draw(u.canvas);
        }),this);
        gtk_widget_show_all(window);gtk_window_maximize(GTK_WINDOW(window));
        g_timeout_add(100,+[](gpointer p)->gboolean {
            auto& u=*static_cast<Ui*>(p);
            {std::lock_guard<std::mutex> lock(u.state.mutex);
                if(!u.state.error.empty()) gtk_label_set_text(GTK_LABEL(u.status),("Error: "+u.state.error).c_str());
                else if(u.state.latest) {
                    std::ostringstream text;text<<"NPU  ·  "<<u.state.latest->boxes.size()<<" 人  ·  "<<std::fixed<<std::setprecision(1)<<u.state.latest->inference_ms<<" ms  ·  "<<(u.state.seconds>0?u.state.frames/u.state.seconds:0)<<" FPS  ·  "<<u.state.frames<<" 帧  ·  "<<u.state.dropped<<" 丢帧"<<(u.state.done?"  ·  已结束":"");
                    text<<"  ·  "<<ai::follow_label(u.state.latest->follow);
                    if(u.state.latest->follow.id) text<<" ID "<<u.state.latest->follow.id;
                    if(u.state.latest->follow.offset) text<<"  偏差 "<<(*u.state.latest->follow.offset)[0]<<", "<<(*u.state.latest->follow.offset)[1];
                    gtk_label_set_text(GTK_LABEL(u.status),text.str().c_str());gtk_widget_queue_draw(u.canvas);
                }
            }
            if(u.state.done && !u.reported) {
                if(u.worker.joinable()) u.worker.join();
                try {report(u.opt,u.state);} catch(const std::exception& e){gtk_label_set_text(GTK_LABEL(u.status),e.what());}
                u.reported=true;gtk_widget_set_sensitive(u.start,TRUE);gtk_widget_set_sensitive(u.open,TRUE);gtk_widget_set_sensitive(u.demo_video,TRUE);
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
    ai::Letterbox shape(1280,720);auto b=shape.restore(ai::Box{0,91,416,325,.9,0});
    if(b.x2!=1280 || b.y1!=0 || b.y2!=720 || ai::quantize(.5f,.01f,-10,255)!=60) throw std::runtime_error("Core self-test failed");
    std::cout<<"Person follow hardware-independent self-test PASS\n";
}
}
int main(int argc,char** argv) {
    Options opt;
    try {
        std::filesystem::path executable=std::filesystem::canonical("/proc/self/exe");
        opt.model=(executable.parent_path().parent_path()/"share/person-follow/models/pose.bin").string();
        for(int i=1;i<argc;++i) {
            std::string key=argv[i];
            if(key=="--self-test") {self_test();return 0;}
            if(key=="--exact") {opt.exact=true;continue;}
            if(key=="--gui") {opt.gui=true;continue;}
            if(key=="--help") {std::cout<<"person-follow [--gui] --input FILE [--model CONTEXT.bin] [--report JSON] [--target-id ID] [--tracks JSONL] [--detections JSONL] [--batch LIST] [--exact] [--snapshot PNG] [--dump-output RAW] [--confidence 0.25] [--duration SECONDS] [--repeat COUNT]\n";return 0;}
            if(i+1>=argc) throw std::runtime_error("Missing value for "+key);
            std::string value=argv[++i];
            if(key=="--input") opt.input=value;else if(key=="--model") opt.model=value;
            else if(key=="--report") opt.report=value;else if(key=="--snapshot") opt.snapshot=value;
            else if(key=="--dump-output") opt.dump=value;
            else if(key=="--dump-input") opt.dump_input=value;
            else if(key=="--target-id") {
                if(value.empty()||value.find_first_not_of("0123456789")!=std::string::npos) throw std::runtime_error("Target ID must be a positive integer");
                opt.target_id=std::stoull(value);if(!opt.target_id) throw std::runtime_error("Target ID must be positive");
            }
            else if(key=="--tracks") opt.tracks=value;
            else if(key=="--detections") opt.detections=value;
            else if(key=="--batch") opt.batch=value;
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
        for(auto* path:{&opt.input,&opt.model,&opt.report,&opt.dump,&opt.dump_input,&opt.snapshot,&opt.dsp_dir,&opt.tracks,&opt.batch,&opt.detections})
            if(!path->empty()) *path=std::filesystem::absolute(*path).string();
        if(!opt.dsp_dir.empty()) std::filesystem::current_path(opt.dsp_dir);
        std::signal(SIGINT,signal_handler);std::signal(SIGTERM,signal_handler);gst_init(nullptr,nullptr);
        if(opt.gui&&!opt.batch.empty()) throw std::runtime_error("--batch is CLI only");
        if(opt.gui) {
            g_setenv("GDK_GL","gles",TRUE);
            if(!gtk_init_check(nullptr,nullptr)) throw std::runtime_error("Cannot connect to desktop; set XDG_RUNTIME_DIR and WAYLAND_DISPLAY");
            Ui ui(opt);ui.create();return ui.state.error.empty()?0:1;
        }
        if(opt.input.empty()&&opt.batch.empty()) throw std::runtime_error("Use --input FILE or --gui");
        State state;state.threshold=opt.threshold;run(opt,state);report(opt,state);return state.error.empty()?(interrupted?130:0):1;
    } catch(const std::exception& e) {std::cerr<<"person-follow: "<<e.what()<<"\n";return 1;}
}
