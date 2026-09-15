// SPDX-License-Identifier: MIT
#include "renderer.hpp"
#include <gtk/gtk.h>
#include <csignal>
#include <fstream>
#include <iostream>
#include <sstream>
#include <unistd.h>

using namespace gpu;
static volatile std::sig_atomic_t interrupted = 0;
static void signal_handler(int) { interrupted = 1; }
static int headless(Options o) {
    Report report; report.options=o; report.started=timestamp();
    // Create and validate the report destination before opening the GPU.
    report.save();
    std::unique_ptr<Context> context;
    std::unique_ptr<Renderer> renderer;
    std::unique_ptr<Session> session;
    try {
        context=std::make_unique<Context>(o.device); report.device=context->device;
        renderer=std::make_unique<Renderer>(o);
        session=std::make_unique<Session>(report,*renderer);
        while(!interrupted && session->step()) {}
        if(interrupted) { session->finish("cancelled"); return 130; }
        std::cout << "PASS: " << o.output << "/summary.json (performance baseline only)\n";
        return 0;
    } catch(const std::exception& e) {
        report.status="failed"; report.error=e.what();
        if(session) session->finish("failed",e.what()); else report.save();
        std::cerr << "GPU benchmark failed: " << e.what() << '\n'; return 1;
    }
}
struct UI {
    Options base;
    GtkWidget *window=nullptr,*area=nullptr,*status=nullptr,*suite=nullptr,*mode=nullptr,*start=nullptr,*stop=nullptr,*duration=nullptr,*path=nullptr,*instances=nullptr;
    std::unique_ptr<Report> report;
    std::unique_ptr<Renderer> renderer;
    std::unique_ptr<Session> session;
    GSubprocess* child=nullptr;
    bool running=false, cancelling=false, closing=false;
    Clock::time_point cancel_start{};
    int result=0;
    std::string executable, destination;
    void label(const std::string& text) { gtk_label_set_text(GTK_LABEL(status),text.c_str()); }
    void finished(int code,const std::string& text) {
        result=code; running=false; cancelling=false;
        gtk_widget_set_sensitive(start,true); gtk_widget_set_sensitive(stop,false);
        gtk_widget_set_sensitive(suite,true); gtk_widget_set_sensitive(mode,true); gtk_widget_set_sensitive(duration,true);
        gtk_widget_set_sensitive(instances,true);
        std::ifstream file(std::filesystem::path(destination)/"summary.txt");
        std::string summary((std::istreambuf_iterator<char>(file)),{});
        label(text + (summary.empty()?"":"\n"+summary));
        if(base.quit_after_run || closing) gtk_main_quit();
    }
    void begin() {
        if(running) return;
        Options o=base;
        int choice=gtk_combo_box_get_active(GTK_COMBO_BOX(suite));o.suite=choice==0?"quick":choice==1?"stability":"model3d";
        o.window=gtk_combo_box_get_active(GTK_COMBO_BOX(mode))==1;
        o.duration=gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(duration));
        o.instances=gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(instances));
        destination=(std::filesystem::path(base.output)/(timestamp()+"-"+std::to_string(g_get_monotonic_time()))).string();
        o.output=destination; report=std::make_unique<Report>(); report->options=o; report->started=timestamp();
        running=true; cancelling=false;
        gtk_widget_set_sensitive(start,false); gtk_widget_set_sensitive(stop,true);
        gtk_widget_set_sensitive(suite,false); gtk_widget_set_sensitive(mode,false); gtk_widget_set_sensitive(duration,false);
        gtk_widget_set_sensitive(instances,false);
        gtk_label_set_text(GTK_LABEL(path),destination.c_str());
        label("Warming up (3 seconds per scene)…");
        gtk_widget_set_visible(area,o.window);
        if(o.window) { gtk_gl_area_queue_render(GTK_GL_AREA(area)); return; }
        std::vector<std::string> args={executable,"--headless","--suite",o.suite,"--duration",std::to_string(o.duration),"--size",
            std::to_string(o.width)+"x"+std::to_string(o.height),"--output",destination};
        if(!o.device.empty()) { args.push_back("--device"); args.push_back(o.device); }
        if(!o.model.empty()) { args.push_back("--model");args.push_back(o.model); }
        args.push_back("--instances");args.push_back(std::to_string(o.instances));
        std::vector<const char*> argv; for(const auto& a:args) argv.push_back(a.c_str()); argv.push_back(nullptr);
        GError* error=nullptr;
        child=g_subprocess_newv(argv.data(),static_cast<GSubprocessFlags>(G_SUBPROCESS_FLAGS_STDOUT_PIPE|G_SUBPROCESS_FLAGS_STDERR_MERGE),&error);
        if(!child) { std::string message=error->message; g_error_free(error); finished(1,message); return; }
        g_subprocess_communicate_utf8_async(child,nullptr,nullptr,[](GObject* object,GAsyncResult* result,gpointer data) {
            auto& ui=*static_cast<UI*>(data); gchar* output=nullptr; GError* error=nullptr;
            bool ok=g_subprocess_communicate_utf8_finish(G_SUBPROCESS(object),result,&output,nullptr,&error);
            bool passed=ok && g_subprocess_get_successful(G_SUBPROCESS(object));
            std::string message=ui.cancelling ? "Cancelled; partial measurements saved." : passed ? "Completed — performance baseline recorded." : "Test failed.\n";
            if(!passed && !ui.cancelling && output) { std::string log=output; message+=log.substr(log.size()>1600?log.size()-1600:0); }
            if(error) { message+=error->message; g_error_free(error); }
            g_free(output); g_object_unref(ui.child); ui.child=nullptr;
            ui.finished(passed?0:ui.cancelling?130:1,message);
        },this);
    }
    void cancel() {
        if(!running || cancelling) return;
        cancelling=true; cancel_start=Clock::now(); label("Stopping…");
        if(child) g_subprocess_send_signal(child,SIGINT);
    }
    gboolean render() {
        if(!running || !report->options.window) return TRUE;
        GLint destination_fbo=0; glGetIntegerv(GL_FRAMEBUFFER_BINDING,&destination_fbo);
        try {
            if(auto* error=gtk_gl_area_get_error(GTK_GL_AREA(area))) throw std::runtime_error(error->message);
            if(!renderer) {
                renderer=std::make_unique<Renderer>(report->options);
                report->device="GTK Wayland EGL context";
                session=std::make_unique<Session>(*report,*renderer);
            }
            if(cancelling || interrupted) {
                session->finish("cancelled"); session.reset(); renderer.reset();
                finished(130,"Cancelled; partial measurements saved.");
            } else {
                bool next=session->step();
                int scale=gtk_widget_get_scale_factor(area);
                renderer->present(destination_fbo,gtk_widget_get_allocated_width(area)*scale,gtk_widget_get_allocated_height(area)*scale);
                if(!next) { session.reset(); renderer.reset(); finished(0,"Completed — window frame-rate baseline recorded."); }
            }
        } catch(const std::exception& e) {
            report->status="failed"; report->error=e.what();
            try { if(session) session->finish("failed",e.what()); else report->save(); } catch(const std::exception& save) { std::cerr<<save.what()<<'\n'; }
            session.reset(); renderer.reset(); finished(1,e.what());
        }
        glBindFramebuffer(GL_FRAMEBUFFER,destination_fbo);
        return TRUE;
    }
};
static int gui(Options o) {
    // GDK creates a shared context before GtkGLArea's per-widget ES request.
    // Qualcomm's GLES-only EGL driver cannot create its default desktop GL context.
    g_setenv("GDK_GL","gles",FALSE);
    if(!gtk_init_check(nullptr,nullptr)) throw std::runtime_error("No desktop display available; use --headless over SSH");
    UI ui; ui.base=o;
    ui.executable=std::filesystem::read_symlink("/proc/self/exe").string();
    ui.window=gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(ui.window),"Q6A GPU Performance"); gtk_window_set_default_size(GTK_WINDOW(ui.window),940,520);
    auto* box=gtk_box_new(GTK_ORIENTATION_VERTICAL,8); gtk_container_set_border_width(GTK_CONTAINER(box),12); gtk_container_add(GTK_CONTAINER(ui.window),box);
    auto* title=gtk_label_new(("GPU rendering performance · "+std::to_string(o.width)+" × "+std::to_string(o.height)).c_str()); gtk_box_pack_start(GTK_BOX(box),title,FALSE,FALSE,0);
    auto* controls=gtk_box_new(GTK_ORIENTATION_HORIZONTAL,10); gtk_box_pack_start(GTK_BOX(box),controls,FALSE,FALSE,0);
    ui.suite=gtk_combo_box_text_new(); gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(ui.suite),"Quick: five scenes"); gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(ui.suite),"Stability: mixed load");gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(ui.suite),"3D model: Flight Helmet");
    gtk_combo_box_set_active(GTK_COMBO_BOX(ui.suite),o.suite=="quick"?0:o.suite=="stability"?1:2);
    ui.mode=gtk_combo_box_text_new(); gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(ui.mode),"Offscreen throughput"); gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(ui.mode),"Window frame rate"); gtk_combo_box_set_active(GTK_COMBO_BOX(ui.mode),o.window?1:0);
    ui.duration=gtk_spin_button_new_with_range(1,3600,1); gtk_spin_button_set_value(GTK_SPIN_BUTTON(ui.duration),o.duration);
    ui.instances=gtk_spin_button_new_with_range(1,64,1);gtk_spin_button_set_value(GTK_SPIN_BUTTON(ui.instances),o.instances);
    ui.start=gtk_button_new_with_label("Start"); ui.stop=gtk_button_new_with_label("Stop"); gtk_widget_set_sensitive(ui.stop,false);
    for(auto* widget : {ui.suite,ui.mode}) gtk_box_pack_start(GTK_BOX(controls),widget,FALSE,FALSE,0);
    auto* actions=gtk_box_new(GTK_ORIENTATION_HORIZONTAL,10); gtk_box_pack_start(GTK_BOX(box),actions,FALSE,FALSE,0);
    for(auto* widget : {gtk_label_new("Seconds / scene"),ui.duration,gtk_label_new("3D instances"),ui.instances,ui.start,ui.stop}) gtk_box_pack_start(GTK_BOX(actions),widget,FALSE,FALSE,0);
    ui.status=gtk_label_new("Choose a test. Stability defaults to 60 seconds; no FPS pass threshold is imposed.");
    gtk_label_set_line_wrap(GTK_LABEL(ui.status),true); gtk_label_set_selectable(GTK_LABEL(ui.status),true); gtk_box_pack_start(GTK_BOX(box),ui.status,FALSE,FALSE,0);
    ui.area=gtk_gl_area_new(); gtk_gl_area_set_use_es(GTK_GL_AREA(ui.area),TRUE); gtk_gl_area_set_required_version(GTK_GL_AREA(ui.area),3,0);
    gtk_gl_area_set_auto_render(GTK_GL_AREA(ui.area),FALSE); gtk_widget_set_size_request(ui.area,480,120); gtk_box_pack_start(GTK_BOX(box),ui.area,TRUE,TRUE,0);
    gtk_widget_set_no_show_all(ui.area,TRUE);
    ui.path=gtk_label_new("Reports are saved automatically as JSON and CSV."); gtk_label_set_selectable(GTK_LABEL(ui.path),true); gtk_label_set_line_wrap(GTK_LABEL(ui.path),true); gtk_box_pack_start(GTK_BOX(box),ui.path,FALSE,FALSE,0);
    gtk_label_set_max_width_chars(GTK_LABEL(ui.path),85); gtk_label_set_line_wrap_mode(GTK_LABEL(ui.path),PANGO_WRAP_WORD_CHAR);
    gtk_label_set_max_width_chars(GTK_LABEL(ui.status),85); gtk_label_set_line_wrap_mode(GTK_LABEL(ui.status),PANGO_WRAP_WORD_CHAR);
    auto* open=gtk_button_new_with_label("Open results folder"); gtk_box_pack_start(GTK_BOX(box),open,FALSE,FALSE,0);
    g_signal_connect(open,"clicked",G_CALLBACK(+[](GtkButton*,gpointer p) { auto& ui=*static_cast<UI*>(p); if(ui.destination.empty())return; auto uri=g_filename_to_uri(ui.destination.c_str(),nullptr,nullptr); GError* error=nullptr; if(uri && !gtk_show_uri_on_window(GTK_WINDOW(ui.window),uri,GDK_CURRENT_TIME,&error)) { ui.label(error->message); g_error_free(error); } g_free(uri); }),&ui);
    g_signal_connect(ui.start,"clicked",G_CALLBACK(+[](GtkButton*,gpointer p) { static_cast<UI*>(p)->begin(); }),&ui);
    g_signal_connect(ui.stop,"clicked",G_CALLBACK(+[](GtkButton*,gpointer p) { static_cast<UI*>(p)->cancel(); }),&ui);
    g_signal_connect(ui.suite,"changed",G_CALLBACK(+[](GtkComboBox* combo,gpointer p) { auto& ui=*static_cast<UI*>(p); gtk_spin_button_set_value(GTK_SPIN_BUTTON(ui.duration),gtk_combo_box_get_active(combo)==0?10:60); }),&ui);
    g_signal_connect(ui.area,"render",G_CALLBACK(+[](GtkGLArea*,GdkGLContext*,gpointer p)->gboolean { return static_cast<UI*>(p)->render(); }),&ui);
    gtk_widget_add_tick_callback(ui.area,+[](GtkWidget* area,GdkFrameClock*,gpointer p)->gboolean { auto& ui=*static_cast<UI*>(p); if(ui.running && ui.report->options.window) gtk_gl_area_queue_render(GTK_GL_AREA(area)); return G_SOURCE_CONTINUE; },&ui,nullptr);
    g_signal_connect(ui.window,"delete-event",G_CALLBACK(+[](GtkWidget*,GdkEvent*,gpointer p)->gboolean { auto& ui=*static_cast<UI*>(p); if(ui.running) { ui.closing=true; ui.cancel(); } else gtk_main_quit(); return TRUE; }),&ui);
    guint poll=g_timeout_add(250,+[](gpointer p)->gboolean {
        auto& ui=*static_cast<UI*>(p);
        if(interrupted) { if(ui.running) { ui.closing=true; ui.cancel(); } else gtk_main_quit(); }
        if(ui.running && ui.report->options.window) {
            if(auto* error=gtk_gl_area_get_error(GTK_GL_AREA(ui.area))) {
                ui.report->status="failed"; ui.report->error=error->message;
                try { ui.report->save(); } catch(const std::exception& e) { std::cerr<<e.what()<<'\n'; }
                ui.finished(1,error->message);
            }
        }
        if(ui.child && ui.cancelling && Clock::now()-ui.cancel_start>std::chrono::seconds(8)) g_subprocess_force_exit(ui.child);
        if(ui.running && !ui.cancelling) {
            std::ifstream csv(std::filesystem::path(ui.destination)/"samples.csv"); std::string line,last;
            std::getline(csv,line); while(std::getline(csv,line)) if(!line.empty())last=line;
            if(!last.empty()) {
                std::istringstream fields(last); std::string scene,elapsed,fps,ms;
                std::getline(fields,scene,','); std::getline(fields,elapsed,','); std::getline(fields,fps,','); std::getline(fields,ms,',');
                ui.label(scene+" · "+elapsed+" s · "+fps+" FPS · "+ms+" ms/frame");
            }
        }
        return G_SOURCE_CONTINUE;
    },&ui);
    gtk_widget_show_all(ui.window); gtk_widget_set_visible(ui.area,o.window);
    if(o.autorun) g_idle_add(+[](gpointer p)->gboolean { static_cast<UI*>(p)->begin(); return G_SOURCE_REMOVE; },&ui);
    gtk_main(); g_source_remove(poll);
    // Stop the proprietary EGL Wayland updater while wl_surface is still alive.
    // Destroying GtkWindow first races its proxy teardown against that thread.
    if(gdk_gl_context_get_current())glFinish();
    gdk_gl_context_clear_current();
    if(gtk_widget_get_realized(ui.area))gtk_widget_unrealize(ui.area);
    if(gdk_gl_context_get_current())glFinish();
    gdk_gl_context_clear_current();
    gtk_widget_destroy(ui.window); return ui.result;
}
int main(int argc,char** argv) {
    try {
        if(argc==2 && std::string(argv[1])=="--help") {
            std::cout<<"gpu-benchmark [--gui [--window] [--autorun --quit-after-run] | --headless]\n"
                "  --suite quick|stability|model3d --size WIDTHxHEIGHT --duration SECONDS\n"
                "  --model PATH.glb --instances 1..64 --check-model --offscreen\n"
                "  --device /dev/dri/renderD128 --output DIRECTORY --self-test\n";
            return 0;
        }
        auto o=parse(argc,argv);
        if(o.self_test) return self_test();
        if(o.check_model) { auto model=load_model(o.model.empty()?default_model():o.model);std::cout<<model.triangles<<" triangles, "<<model.meshes.size()<<" meshes, SHA256 "<<model.hash<<'\n';return 0; }
        bool explicit_output=false; for(int i=1;i<argc;i++) if(std::string(argv[i])=="--output")explicit_output=true;
        if(o.gui && !explicit_output) o.output=(std::filesystem::path(g_get_user_data_dir())/"gpu-benchmark/results").string();
        if(o.gui && std::filesystem::path(o.output).is_relative()) o.output=std::filesystem::absolute(o.output).string();
        std::signal(SIGINT,signal_handler); std::signal(SIGTERM,signal_handler);
        return o.gui ? gui(o) : headless(o);
    } catch(const std::exception& e) { std::cerr<<e.what()<<'\n'; return 2; }
}
