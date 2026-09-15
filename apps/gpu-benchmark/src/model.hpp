// SPDX-License-Identifier: MIT
#pragma once
#include "core.hpp"
#include <GLES3/gl3.h>
#include <memory>

namespace gpu {
struct Mesh {
    std::vector<float> vertices;
    std::vector<unsigned> indices;
    std::vector<unsigned char> image;
    float color[4] = {1,1,1,1};
};
struct ModelData {
    std::vector<Mesh> meshes;
    std::string hash;
    size_t triangles = 0;
};
ModelData load_model(const std::string& filename);
std::string default_model();
class ModelRenderer {
    struct Object { GLuint vao=0,vbo=0,ibo=0,texture=0; GLsizei count=0; float color[4]={1,1,1,1}; };
    std::vector<Object> objects;
    GLuint program=0;
    int instances;
public:
    ModelRenderer(const ModelData& data,int count);
    ~ModelRenderer();
    void draw(int width,int height,float phase);
};
}
