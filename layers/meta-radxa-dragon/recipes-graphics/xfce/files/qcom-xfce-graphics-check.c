/* SPDX-License-Identifier: MIT */
/* Exercise GPU rendering, X11 pixmap import, and actual window presentation. */
#define GL_GLEXT_PROTOTYPES
#include <GL/gl.h>
#include <GL/glx.h>
#include <GL/glxext.h>
#include <X11/Xutil.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int near(unsigned value, unsigned expected)
{
    return abs((int)value - (int)expected) <= 3;
}

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    Display *display = XOpenDisplay(NULL);
    if (!display) {
        fputs("Cannot open X display\n", stderr);
        return 1;
    }
    const int attrs[] = {
        GLX_X_RENDERABLE, True,
        GLX_DRAWABLE_TYPE, GLX_WINDOW_BIT | GLX_PIXMAP_BIT,
        GLX_RENDER_TYPE, GLX_RGBA_BIT,
        GLX_DOUBLEBUFFER, True,
        GLX_RED_SIZE, 8, GLX_GREEN_SIZE, 8, GLX_BLUE_SIZE, 8,
        GLX_BIND_TO_TEXTURE_RGB_EXT, True,
        None
    };
    int count = 0;
    GLXFBConfig *configs = glXChooseFBConfig(display, DefaultScreen(display), attrs, &count);
    XVisualInfo *visual = NULL;
    GLXFBConfig config = NULL;
    for (int i = 0; i < count; i++) {
        visual = glXGetVisualFromFBConfig(display, configs[i]);
        if (visual && visual->depth == 24) {
            config = configs[i];
            break;
        }
        if (visual)
            XFree(visual);
        visual = NULL;
    }
    if (!visual) {
        fputs("No RGB texture-from-pixmap visual\n", stderr);
        return 1;
    }
    Window root = RootWindow(display, visual->screen);
    Colormap colormap = XCreateColormap(display, root, visual->visual, AllocNone);
    XSetWindowAttributes wa = { .colormap = colormap, .border_pixel = 0,
                               .override_redirect = True };
    Window window = XCreateWindow(display, root, 0, 0, 64, 64, 0, visual->depth,
                                  InputOutput, visual->visual,
                                  CWColormap | CWBorderPixel | CWOverrideRedirect, &wa);
    XMapWindow(display, window);
    XSync(display, False);
    GLXContext context = glXCreateNewContext(display, config, GLX_RGBA_TYPE, NULL, True);
    if (!context || !glXMakeCurrent(display, window, context)) {
        fputs("Cannot create direct GLX context\n", stderr);
        return 1;
    }
    const char *renderer = (const char *)glGetString(GL_RENDERER);
    if (!glXIsDirect(display, context) || !renderer || !strstr(renderer, "Adreno") ||
        strstr(renderer, "llvmpipe") || strstr(renderer, "softpipe")) {
        fprintf(stderr, "Hardware renderer required: %s\n", renderer ? renderer : "none");
        return 1;
    }
    printf("Renderer: %s\n", renderer);

    Pixmap pixmap = XCreatePixmap(display, root, 64, 64, visual->depth);
    GC gc = XCreateGC(display, pixmap, 0, NULL);
    XSetForeground(display, gc, 0x4080c0);
    XFillRectangle(display, pixmap, gc, 0, 0, 64, 64);
    XSync(display, False);
    const int texture_attrs[] = {
        GLX_TEXTURE_TARGET_EXT, GLX_TEXTURE_2D_EXT,
        GLX_TEXTURE_FORMAT_EXT, GLX_TEXTURE_FORMAT_RGB_EXT, None
    };
    GLXPixmap glx_pixmap = glXCreatePixmap(display, config, pixmap, texture_attrs);
    PFNGLXBINDTEXIMAGEEXTPROC bind_image =
        (PFNGLXBINDTEXIMAGEEXTPROC)glXGetProcAddressARB((const GLubyte *)"glXBindTexImageEXT");
    PFNGLXRELEASETEXIMAGEEXTPROC release_image =
        (PFNGLXRELEASETEXIMAGEEXTPROC)glXGetProcAddressARB((const GLubyte *)"glXReleaseTexImageEXT");
    if (!glx_pixmap || !bind_image || !release_image) {
        fputs("Texture-from-pixmap is unavailable\n", stderr);
        return 1;
    }
    GLuint texture;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glXWaitX();
    bind_image(display, glx_pixmap, GLX_FRONT_LEFT_EXT, NULL);
    glViewport(0, 0, 64, 64);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    const char *vertex_source =
        "#version 330\n"
        "out vec2 uv;\n"
        "void main() { vec2 p=vec2(gl_VertexID&1,gl_VertexID>>1); "
        "uv=p; gl_Position=vec4(p*2.0-1.0,0.0,1.0); }\n";
    const char *fragment_source =
        "#version 330\n"
        "in vec2 uv; uniform sampler2D image; out vec4 color;\n"
        "void main() { color=texture(image,uv); }\n";
    GLuint vertex = glCreateShader(GL_VERTEX_SHADER);
    GLuint fragment = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(vertex, 1, &vertex_source, NULL);
    glShaderSource(fragment, 1, &fragment_source, NULL);
    glCompileShader(vertex);
    glCompileShader(fragment);
    GLuint program = glCreateProgram();
    glAttachShader(program, vertex);
    glAttachShader(program, fragment);
    glLinkProgram(program);
    GLint linked = 0;
    glGetProgramiv(program, GL_LINK_STATUS, &linked);
    if (!linked) {
        char log[1024];
        glGetProgramInfoLog(program, sizeof(log), NULL, log);
        fprintf(stderr, "Window probe shader link failed: %s\n", log);
        return 1;
    }
    glUseProgram(program);
    glUniform1i(glGetUniformLocation(program, "image"), 0);
    GLuint vao;
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glFinish();
    unsigned char pixel[4] = { 0 };
    glReadPixels(32, 32, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
    int passed = glGetError() == GL_NO_ERROR && near(pixel[0], 64) &&
                 near(pixel[1], 128) && near(pixel[2], 192);
    printf("Pixmap texture pixel: %u %u %u\n", pixel[0], pixel[1], pixel[2]);
    glXSwapBuffers(display, window);
    glFinish();
    XSync(display, False);
    usleep(100000);
    XImage *image = XGetImage(display, window, 0, 0, 64, 64, AllPlanes, ZPixmap);
    if (!image)
        passed = 0;
    else {
        unsigned long value = XGetPixel(image, 32, 32);
        printf("Presented window pixel: %06lx\n", value);
        passed = passed && near((value >> 16) & 255, 64) &&
                 near((value >> 8) & 255, 128) && near(value & 255, 192);
        XDestroyImage(image);
    }
    release_image(display, glx_pixmap, GLX_FRONT_LEFT_EXT);
    glDeleteVertexArrays(1, &vao);
    glDeleteProgram(program);
    glDeleteShader(vertex);
    glDeleteShader(fragment);
    glDeleteTextures(1, &texture);
    glXDestroyPixmap(display, glx_pixmap);
    XFreeGC(display, gc);
    XFreePixmap(display, pixmap);
    glXMakeCurrent(display, None, NULL);
    glXDestroyContext(display, context);
    XDestroyWindow(display, window);
    XFreeColormap(display, colormap);
    XFree(visual);
    XFree(configs);
    XCloseDisplay(display);
    puts(passed ? "PASS hardware pixmap and window presentation" : "FAIL pixmap/window pixel mismatch");
    return passed ? 0 : 1;
}
