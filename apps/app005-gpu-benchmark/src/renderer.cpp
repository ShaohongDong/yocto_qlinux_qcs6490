// SPDX-License-Identifier: MIT
#include "renderer.hpp"
#include <EGL/eglext.h>
#include <fcntl.h>
#include <unistd.h>
#include <algorithm>
#include <cmath>
#include <iostream>
#include <numeric>
#include <stdexcept>

namespace gpu {
static void require(bool ok, const std::string& error) { if (!ok) throw std::runtime_error(error); }
void Context::release() {
    if (display != EGL_NO_DISPLAY) {
        eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (context != EGL_NO_CONTEXT) eglDestroyContext(display, context);
        eglTerminate(display);
    }
    if (gbm) gbm_device_destroy(gbm);
    if (fd >= 0) close(fd);
}
Context::~Context() { release(); }
Context::Context(const std::string& requested) {
    try {
        device = requested;
        if (device.empty()) {
            std::vector<std::string> nodes;
            std::error_code ec;
            for (const auto& e : std::filesystem::directory_iterator("/dev/dri", ec))
                if (e.path().filename().string().find("renderD") == 0) nodes.push_back(e.path());
            std::sort(nodes.begin(), nodes.end());
            require(nodes.size() == 1, "Expected one GPU render node; select explicitly with --device /dev/dri/renderD128");
            device = nodes.front();
        }
        require(device.find("/dev/dri/renderD") == 0, "--device must be a DRM render node");
        fd = open(device.c_str(), O_RDWR | O_CLOEXEC);
        require(fd >= 0, "Cannot open " + device);
        gbm = gbm_create_device(fd); require(gbm, "Cannot create GBM device");
        auto get_display = reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(eglGetProcAddress("eglGetPlatformDisplayEXT"));
        require(get_display, "EGL platform display extension unavailable");
        display = get_display(EGL_PLATFORM_GBM_KHR, gbm, nullptr);
        require(display != EGL_NO_DISPLAY && eglInitialize(display, nullptr, nullptr), "Cannot initialize GBM EGL display");
        require(eglBindAPI(EGL_OPENGL_ES_API), "Cannot bind OpenGL ES");
        const EGLint attributes[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
            EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_NONE};
        EGLConfig config; EGLint count = 0;
        require(eglChooseConfig(display, attributes, &config, 1, &count) && count, "No EGL GLES3 config");
        const EGLint ctx[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
        context = eglCreateContext(display, config, EGL_NO_CONTEXT, ctx);
        require(context != EGL_NO_CONTEXT, "Cannot create GLES3 context");
        require(eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, context), "Surfaceless EGL context unsupported");
    } catch (...) { release(); throw; }
}
static GLuint shader(GLenum type, const char* source) {
    GLuint id = glCreateShader(type); glShaderSource(id, 1, &source, nullptr); glCompileShader(id);
    GLint ok; glGetShaderiv(id, GL_COMPILE_STATUS, &ok);
    if (!ok) { char log[2048]{}; glGetShaderInfoLog(id, sizeof(log), nullptr, log); glDeleteShader(id); throw std::runtime_error(log); }
    return id;
}
Renderer::Renderer(const Options& options) : width(options.width), height(options.height) {
    const char* vs = R"(#version 300 es
layout(location=0) in vec2 position;
out vec2 uv;
uniform float phase;
void main() { uv=position*0.5+0.5; gl_Position=vec4(position,0.0,1.0); }
)";
    const char* fs = R"(#version 300 es
precision highp float;
in vec2 uv;
out vec4 color;
uniform sampler2D pattern;
uniform int mode;
uniform float phase;
void main() {
    if(mode==4) { color=vec4(0.25,0.5,0.75,1.0); return; }
    vec3 c=vec3(uv,0.4+0.1*sin(phase));
    if(mode==1 || mode==3) {
        c=vec3(0.0);
        for(int i=0;i<16;i++) c+=texture(pattern,uv*4.0+vec2(float(i)*0.013,phase*0.01)).rgb/16.0;
    }
    if(mode==2 || mode==3) {
        vec3 v=c+vec3(0.01*phase);
        for(int i=0;i<64;i++) v=sin(v.yzx*1.01+vec3(uv,0.1))+v*0.1;
        c=0.5+0.5*v;
    }
    color=vec4(c,1.0);
}
)";
    GLuint vertex = shader(GL_VERTEX_SHADER, vs), fragment = 0;
    try { fragment = shader(GL_FRAGMENT_SHADER, fs); } catch (...) { glDeleteShader(vertex); throw; }
    program = glCreateProgram(); glAttachShader(program, vertex); glAttachShader(program, fragment);
    glLinkProgram(program); glDeleteShader(vertex); glDeleteShader(fragment);
    GLint ok; glGetProgramiv(program, GL_LINK_STATUS, &ok); require(ok, "Shader link failed");
    std::vector<float> vertices;
    for (int y = 0; y < 128; ++y) for (int x = 0; x < 128; ++x) {
        float a = x / 64.f - 1.f, b = y / 64.f - 1.f, c = (x+1) / 64.f - 1.f, d = (y+1) / 64.f - 1.f;
        vertices.insert(vertices.end(), {a,b,c,b,a,d,a,d,c,b,c,d});
    }
    glGenVertexArrays(1, &vao); glBindVertexArray(vao);
    glGenBuffers(1, &vbo); glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, vertices.size()*sizeof(float), vertices.data(), GL_STATIC_DRAW);
    glEnableVertexAttribArray(0); glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
    std::vector<unsigned char> pixels(256*256*4);
    for (int y=0; y<256; ++y) for (int x=0; x<256; ++x) {
        size_t i=(y*256+x)*4; pixels[i]=x; pixels[i+1]=y; pixels[i+2]=(x^y); pixels[i+3]=255;
    }
    glGenTextures(1, &texture); glBindTexture(GL_TEXTURE_2D, texture);
    glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,256,256,0,GL_RGBA,GL_UNSIGNED_BYTE,pixels.data());
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
    glGenTextures(1, &target); glBindTexture(GL_TEXTURE_2D, target);
    glTexStorage2D(GL_TEXTURE_2D,1,GL_RGBA8,width,height);
    glGenFramebuffers(1,&fbo); glBindFramebuffer(GL_FRAMEBUFFER,fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,target,0);
    glGenRenderbuffers(1,&depth);glBindRenderbuffer(GL_RENDERBUFFER,depth);glRenderbufferStorage(GL_RENDERBUFFER,GL_DEPTH_COMPONENT24,width,height);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER,GL_DEPTH_ATTACHMENT,GL_RENDERBUFFER,depth);
    require(glCheckFramebufferStatus(GL_FRAMEBUFFER)==GL_FRAMEBUFFER_COMPLETE,"Incomplete benchmark framebuffer");
    GLint n=0; glGetIntegerv(GL_NUM_EXTENSIONS,&n);
    for (int i=0;i<n;i++) if (std::string(reinterpret_cast<const char*>(glGetStringi(GL_EXTENSIONS,i)))=="GL_EXT_disjoint_timer_query") timer=true;
    query_result=reinterpret_cast<PFNGLGETQUERYOBJECTUI64VEXTPROC>(eglGetProcAddress("glGetQueryObjectui64vEXT"));
    timer=timer && query_result;
    if (timer) glGenQueries(1,&query);
    require(glGetError()==GL_NO_ERROR,"GL resource initialization failed");
    if(options.suite=="quick" || options.suite=="model3d") {
        model_path=options.model.empty()?default_model():options.model;
        auto data=load_model(model_path);model_hash=data.hash;model_triangles=data.triangles;
        model=std::make_unique<ModelRenderer>(data,options.instances);
    }
}
Renderer::~Renderer() {
    model.reset();glDeleteRenderbuffers(1,&depth);
    if(query) glDeleteQueries(1,&query);
    glDeleteFramebuffers(1,&fbo); glDeleteTextures(1,&target); glDeleteTextures(1,&texture);
    glDeleteBuffers(1,&vbo); glDeleteVertexArrays(1,&vao); glDeleteProgram(program);
}
void Renderer::identify(Report& r) {
    r.renderer=reinterpret_cast<const char*>(glGetString(GL_RENDERER));
    r.vendor=reinterpret_cast<const char*>(glGetString(GL_VENDOR));
    r.version=reinterpret_cast<const char*>(glGetString(GL_VERSION));
    r.options.model=model_path;r.model_hash=model_hash;r.model_triangles=model_triangles;
    require(!software_renderer(r.renderer),"Software renderer rejected: " + r.renderer);
    require(r.renderer.find("Adreno")!=std::string::npos || r.renderer.find("FD")!=std::string::npos,
            "Q6A acceptance requires an Adreno/Freedreno renderer: " + r.renderer);
}
void Renderer::wait() {
    GLsync fence=glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE,0);
    require(fence,"Cannot create GPU completion fence");
    glFlush(); auto start=Clock::now();
    while (true) {
        GLenum result=glClientWaitSync(fence,0,10000000);
        if(result==GL_ALREADY_SIGNALED || result==GL_CONDITION_SATISFIED) break;
        if(result==GL_WAIT_FAILED || Clock::now()-start>std::chrono::seconds(5)) {
            glDeleteSync(fence); throw std::runtime_error("GPU fence failed or exceeded 5 seconds");
        }
    }
    glDeleteSync(fence);
}
std::pair<double,double> Renderer::frame(const std::string& scene,float phase) {
    int mode=scene=="geometry"?0:scene=="texture"?1:scene=="fragment"?2:scene=="check"?4:3;
    auto start=Clock::now();
    glBindFramebuffer(GL_FRAMEBUFFER,fbo); glViewport(0,0,width,height);
    glDisable(GL_DEPTH_TEST); glDisable(GL_SCISSOR_TEST); glDisable(GL_BLEND);
    glUseProgram(program); glBindVertexArray(vao); glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D,texture);
    glUniform1i(glGetUniformLocation(program,"pattern"),0);
    glUniform1i(glGetUniformLocation(program,"mode"),mode);
    glUniform1f(glGetUniformLocation(program,"phase"),phase);
    if(timer) glBeginQuery(GL_TIME_ELAPSED_EXT,query);
    if(scene=="model3d") { require(bool(model),"3D model not loaded");model->draw(width,height,phase); }
    else glDrawArrays(GL_TRIANGLES,0,128*128*6);
    if(timer) glEndQuery(GL_TIME_ELAPSED_EXT);
    wait();
    double cpu=std::chrono::duration<double,std::milli>(Clock::now()-start).count(), gpu=-1;
    if(timer) {
        GLint disjoint=0; glGetIntegerv(GL_GPU_DISJOINT_EXT,&disjoint);
        GLuint available=0; glGetQueryObjectuiv(query,GL_QUERY_RESULT_AVAILABLE,&available);
        if(!disjoint && available) { GLuint64 ns=0; query_result(query,GL_QUERY_RESULT_EXT,&ns); gpu=ns/1e6; }
    }
    require(glGetError()==GL_NO_ERROR,"GL rendering error");
    return {cpu,gpu};
}
void Renderer::check_pixels() {
    frame("check",0); unsigned char pixel[4]{};
    glReadPixels(width/2,height/2,1,1,GL_RGBA,GL_UNSIGNED_BYTE,pixel);
    require(std::abs(int(pixel[0])-64)<=1 && std::abs(int(pixel[1])-128)<=1 && std::abs(int(pixel[2])-191)<=1 && pixel[3]==255,
            "Deterministic shader pixel check failed");
    frame("geometry",0);
    glReadPixels(width/4,height/4,1,1,GL_RGBA,GL_UNSIGNED_BYTE,pixel);
    require(std::abs(int(pixel[0])-64)<=3 && std::abs(int(pixel[1])-64)<=3 && std::abs(int(pixel[2])-102)<=1,
            "Geometry gradient pixel check failed");
    require(glGetError()==GL_NO_ERROR,"Pixel readback failed");
    if(model) {
        frame("model3d",0);
        std::vector<unsigned char> rgba(static_cast<size_t>(width)*height*4);
        glReadPixels(0,0,width,height,GL_RGBA,GL_UNSIGNED_BYTE,rgba.data());
        size_t lit=0;for(size_t j=0;j<rgba.size();j+=4)if(rgba[j]>35 || rgba[j+1]>40 || rgba[j+2]>45)lit++;
        require(lit>static_cast<size_t>(width)*height/200,"3D model pixel coverage check failed");
        require(glGetError()==GL_NO_ERROR,"3D pixel readback failed");
    }
}
void Renderer::present(GLuint destination,int w,int h) {
    glBindFramebuffer(GL_READ_FRAMEBUFFER,fbo); glBindFramebuffer(GL_DRAW_FRAMEBUFFER,destination);
    glBlitFramebuffer(0,0,width,height,0,0,w,h,GL_COLOR_BUFFER_BIT,GL_NEAREST);
    glBindFramebuffer(GL_FRAMEBUFFER,destination);
    require(glGetError()==GL_NO_ERROR,"Window blit failed");
}
Session::Session(Report& r,Renderer& engine) : report(r),renderer(engine) {
    renderer.identify(report); renderer.check_pixels(); report.pixels_checked=true;
    for (auto s : report.options.suite=="quick" ? std::vector<std::string>{"geometry","texture","fragment","mixed","model3d"} : std::vector<std::string>{report.options.suite=="model3d"?"model3d":"mixed"}) {
        Scene scene; scene.name=s; report.scenes.push_back(scene);
    }
    warm_start=Clock::now(); report.save();
}
void Session::sample(Clock::time_point now) {
    auto& s=report.scenes[index];
    size_t count=s.frames.size()-sample_frame;
    if(!count) return;
    Sample p; p.scene=s.name; p.second=std::chrono::duration<double>(now-start).count();
    p.interval=std::chrono::duration<double>(now-sample_start).count(); p.fps=count/p.interval;
    p.mean_ms=std::accumulate(s.frames.begin()+sample_frame,s.frames.end(),0.0)/count;
    if(s.gpu_times.size()>sample_gpu) p.gpu_ms=std::accumulate(s.gpu_times.begin()+sample_gpu,s.gpu_times.end(),0.0)/(s.gpu_times.size()-sample_gpu);
    telemetry.read(p); report.samples.push_back(p);
    std::cout << s.name << " " << int(p.second) << "s  " << p.fps << " FPS  " << p.mean_ms << " ms" << std::endl;
    sample_frame=s.frames.size(); sample_gpu=s.gpu_times.size(); sample_start=now;
    s.seconds=p.second; report.save();
}
bool Session::step() {
    auto now=Clock::now();
    if(!measuring && now-warm_start>=std::chrono::seconds(3)) {
        measuring=true; start=sample_start=now;
    }
    auto& s=report.scenes[index];
    auto times=renderer.frame(s.name,std::chrono::duration<float>(now-warm_start).count());
    now=Clock::now();
    if(!measuring) return true;
    s.frames.push_back(times.first); if(times.second>=0) s.gpu_times.push_back(times.second);
    s.seconds=std::chrono::duration<double>(now-start).count();
    bool complete=s.seconds>=report.options.duration;
    if(now-sample_start>=std::chrono::seconds(1) || complete) sample(now);
    if(!complete) return true;
    s.completed=true;
    if(++index==report.scenes.size()) { renderer.check_pixels(); finish("passed"); return false; }
    measuring=false; sample_frame=sample_gpu=0; warm_start=Clock::now(); return true;
}
void Session::finish(const std::string& status,const std::string& error) {
    if(measuring && index<report.scenes.size()) sample(Clock::now());
    report.status=status; report.error=error; report.save();
}
}
