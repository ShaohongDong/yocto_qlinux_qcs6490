// SPDX-License-Identifier: MIT
#pragma once
#include "core.hpp"
#include "model.hpp"
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>
#include <gbm.h>
#include <memory>

namespace gpu {
class Context {
    int fd = -1;
    gbm_device* gbm = nullptr;
    EGLDisplay display = EGL_NO_DISPLAY;
    EGLContext context = EGL_NO_CONTEXT;
    void release();
public:
    std::string device;
    explicit Context(const std::string& requested);
    ~Context();
    Context(const Context&) = delete;
    Context& operator=(const Context&) = delete;
};
class Renderer {
    GLuint program = 0, vao = 0, vbo = 0, texture = 0, target = 0, fbo = 0, query = 0, depth = 0;
    std::unique_ptr<ModelRenderer> model;
    std::string model_path, model_hash;
    size_t model_triangles=0;
    PFNGLGETQUERYOBJECTUI64VEXTPROC query_result = nullptr;
    int width, height;
    bool timer = false;
    void wait();
public:
    explicit Renderer(const Options& options);
    ~Renderer();
    void identify(Report& report);
    void check_pixels();
    std::pair<double, double> frame(const std::string& scene, float phase);
    void present(GLuint destination, int w, int h);
};
class Session {
    Report& report;
    Renderer& renderer;
    Telemetry telemetry;
    Clock::time_point warm_start = Clock::now(), start{}, sample_start{};
    size_t index = 0, sample_frame = 0, sample_gpu = 0;
    bool measuring = false;
    void sample(Clock::time_point now);
public:
    Session(Report& r, Renderer& engine);
    bool step();
    void finish(const std::string& status, const std::string& error = "");
};
}
