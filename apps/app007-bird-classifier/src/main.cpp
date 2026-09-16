// SPDX-License-Identifier: MIT
#include "core.hpp"
#include "qnn.hpp"
#include <gtk/gtk.h>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <thread>
#include <atomic>
#include <sys/resource.h>
using Clock=std::chrono::steady_clock;
namespace fs=std::filesystem;
static double ms(Clock::time_point t){return std::chrono::duration<double,std::milli>(Clock::now()-t).count();}
struct Options {std::string input,model,labels,report,batch,dsp,dump_input,dump_output;int repeat=1,warmup=0;bool gui=false,self=false,preprocess_only=false;};
struct Result {std::string path;int truth=-1;std::vector<float> logits,prob;double inference=0,end_to_end=0;};
static std::vector<std::string> lines(const std::string& path){std::ifstream f(path);if(!f)throw std::runtime_error("Cannot open "+path);std::vector<std::string> v;std::string s;while(std::getline(f,s)){if(!s.empty()&&s.back()=='\r')s.pop_back();if(!s.empty())v.push_back(s);}return v;}
static std::string sha256(const std::string& path){std::ifstream f(path,std::ios::binary);if(!f)throw std::runtime_error("Cannot hash model");auto* c=g_checksum_new(G_CHECKSUM_SHA256);char b[65536];while(f.read(b,sizeof b)||f.gcount())g_checksum_update(c,reinterpret_cast<guchar*>(b),f.gcount());std::string s=g_checksum_get_string(c);g_checksum_free(c);return s;}
using Pix=std::unique_ptr<GdkPixbuf,decltype(&g_object_unref)>;
static Pix load(const std::string& path){GError* e=nullptr;auto* p=gdk_pixbuf_new_from_file(path.c_str(),&e);if(!p){std::string s=e?e->message:"Cannot decode image";g_clear_error(&e);throw std::runtime_error(s);}return Pix(p,g_object_unref);}
static std::vector<float> input(const std::string& path){auto p=load(path);return ai::preprocess(gdk_pixbuf_read_pixels(p.get()),gdk_pixbuf_get_width(p.get()),gdk_pixbuf_get_height(p.get()),gdk_pixbuf_get_rowstride(p.get()),gdk_pixbuf_get_n_channels(p.get()));}
static void raw(const std::string& path,const std::vector<float>& x){if(path.empty())return;std::ofstream f(path,std::ios::binary);f.write(reinterpret_cast<const char*>(x.data()),x.size()*sizeof(float));if(!f)throw std::runtime_error("Cannot write "+path);}
static Result infer(ai::Qnn& net,const std::string& path,int truth,const Options& opt){auto start=Clock::now();auto x=input(path);auto t=Clock::now();auto y=net.execute(x);double elapsed=ms(t);auto p=ai::softmax(y);Result r{path,truth,y,p,elapsed,ms(start)};raw(opt.dump_input,x);raw(opt.dump_output,y);return r;}
static double percentile(std::vector<double> v,double p){if(v.empty())return 0;std::sort(v.begin(),v.end());return v[size_t(std::ceil(p*v.size()))-1];}
static void report(const Options& o,const std::vector<Result>& rows,const std::vector<std::string>& labels,const std::string& profile,double load_ms,const std::string& error=""){
    if(o.report.empty())return;
    std::ofstream f(o.report);if(!f)throw std::runtime_error("Cannot write report");
    std::vector<double> latency,end;int n=0,correct=0,top5=0;int cm[20][20]={};struct rusage ru{};getrusage(RUSAGE_SELF,&ru);
    for(const auto& r:rows){latency.push_back(r.inference);end.push_back(r.end_to_end);if(r.truth>=0){++n;auto rank=ai::rank(r.prob);correct+=rank[0]==r.truth;top5+=std::find(rank.begin(),rank.begin()+5,r.truth)!=rank.begin()+5;++cm[r.truth][rank[0]];}}
    f<<"{\"status\":"<<ai::quote(error.empty()?"PASS":"FAIL")<<",\"error\":"<<ai::quote(error)<<",\"backend\":\"QNN_HTP\",\"model_sha256\":"<<ai::quote(fs::exists(o.model)?sha256(o.model):"")<<",\"load_ms\":"<<load_ms<<",\"warmup\":"<<o.warmup<<",\"count\":"<<rows.size()<<",\"peak_rss_kb\":"<<ru.ru_maxrss<<",\"inference_p50_ms\":"<<percentile(latency,.5)<<",\"inference_p95_ms\":"<<percentile(latency,.95)<<",\"end_to_end_p50_ms\":"<<percentile(end,.5)<<",\"end_to_end_p95_ms\":"<<percentile(end,.95)<<",\"labelled_count\":"<<n<<",\"top1\":"<<(n?double(correct)/n:0)<<",\"top5\":"<<(n?double(top5)/n:0)<<",\"qnn_profile\":"<<profile<<",\"confusion_matrix\":[";
    for(int i=0;i<20;++i){if(i)f<<',';f<<'[';for(int j=0;j<20;++j){if(j)f<<',';f<<cm[i][j];}f<<']';}f<<"],\"results\":[";
    for(size_t i=0;i<rows.size();++i){if(i)f<<',';const auto& r=rows[i];auto rank=ai::rank(r.prob);f<<"{\"input\":"<<ai::quote(r.path)<<",\"truth\":"<<r.truth<<",\"inference_ms\":"<<r.inference<<",\"uncertain\":"<<(r.prob[rank[0]]<.5?"true":"false")<<",\"top5\":[";for(int k=0;k<5;++k){if(k)f<<',';int c=rank[k];f<<"{\"class\":"<<c<<",\"name\":"<<ai::quote(labels[c])<<",\"probability\":"<<r.prob[c]<<'}';}f<<"],\"logits\":[";for(size_t k=0;k<r.logits.size();++k){if(k)f<<',';f<<r.logits[k];}f<<"]}";}
    f<<"]}\n";if(!f)throw std::runtime_error("Failed writing report");
}
struct Ui {
    Options opt;std::vector<std::string> labels;std::unique_ptr<ai::Qnn> net;std::vector<Result> results;
    GtkWidget *window=nullptr,*picture=nullptr,*status=nullptr,*choose=nullptr,*run=nullptr;std::thread worker;std::atomic<bool> busy{false},done{false};std::string message;std::mutex mutex;double load_ms=0;
    void start(){if(busy||opt.input.empty())return; if(worker.joinable())worker.join();busy=true;done=false;gtk_widget_set_sensitive(run,FALSE);gtk_widget_set_sensitive(choose,FALSE);gtk_label_set_text(GTK_LABEL(status),"Running on Q6A NPU...");
        try{auto p=load(opt.input);double scale=std::min(540.0/gdk_pixbuf_get_width(p.get()),340.0/gdk_pixbuf_get_height(p.get()));auto* scaled=gdk_pixbuf_scale_simple(p.get(),std::max(1,int(gdk_pixbuf_get_width(p.get())*scale)),std::max(1,int(gdk_pixbuf_get_height(p.get())*scale)),GDK_INTERP_BILINEAR);gtk_image_set_from_pixbuf(GTK_IMAGE(picture),scaled);g_object_unref(scaled);}catch(const std::exception& e){gtk_label_set_text(GTK_LABEL(status),e.what());busy=false;gtk_widget_set_sensitive(run,TRUE);gtk_widget_set_sensitive(choose,TRUE);return;}
        worker=std::thread([this]{std::string text;try{if(!net){auto t=Clock::now();net=std::make_unique<ai::Qnn>(opt.model);load_ms=ms(t);}auto r=infer(*net,opt.input,-1,opt);auto rank=ai::rank(r.prob);std::ostringstream s;s<<"QNN HTP / NPU  |  "<<r.inference<<" ms\n\n";for(int i=0;i<5;++i)s<<i+1<<". "<<labels[rank[i]]<<"  "<<int(std::round(r.prob[rank[i]]*100))<<"%\n";if(r.prob[rank[0]]<.5)s<<"\nUncertain: try a clearer bird image.";s<<"\n20 species only; probabilities are not calibrated.";text=s.str();results.push_back(r);report(opt,results,labels,net->profile_json(),load_ms);}catch(const std::exception& e){text=std::string("Failed: ")+e.what();try{report(opt,results,labels,"[]",load_ms,e.what());}catch(...) {}}{std::lock_guard<std::mutex> lock(mutex);message=text;}done=true;});
    }
    void show(){window=gtk_window_new(GTK_WINDOW_TOPLEVEL);gtk_window_set_title(GTK_WINDOW(window),"Q6A Bird Classifier — 20 species");gtk_window_set_default_size(GTK_WINDOW(window),960,540);auto* box=gtk_box_new(GTK_ORIENTATION_VERTICAL,16);gtk_container_set_border_width(GTK_CONTAINER(box),16);gtk_container_add(GTK_CONTAINER(window),box);
        auto* title=gtk_label_new(nullptr);gtk_label_set_markup(GTK_LABEL(title),"<span size='xx-large' weight='bold'>Bird species recognition</span>");gtk_box_pack_start(GTK_BOX(box),title,FALSE,FALSE,0);
        auto* buttons=gtk_box_new(GTK_ORIENTATION_HORIZONTAL,12);gtk_box_pack_start(GTK_BOX(box),buttons,FALSE,FALSE,0);choose=gtk_button_new_with_label("Choose image");run=gtk_button_new_with_label("Identify bird");gtk_box_pack_start(GTK_BOX(buttons),choose,FALSE,FALSE,0);gtk_box_pack_start(GTK_BOX(buttons),run,FALSE,FALSE,0);
        auto* content=gtk_box_new(GTK_ORIENTATION_HORIZONTAL,24);gtk_box_pack_start(GTK_BOX(box),content,TRUE,TRUE,0);picture=gtk_image_new();gtk_widget_set_size_request(picture,540,340);gtk_box_pack_start(GTK_BOX(content),picture,TRUE,TRUE,0);status=gtk_label_new("Choose a picture with one bird.\nInference runs on the Q6A NPU.");gtk_widget_set_size_request(status,300,-1);gtk_label_set_xalign(GTK_LABEL(status),0);gtk_label_set_line_wrap(GTK_LABEL(status),TRUE);gtk_box_pack_start(GTK_BOX(content),status,TRUE,TRUE,0);
        g_signal_connect(choose,"clicked",G_CALLBACK(+[](GtkButton*,gpointer p){auto& u=*static_cast<Ui*>(p);auto* d=gtk_file_chooser_dialog_new("Choose bird image",GTK_WINDOW(u.window),GTK_FILE_CHOOSER_ACTION_OPEN,"Cancel",GTK_RESPONSE_CANCEL,"Open",GTK_RESPONSE_ACCEPT,nullptr);gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(d),(fs::path(u.opt.model).parent_path().parent_path()/"data").c_str());auto* filter=gtk_file_filter_new();gtk_file_filter_add_pixbuf_formats(filter);gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(d),filter);if(gtk_dialog_run(GTK_DIALOG(d))==GTK_RESPONSE_ACCEPT){char* name=gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(d));u.opt.input=name;g_free(name);}gtk_widget_destroy(d);u.start();}),this);
        g_signal_connect(run,"clicked",G_CALLBACK(+[](GtkButton*,gpointer p){static_cast<Ui*>(p)->start();}),this);
        g_signal_connect(window,"destroy",G_CALLBACK(+[](GtkWidget*,gpointer){gtk_main_quit();}),this);
        guint timer=g_timeout_add(50,+[](gpointer p)->gboolean{auto& u=*static_cast<Ui*>(p);if(u.done.exchange(false)){if(u.worker.joinable())u.worker.join();{std::lock_guard<std::mutex> lock(u.mutex);gtk_label_set_text(GTK_LABEL(u.status),u.message.c_str());}u.busy=false;gtk_widget_set_sensitive(u.run,TRUE);gtk_widget_set_sensitive(u.choose,TRUE);}return G_SOURCE_CONTINUE;},this);
        gtk_widget_show_all(window);if(!opt.input.empty())start();gtk_main();g_source_remove(timer);if(worker.joinable())worker.join();
    }
};
static int positive(const std::string& s,bool zero=false){size_t used=0;int n=std::stoi(s,&used);if(used!=s.size()||n<(zero?0:1)||n>100000)throw std::runtime_error("Invalid repetition count");return n;}
int main(int argc,char** argv){Options o;std::vector<std::string> labels;std::vector<Result> results;double load_ms=0;
    try{auto base=fs::canonical("/proc/self/exe").parent_path().parent_path()/"share/bird-classifier/models";o.model=(base/"bird.bin").string();o.labels=(base/"labels.txt").string();
        for(int i=1;i<argc;++i){std::string key=argv[i];if(key=="--preprocess-only"){o.preprocess_only=true;continue;}if(key=="--gui"){o.gui=true;continue;}if(key=="--self-test"){o.self=true;continue;}if(key=="--help"){std::cout<<"bird-classifier [--gui] --input IMAGE | --batch TEST.tsv [--model bird.bin] [--labels labels.txt] [--report FILE] [--repeat N] [--warmup N] [--dump-input FILE] [--dump-output FILE] [--dsp-dir DIR]\n";return 0;}if(i+1>=argc)throw std::runtime_error("Missing option value");std::string value=argv[++i];if(key=="--input")o.input=value;else if(key=="--model")o.model=value;else if(key=="--labels")o.labels=value;else if(key=="--report")o.report=value;else if(key=="--batch")o.batch=value;else if(key=="--repeat")o.repeat=positive(value);else if(key=="--warmup")o.warmup=positive(value,true);else if(key=="--dsp-dir")o.dsp=value;else if(key=="--dump-input")o.dump_input=value;else if(key=="--dump-output")o.dump_output=value;else throw std::runtime_error("Unknown option "+key);}
        if(o.self){auto p=ai::softmax(std::vector<float>(20,0));if(std::abs(p[0]-.05f)>1e-6f||ai::quantize(1,.5,0,255)!=2)throw std::runtime_error("Core self-test failed");std::vector<uint8_t> white(5*3*3,255);auto x=ai::preprocess(white.data(),5,3,15,3);if(x.size()!=224*224*3||std::abs(x[0]-(1-.485f)/.229f)>1e-5)throw std::runtime_error("Preprocess self-test failed");std::cout<<"Bird classifier self-test PASS (no hardware execution)\n";return 0;}
        for(auto* p:{&o.input,&o.model,&o.labels,&o.report,&o.batch,&o.dsp,&o.dump_input,&o.dump_output})if(!p->empty())*p=fs::absolute(*p).string();
        if(o.preprocess_only){if(o.input.empty()||o.dump_input.empty())throw std::runtime_error("Preprocess-only requires input and dump-input");raw(o.dump_input,input(o.input));return 0;}
        if(!o.dsp.empty())fs::current_path(o.dsp);
        labels=lines(o.labels);if(labels.size()!=20)throw std::runtime_error("Expected 20 labels");
        if(!o.input.empty()&&!o.batch.empty())throw std::runtime_error("Choose input or batch");
        if(o.gui){if(!o.batch.empty())throw std::runtime_error("Batch requires CLI");if(!gtk_init_check(nullptr,nullptr))throw std::runtime_error("No GTK display");Ui u;u.opt=o;u.labels=labels;u.show();return 0;}
        std::vector<std::pair<std::string,int>> inputs;if(!o.batch.empty()){for(const auto& row:lines(o.batch)){auto tab=row.find('\t');if(tab==std::string::npos)throw std::runtime_error("Expected label TAB path");size_t used=0;int label=std::stoi(row.substr(0,tab),&used);if(used!=tab||label<0||label>=20)throw std::runtime_error("Invalid test label");inputs.emplace_back((fs::path(o.batch).parent_path()/row.substr(tab+1)).string(),label);}}else if(!o.input.empty())inputs.emplace_back(o.input,-1);if(inputs.empty())throw std::runtime_error("No input images");
        auto t=Clock::now();ai::Qnn net(o.model);load_ms=ms(t);for(int i=0;i<o.warmup;++i)infer(net,inputs[0].first,-1,Options{});
        for(int i=0;i<o.repeat;++i)for(const auto& item:inputs){results.push_back(infer(net,item.first,item.second,o));const auto& r=results.back();auto rank=ai::rank(r.prob);std::cout<<r.path<<"\t"<<labels[rank[0]]<<"\t"<<r.prob[rank[0]]<<"\t"<<r.inference<<" ms\n";}
        report(o,results,labels,net.profile_json(),load_ms);return 0;
    }catch(const std::exception& e){std::cerr<<"FAIL: "<<e.what()<<'\n';try{report(o,results,labels,"[]",load_ms,e.what());}catch(...){}return 1;}
}
