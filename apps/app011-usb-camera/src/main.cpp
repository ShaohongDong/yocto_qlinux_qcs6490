// SPDX-License-Identifier: MIT
#include "devices.hpp"
#include <gtk/gtk.h>
#include <gst/app/gstappsink.h>
#include <gst/video/video.h>
#include <chrono>
#include <iostream>
#include <cstring>
#include <cmath>
#include <stdexcept>
using Clock=std::chrono::steady_clock;
struct Preview {
    GstElement* pipeline=nullptr;
    GstAppSink* sink=nullptr;
    GstBus* bus=nullptr;
    GdkPixbuf* pixels=nullptr;
    std::vector<camera::Device> devices;
    GtkWidget *window=nullptr,*canvas=nullptr,*device=nullptr,*mode=nullptr,*start=nullptr,*refresh=nullptr,*status=nullptr;
    guint timer=0;
    bool fullscreen=false,headless=false,failed=false;
    unsigned long frames=0,displayed=0,previous=0;
    Clock::time_point began,last,stats;
    double duration=0;
    ~Preview(){stop();}
    void message(const std::string& text) {
        std::cout<<text<<std::endl;
        if(status) gtk_label_set_text(GTK_LABEL(status),text.c_str());
    }
    void controls(bool running) {
        if(!window) return;
        gtk_widget_set_sensitive(device,!running); gtk_widget_set_sensitive(mode,!running);
        gtk_widget_set_sensitive(refresh,!running);
        gtk_button_set_label(GTK_BUTTON(start),running?"停止":"开始");
    }
    void stop() {
        if(timer) {g_source_remove(timer); timer=0;}
        if(pipeline) {gst_element_set_state(pipeline,GST_STATE_NULL);gst_object_unref(pipeline);pipeline=nullptr;}
        if(sink) {gst_object_unref(sink);sink=nullptr;}
        if(bus) {gst_object_unref(bus);bus=nullptr;}
        if(pixels) {g_object_unref(pixels);pixels=nullptr;}
        controls(false);
        if(canvas) gtk_widget_queue_draw(canvas);
    }
    void error(const std::string& text) {failed=true;stop();message("错误："+text);}
    bool play(const std::string& path,const camera::Mode& m, bool synthetic=false) {
        stop(); failed=false; frames=displayed=previous=0;
        std::string caps=(m.format=="MJPG"?"image/jpeg":"video/x-raw,format="+std::string(m.format=="YUYV"?"YUY2":"NV12"));
        caps+=",width="+std::to_string(m.width)+",height="+std::to_string(m.height)+",framerate="+std::to_string(m.denominator)+"/"+std::to_string(m.numerator);
        std::string description=synthetic?"videotestsrc is-live=true ! video/x-raw,width=640,height=480,framerate=30/1":
            "v4l2src name=camera ! "+caps+(m.format=="MJPG"?" ! jpegdec":"");
        description+=" ! videoconvert ! video/x-raw,format=RGB ! appsink name=frames max-buffers=1 drop=true sync=false";
        GError* e=nullptr; pipeline=gst_parse_launch(description.c_str(),&e);
        if(e) {std::string s=e->message;g_error_free(e);error(s);return false;}
        if(!pipeline) {error("无法创建采集管线");return false;}
        if(!synthetic) {auto* source=gst_bin_get_by_name(GST_BIN(pipeline),"camera");g_object_set(source,"device",path.c_str(),nullptr);gst_object_unref(source);}
        sink=GST_APP_SINK(gst_bin_get_by_name(GST_BIN(pipeline),"frames")); bus=gst_element_get_bus(pipeline);
        began=last=stats=Clock::now(); controls(true);
        if(gst_element_set_state(pipeline,GST_STATE_PLAYING)==GST_STATE_CHANGE_FAILURE) {error("启动失败，请检查设备占用、权限及所选模式");return false;}
        message("正在启动："+path+" / "+m.label());
        return true;
    }
    bool tick() {
        if(!pipeline) return false;
        GstMessage* msg=gst_bus_pop_filtered(bus,static_cast<GstMessageType>(GST_MESSAGE_ERROR|GST_MESSAGE_EOS));
        if(msg) {
            std::string text="采集意外结束";
            if(GST_MESSAGE_TYPE(msg)==GST_MESSAGE_ERROR) {GError* e=nullptr;gchar* debug=nullptr;gst_message_parse_error(msg,&e,&debug);text=e->message;g_error_free(e);g_free(debug);}
            gst_message_unref(msg);error(text);return false;
        }
        auto* sample=gst_app_sink_try_pull_sample(sink,0);
        if(sample) {
            GstVideoInfo info;gst_video_info_init(&info);GstVideoFrame frame;
            if(!gst_video_info_from_caps(&info,gst_sample_get_caps(sample)) ||
                !gst_video_frame_map(&frame,&info,gst_sample_get_buffer(sample),GST_MAP_READ)) {
                gst_sample_unref(sample);error("无法读取视频帧");return false;
            }
            int w=GST_VIDEO_INFO_WIDTH(&info),h=GST_VIDEO_INFO_HEIGHT(&info);
            if(!headless) {
                if(!pixels || gdk_pixbuf_get_width(pixels)!=w || gdk_pixbuf_get_height(pixels)!=h) {
                    if(pixels) g_object_unref(pixels);
                    pixels=gdk_pixbuf_new(GDK_COLORSPACE_RGB,false,8,w,h);
                }
                for(int y=0;y<h;++y) std::memcpy(gdk_pixbuf_get_pixels(pixels)+y*gdk_pixbuf_get_rowstride(pixels),
                    static_cast<unsigned char*>(GST_VIDEO_FRAME_PLANE_DATA(&frame,0))+y*GST_VIDEO_FRAME_PLANE_STRIDE(&frame,0),w*3);
                gtk_widget_queue_draw(canvas);
            }
            gst_video_frame_unmap(&frame);gst_sample_unref(sample);++frames;last=Clock::now();
        }
        auto now=Clock::now();
        if(std::chrono::duration<double>(now-last).count()>5) {error("5 秒未收到画面，请检查摄像头连接");return false;}
        double interval=std::chrono::duration<double>(now-stats).count();
        if(interval>=1) {
            message("预览中  接收 "+std::to_string((frames-previous)/interval)+" fps  显示 "+std::to_string(displayed/interval)+" fps  累计 "+std::to_string(frames)+" 帧");
            previous=frames;displayed=0;stats=now;
        }
        if(duration>0 && std::chrono::duration<double>(now-began).count()>=duration) {
            message("完成：frames="+std::to_string(frames));stop();if(window) gtk_main_quit();return false;
        }
        return true;
    }
    void modes() {
        gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(mode));
        int i=gtk_combo_box_get_active(GTK_COMBO_BOX(device));
        if(i<0 || size_t(i)>=devices.size()) return;
        for(auto& m:devices[i].modes) gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(mode),m.label().c_str());
        gtk_combo_box_set_active(GTK_COMBO_BOX(mode),0);
    }
    void scan() {
        stop();devices.clear();gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(device));
        try {devices=camera::discover();} catch(const std::exception& e) {message(std::string("无法枚举摄像头：")+e.what());return;}
        for(auto& d:devices) gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(device),(d.name+" ("+d.path+")").c_str());
        gtk_combo_box_set_active(GTK_COMBO_BOX(device),devices.empty()?-1:0);
        message(devices.empty()?"未找到可用 USB 摄像头，请连接后刷新，并检查设备权限":"请选择模式，点击开始");
    }
    void toggle() {
        if(pipeline) {stop();message("已停止");return;}
        int i=gtk_combo_box_get_active(GTK_COMBO_BOX(device)),j=gtk_combo_box_get_active(GTK_COMBO_BOX(mode));
        if(i<0||j<0) {message("请先选择摄像头和模式");return;}
        if(play(devices.at(i).path,devices.at(i).modes.at(j)))
            timer=g_timeout_add(10,+[](gpointer p)->gboolean {auto* self=static_cast<Preview*>(p);return self->tick();},this);
    }
    void gui() {
        window=gtk_window_new(GTK_WINDOW_TOPLEVEL);gtk_window_set_title(GTK_WINDOW(window),"USB 摄像头预览");gtk_window_set_default_size(GTK_WINDOW(window),1100,760);
        auto* box=gtk_box_new(GTK_ORIENTATION_VERTICAL,8);gtk_container_set_border_width(GTK_CONTAINER(box),8);gtk_container_add(GTK_CONTAINER(window),box);
        auto* bar=gtk_box_new(GTK_ORIENTATION_HORIZONTAL,8);gtk_box_pack_start(GTK_BOX(box),bar,false,false,0);
        device=gtk_combo_box_text_new();mode=gtk_combo_box_text_new();refresh=gtk_button_new_with_label("刷新");start=gtk_button_new_with_label("开始");auto* full=gtk_button_new_with_label("全屏");
        for(auto* w:{device,refresh,mode,start,full}) gtk_box_pack_start(GTK_BOX(bar),w,false,false,0);
        canvas=gtk_drawing_area_new();gtk_widget_set_hexpand(canvas,true);gtk_widget_set_vexpand(canvas,true);gtk_box_pack_start(GTK_BOX(box),canvas,true,true,0);
        status=gtk_label_new("");gtk_label_set_line_wrap(GTK_LABEL(status),true);gtk_box_pack_start(GTK_BOX(box),status,false,false,0);
        g_signal_connect(device,"changed",G_CALLBACK((+[](GtkComboBox*,gpointer p){static_cast<Preview*>(p)->modes();})),this);
        g_signal_connect(refresh,"clicked",G_CALLBACK((+[](GtkButton*,gpointer p){static_cast<Preview*>(p)->scan();})),this);
        g_signal_connect(start,"clicked",G_CALLBACK((+[](GtkButton*,gpointer p){static_cast<Preview*>(p)->toggle();})),this);
        g_signal_connect(full,"clicked",G_CALLBACK((+[](GtkButton*,gpointer p){auto* s=static_cast<Preview*>(p);s->fullscreen=!s->fullscreen;if(s->fullscreen) gtk_window_fullscreen(GTK_WINDOW(s->window));else gtk_window_unfullscreen(GTK_WINDOW(s->window));})),this);
        g_signal_connect(window,"key-press-event",G_CALLBACK((+[](GtkWidget*,GdkEventKey* e,gpointer p)->gboolean{auto* s=static_cast<Preview*>(p);if(e->keyval==GDK_KEY_Escape){gtk_window_unfullscreen(GTK_WINDOW(s->window));s->fullscreen=false;return true;}return false;})),this);
        g_signal_connect(window,"delete-event",G_CALLBACK((+[](GtkWidget*,GdkEvent*,gpointer p)->gboolean{static_cast<Preview*>(p)->stop();gtk_main_quit();return true;})),this);
        g_signal_connect(canvas,"draw",G_CALLBACK((+[](GtkWidget* widget,cairo_t* cr,gpointer p)->gboolean {
            auto* s=static_cast<Preview*>(p);cairo_set_source_rgb(cr,0.06,0.06,0.06);cairo_paint(cr);
            if(s->pixels) {double w=gdk_pixbuf_get_width(s->pixels),h=gdk_pixbuf_get_height(s->pixels),cw=gtk_widget_get_allocated_width(widget),ch=gtk_widget_get_allocated_height(widget);double scale=std::min(cw/w,ch/h);cairo_translate(cr,(cw-w*scale)/2,(ch-h*scale)/2);cairo_scale(cr,scale,scale);gdk_cairo_set_source_pixbuf(cr,s->pixels,0,0);cairo_paint(cr);++s->displayed;}return true;
        })),this);
        gtk_widget_show_all(window);scan();if(!devices.empty()) toggle();gtk_main();stop();gtk_widget_destroy(window);
        window=canvas=device=mode=start=refresh=status=nullptr;
    }
};
int main(int argc,char** argv) {
    bool list=false,self=false,headless=false,synthetic=false;double duration=0;std::string path;
    try {
        for(int i=1;i<argc;++i) {
            std::string a=argv[i];
            if(a=="--gui") {} else if(a=="--list-devices") list=true;else if(a=="--self-test") self=true;
            else if(a=="--headless") headless=true;else if(a=="--test-source") synthetic=true;
            else if(a=="--device" && i+1<argc) path=argv[++i];
            else if(a=="--duration" && i+1<argc) {std::string v=argv[++i];size_t n;duration=std::stod(v,&n);if(n!=v.size()||!std::isfinite(duration)||duration<=0) throw std::runtime_error("duration must be positive");}
            else if(a=="--help") {std::cout<<"usb-camera [--gui | --list-devices | --self-test | --headless] [--device PATH] [--duration SECONDS] [--test-source]\n";return 0;}
            else throw std::runtime_error("Unknown or incomplete option: "+a);
        }
        if(self) {
            if(camera::capture(V4L2_CAP_META_CAPTURE|V4L2_CAP_STREAMING,"usb-test") ||
                camera::capture(V4L2_CAP_VIDEO_CAPTURE|V4L2_CAP_STREAMING,"platform:test") ||
                !camera::capture(V4L2_CAP_VIDEO_CAPTURE|V4L2_CAP_STREAMING,"usb-test") ||
                !(camera::rank({"MJPG",1920,1080,1,30})>camera::rank({"YUYV",1920,1080,1,5}))) return 1;
            std::cout<<"USB camera self-test PASS\n";return 0;
        }
        if(list) {for(const auto& d:camera::discover()){std::cout<<d.path<<" "<<d.name<<"\n";for(const auto& m:d.modes)std::cout<<"  "<<m.label()<<"\n";}return 0;}
        gst_init(nullptr,nullptr);Preview p;p.headless=headless;p.duration=duration;
        if(headless) {
            if(duration==0) throw std::runtime_error("--headless requires --duration");
            camera::Mode mode{"MJPG",1920,1080,1,30};
            if(!synthetic && path.empty()) {auto devices=camera::discover();if(devices.empty())throw std::runtime_error("No USB camera");path=devices.front().path;mode=devices.front().modes.front();}
            if(!p.play(path,mode,synthetic))return 1;
            while(p.tick()) g_usleep(1000);
            return p.failed||p.frames==0?1:0;
        }
        if(synthetic || !path.empty()) throw std::runtime_error("--device and --test-source require --headless");
        if(!gtk_init_check(nullptr,nullptr)) throw std::runtime_error("Cannot open desktop display");
        p.gui();return p.failed?1:0;
    } catch(const std::exception& e){std::cerr<<e.what()<<"\n";return 1;}
}
