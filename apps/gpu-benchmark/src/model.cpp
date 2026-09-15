// SPDX-License-Identifier: MIT
#include "model.hpp"
#include "third_party/cgltf.h"
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>
#include <stdexcept>

namespace gpu {
static void need(bool ok,const char* message) { if(!ok)throw std::runtime_error(message); }
std::string default_model() {
    auto bin=std::filesystem::read_symlink("/proc/self/exe").parent_path();
    for(auto candidate:{bin/"models/FlightHelmet.glb",bin/"../share/gpu-benchmark/models/FlightHelmet.glb"})
        if(std::filesystem::is_regular_file(candidate))return candidate.lexically_normal().string();
    throw std::runtime_error("FlightHelmet.glb not installed; supply --model PATH.glb");
}
ModelData load_model(const std::string& filename) {
    auto size=std::filesystem::file_size(filename);
    need(size>20 && size<=128*1024*1024,"Model must be a GLB between 20 bytes and 128 MiB");
    std::ifstream file(filename,std::ios::binary);std::vector<unsigned char> bytes(size);
    file.read(reinterpret_cast<char*>(bytes.data()),bytes.size());need(bool(file),"Cannot read model");
    cgltf_options options{};cgltf_data* raw=nullptr;
    need(cgltf_parse(&options,bytes.data(),bytes.size(),&raw)==cgltf_result_success,"Cannot parse glTF model");
    std::unique_ptr<cgltf_data,decltype(&cgltf_free)> data(raw,cgltf_free);
    need(data->file_type==cgltf_file_type_glb,"Only self-contained GLB models are supported");
    need(data->extensions_required_count==0,"Model requires unsupported glTF extensions");
    for(size_t i=0;i<data->buffers_count;i++)need(!data->buffers[i].uri,"External model buffers are unsupported");
    need(cgltf_load_buffers(&options,data.get(),nullptr)==cgltf_result_success,"Cannot load GLB buffers");
    need(cgltf_validate(data.get())==cgltf_result_success,"Invalid model accessor or buffer ranges");
    ModelData result;
    gchar* hash=g_compute_checksum_for_data(G_CHECKSUM_SHA256,bytes.data(),bytes.size());result.hash=hash;g_free(hash);
    float low[3]={INFINITY,INFINITY,INFINITY},high[3]={-INFINITY,-INFINITY,-INFINITY};
    for(size_t n=0;n<data->nodes_count;n++) {
        const auto& node=data->nodes[n];if(!node.mesh)continue;
        need(!node.skin,"Skinned models are unsupported");
        float world[16];cgltf_node_transform_world(&node,world);
        float a=world[0],b=world[4],c=world[8],d=world[1],e=world[5],f=world[9],g=world[2],h=world[6],i=world[10];
        float det=a*(e*i-f*h)-b*(d*i-f*g)+c*(d*h-e*g);need(std::abs(det)>1e-12,"Singular model transform");
        float normal[9]={(e*i-f*h)/det,(f*g-d*i)/det,(d*h-e*g)/det,(c*h-b*i)/det,(a*i-c*g)/det,(b*g-a*h)/det,(b*f-c*e)/det,(c*d-a*f)/det,(a*e-b*d)/det};
        for(size_t p=0;p<node.mesh->primitives_count;p++) {
            const auto& primitive=node.mesh->primitives[p];
            need(primitive.type==cgltf_primitive_type_triangles && primitive.targets_count==0,"Only static triangle meshes are supported");
            const cgltf_accessor *positions=nullptr,*normals=nullptr,*uvs=nullptr;
            for(size_t j=0;j<primitive.attributes_count;j++) {
                auto attr=primitive.attributes[j];
                if(attr.type==cgltf_attribute_type_position)positions=attr.data;
                if(attr.type==cgltf_attribute_type_normal)normals=attr.data;
                if(attr.type==cgltf_attribute_type_texcoord && attr.index==0)uvs=attr.data;
            }
            need(positions && normals && positions->count==normals->count,"Model needs matching POSITION and NORMAL attributes");
            need(positions->count<=2000000 && (!uvs || uvs->count==positions->count),"Invalid or excessive vertex count");
            Mesh mesh;
            for(size_t j=0;j<positions->count;j++) {
                float point[3],norm[3],uv[2]={0,0};
                need(cgltf_accessor_read_float(positions,j,point,3) && cgltf_accessor_read_float(normals,j,norm,3),"Cannot decode model vertices");
                if(uvs)need(cgltf_accessor_read_float(uvs,j,uv,2),"Cannot decode texture coordinates");
                float transformed[3],direction[3];
                for(int k=0;k<3;k++) {
                    transformed[k]=world[k]*point[0]+world[4+k]*point[1]+world[8+k]*point[2]+world[12+k];
                    direction[k]=normal[k*3]*norm[0]+normal[k*3+1]*norm[1]+normal[k*3+2]*norm[2];
                    need(std::isfinite(transformed[k]) && std::isfinite(direction[k]),"Non-finite model coordinates");
                    low[k]=std::min(low[k],transformed[k]);high[k]=std::max(high[k],transformed[k]);
                }
                mesh.vertices.insert(mesh.vertices.end(),{transformed[0],transformed[1],transformed[2],direction[0],direction[1],direction[2],uv[0],uv[1]});
            }
            size_t count=primitive.indices?primitive.indices->count:positions->count;
            need(count<=6000000 && count%3==0,"Invalid triangle index count");
            for(size_t j=0;j<count;j++) {
                auto index=primitive.indices?cgltf_accessor_read_index(primitive.indices,j):j;
                need(index<positions->count,"Model index out of range");mesh.indices.push_back(index);
            }
            if(primitive.material) {
                const auto& material=*primitive.material;
                need(material.alpha_mode==cgltf_alpha_mode_opaque,"Transparent and masked materials are unsupported");
                const auto& pbr=material.pbr_metallic_roughness;
                std::copy(pbr.base_color_factor,pbr.base_color_factor+4,mesh.color);
                if(pbr.base_color_texture.texture) {
                    need(uvs && !pbr.base_color_texture.has_transform,"Base-color UV0 required; texture transforms unsupported");
                    auto* image=pbr.base_color_texture.texture->image;
                    need(image && image->buffer_view && !image->uri,"Textures must be embedded in the GLB");
                    auto* image_bytes=cgltf_buffer_view_data(image->buffer_view);
                    need(image_bytes && image->buffer_view->size<=64*1024*1024,"Invalid embedded texture");
                    mesh.image.assign(image_bytes,image_bytes+image->buffer_view->size);
                }
            }
            result.triangles+=count/3;need(result.triangles<=2000000,"Model exceeds 2 million triangles");
            result.meshes.push_back(std::move(mesh));
        }
    }
    need(result.triangles>0,"Model contains no triangles");
    float extent=std::max({high[0]-low[0],high[1]-low[1],high[2]-low[2]});need(extent>1e-8,"Degenerate model bounds");
    for(auto& mesh:result.meshes)for(size_t j=0;j<mesh.vertices.size();j+=8)for(int k=0;k<3;k++)
        mesh.vertices[j+k]=(mesh.vertices[j+k]-(low[k]+high[k])*0.5f)*2.f/extent;
    return result;
}
static GLuint compile(GLenum type,const char* source) {
    GLuint shader=glCreateShader(type);glShaderSource(shader,1,&source,nullptr);glCompileShader(shader);
    GLint ok;glGetShaderiv(shader,GL_COMPILE_STATUS,&ok);
    if(!ok) { char log[2048]{};glGetShaderInfoLog(shader,sizeof(log),nullptr,log);glDeleteShader(shader);throw std::runtime_error(log); }
    return shader;
}
ModelRenderer::ModelRenderer(const ModelData& data,int count):instances(count) {
    const char* vertex=R"(#version 300 es
layout(location=0) in vec3 position;
layout(location=1) in vec3 normal;
layout(location=2) in vec2 texcoord;
uniform float phase,aspect;
uniform int columns;
out vec3 N,P;
out vec2 uv;
void main() {
    float angle=phase*0.35+float(gl_InstanceID)*0.17;
    mat3 rotation=mat3(cos(angle),0.0,-sin(angle),0.0,1.0,0.0,sin(angle),0.0,cos(angle));
    vec3 p=rotation*position;
    p.xy+=vec2(float(gl_InstanceID%columns)-float(columns-1)*0.5,
               float(gl_InstanceID/columns)-float(columns-1)*0.5)*2.5;
    N=rotation*normal;P=p;uv=texcoord;
    float distance=3.4*float(columns)/min(aspect,1.0);
    float z=p.z-distance;
    float nearPlane=0.1,farPlane=distance+30.0;
    gl_Position=vec4(p.x*2.41421356/aspect,p.y*2.41421356,
      -(farPlane+nearPlane)/(farPlane-nearPlane)*z-2.0*farPlane*nearPlane/(farPlane-nearPlane),-z);
}
)";
    const char* fragment=R"(#version 300 es
precision highp float;
in vec3 N,P;
in vec2 uv;
uniform sampler2D baseTexture;
uniform vec4 baseColor;
out vec4 color;
void main() {
    vec3 n=normalize(N);
    if(!gl_FrontFacing)n=-n;
    vec3 light=normalize(vec3(3.0,4.0,5.0));
    vec3 fill=normalize(vec3(-4.0,1.0,2.0));
    float diffuse=0.20+0.65*max(dot(n,light),0.0)+0.30*max(dot(n,fill),0.0);
    float spec=pow(max(dot(n,normalize(light+normalize(vec3(0,0,5)-P))),0.0),32.0)*0.18;
    vec3 linear=texture(baseTexture,uv).rgb*baseColor.rgb*diffuse+vec3(spec);
    color=vec4(pow(max(linear,vec3(0.0)),vec3(1.0/2.2)),1.0);
}
)";
    auto vs=compile(GL_VERTEX_SHADER,vertex),fs=compile(GL_FRAGMENT_SHADER,fragment);
    program=glCreateProgram();glAttachShader(program,vs);glAttachShader(program,fs);glLinkProgram(program);glDeleteShader(vs);glDeleteShader(fs);
    GLint linked;glGetProgramiv(program,GL_LINK_STATUS,&linked);need(linked,"3D shader link failed");
    for(const auto& mesh:data.meshes) {
        Object object;object.count=mesh.indices.size();std::copy(mesh.color,mesh.color+4,object.color);
        glGenVertexArrays(1,&object.vao);glBindVertexArray(object.vao);
        glGenBuffers(1,&object.vbo);glBindBuffer(GL_ARRAY_BUFFER,object.vbo);glBufferData(GL_ARRAY_BUFFER,mesh.vertices.size()*sizeof(float),mesh.vertices.data(),GL_STATIC_DRAW);
        glGenBuffers(1,&object.ibo);glBindBuffer(GL_ELEMENT_ARRAY_BUFFER,object.ibo);glBufferData(GL_ELEMENT_ARRAY_BUFFER,mesh.indices.size()*sizeof(unsigned),mesh.indices.data(),GL_STATIC_DRAW);
        for(int attr=0;attr<3;attr++) { glEnableVertexAttribArray(attr);glVertexAttribPointer(attr,attr==2?2:3,GL_FLOAT,GL_FALSE,8*sizeof(float),reinterpret_cast<void*>(static_cast<uintptr_t>((attr==0?0:attr==1?3:6)*sizeof(float)))); }
        int width=1,height=1;std::vector<unsigned char> pixels={255,255,255,255};
        if(!mesh.image.empty()) {
            GError* error=nullptr;auto* loader=gdk_pixbuf_loader_new();
            bool ok=gdk_pixbuf_loader_write(loader,mesh.image.data(),mesh.image.size(),&error);
            if(ok)ok=gdk_pixbuf_loader_close(loader,&error);
            if(!ok) { std::string message=error?error->message:"Texture decode failed";g_clear_error(&error);g_object_unref(loader);throw std::runtime_error(message); }
            auto* pixbuf=gdk_pixbuf_loader_get_pixbuf(loader);width=gdk_pixbuf_get_width(pixbuf);height=gdk_pixbuf_get_height(pixbuf);
            if(width>4096 || height>4096) { g_object_unref(loader);throw std::runtime_error("Model textures exceed 4096x4096"); }
            int channels=gdk_pixbuf_get_n_channels(pixbuf),stride=gdk_pixbuf_get_rowstride(pixbuf);auto* source=gdk_pixbuf_get_pixels(pixbuf);
            pixels.resize(width*height*4);
            for(int y=0;y<height;y++)for(int x=0;x<width;x++) { auto* from=source+y*stride+x*channels;auto* to=pixels.data()+(y*width+x)*4;std::copy(from,from+3,to);to[3]=channels==4?from[3]:255; }
            g_object_unref(loader);
        }
        glGenTextures(1,&object.texture);glBindTexture(GL_TEXTURE_2D,object.texture);
        glTexImage2D(GL_TEXTURE_2D,0,GL_SRGB8_ALPHA8,width,height,0,GL_RGBA,GL_UNSIGNED_BYTE,pixels.data());glGenerateMipmap(GL_TEXTURE_2D);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR_MIPMAP_LINEAR);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
        objects.push_back(object);
    }
    need(glGetError()==GL_NO_ERROR,"3D model upload failed");
}
ModelRenderer::~ModelRenderer() {
    for(auto& o:objects) { glDeleteTextures(1,&o.texture);glDeleteBuffers(1,&o.vbo);glDeleteBuffers(1,&o.ibo);glDeleteVertexArrays(1,&o.vao); }
    glDeleteProgram(program);
}
void ModelRenderer::draw(int width,int height,float phase) {
    glEnable(GL_DEPTH_TEST);glDepthFunc(GL_LESS);glDepthMask(GL_TRUE);glDisable(GL_CULL_FACE);glDisable(GL_BLEND);
    glClearColor(.025f,.04f,.065f,1);glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT);glUseProgram(program);
    glUniform1f(glGetUniformLocation(program,"phase"),phase);glUniform1f(glGetUniformLocation(program,"aspect"),float(width)/height);
    glUniform1i(glGetUniformLocation(program,"columns"),static_cast<int>(std::ceil(std::sqrt(instances))));
    glUniform1i(glGetUniformLocation(program,"baseTexture"),0);glActiveTexture(GL_TEXTURE0);
    for(const auto& o:objects) {
        glBindVertexArray(o.vao);glBindTexture(GL_TEXTURE_2D,o.texture);glUniform4fv(glGetUniformLocation(program,"baseColor"),1,o.color);
        glDrawElementsInstanced(GL_TRIANGLES,o.count,GL_UNSIGNED_INT,nullptr,instances);
    }
}
}
