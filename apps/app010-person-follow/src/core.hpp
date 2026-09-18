// SPDX-License-Identifier: MIT
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <vector>
namespace ai {
constexpr int side=416;
constexpr size_t anchors=3549,channels=56;
inline size_t anchor_count(int input_side) {
    if(input_side!=416 && input_side!=640) throw std::runtime_error("Supported pose input sizes are 416 and 640");
    return size_t(input_side/8)*(input_side/8)+size_t(input_side/16)*(input_side/16)+size_t(input_side/32)*(input_side/32);
}
struct Point {float x=0,y=0,confidence=0;};
struct Box {
    float x1=0,y1=0,x2=0,y2=0,score=0;
    int category=0;
    std::array<Point,17> keypoints{};
    uint64_t track_id=0;
    std::vector<Point> trail{};
};
struct Letterbox {
    int width,height,resized_width,resized_height,left,top,input_side;
    explicit Letterbox(int w,int h,int size=side):width(w),height(h),input_side(size) {
        if(w<=0||h<=0) throw std::runtime_error("Invalid image dimensions");
        anchor_count(input_side);
        double scale=std::min(double(input_side)/w,double(input_side)/h);
        resized_width=std::max(1,int(std::nearbyint(w*scale)));
        resized_height=std::max(1,int(std::nearbyint(h*scale)));
        left=(input_side-resized_width)/2;top=(input_side-resized_height)/2;
    }
    Point restore(Point p) const {
        p.x=std::clamp((p.x-left)*width/resized_width,0.f,float(width));
        p.y=std::clamp((p.y-top)*height/resized_height,0.f,float(height));return p;
    }
    Box restore(Box b) const {
        auto a=restore(Point{b.x1,b.y1,1}),z=restore(Point{b.x2,b.y2,1});
        b.x1=a.x;b.y1=a.y;b.x2=z.x;b.y2=z.y;
        for(auto& p:b.keypoints) p=restore(p);
        return b;
    }
};
inline float iou(const Box&a,const Box&b) {
    float intersection=std::max(0.f,std::min(a.x2,b.x2)-std::max(a.x1,b.x1))*std::max(0.f,std::min(a.y2,b.y2)-std::max(a.y1,b.y1));
    float area=(a.x2-a.x1)*(a.y2-a.y1)+(b.x2-b.x1)*(b.y2-b.y1)-intersection;return area>0?intersection/area:0;
}
inline std::vector<Box> decode(const std::vector<float>& output,bool last,const Letterbox& shape,float threshold) {
    const size_t anchors=anchor_count(shape.input_side);
    if(output.size()!=anchors*channels) throw std::runtime_error("Pose output shape does not match model input resolution");
    auto v=[&](size_t i,size_t c){return output[last?i*channels+c:c*anchors+i];};
    std::vector<Box> candidates,result,selected;
    for(size_t i=0;i<anchors;++i) {
        float x=v(i,0),y=v(i,1),w=v(i,2),h=v(i,3),score=v(i,4);
        if(!std::isfinite(x)||!std::isfinite(y)||!std::isfinite(w)||!std::isfinite(h)||!std::isfinite(score)||score<threshold||w<=0||h<=0) continue;
        Box b;b.x1=x-w/2;b.y1=y-h/2;b.x2=x+w/2;b.y2=y+h/2;b.score=std::min(score,1.f);
        for(size_t k=0;k<17;++k) {
            Point p{v(i,5+k*3),v(i,6+k*3),v(i,7+k*3)};
            if(!std::isfinite(p.x)||!std::isfinite(p.y)||!std::isfinite(p.confidence)) p={};
            p.confidence=std::clamp(p.confidence,0.f,1.f);b.keypoints[k]=p;
        }
        candidates.push_back(b);
    }
    std::stable_sort(candidates.begin(),candidates.end(),[](const Box&a,const Box&b){return a.score>b.score;});
    if(candidates.size()>3000) candidates.resize(3000);
    for(const auto& b:candidates) {
        bool suppressed=false;
        for(const auto& a:selected) if(iou(a,b)>.7f) {suppressed=true;break;}
        if(suppressed) continue;
        selected.push_back(b);auto r=shape.restore(b);
        if(r.x2>r.x1&&r.y2>r.y1) result.push_back(r);
        if(selected.size()>=300) break;
    }
    return result;
}
inline uint32_t quantize(float value,float scale,int32_t offset,uint32_t maximum) {
    if(!(scale>0)||!std::isfinite(scale)||!std::isfinite(value)) throw std::runtime_error("Invalid quantization parameters/input");
    return uint32_t(std::clamp(std::round(double(value)/scale)-offset,0.,double(maximum)));
}
inline const char* label(int) {return "person";}
// Half-pixel bilinear RGB resize without antialiasing, rounded to uint8.
inline std::vector<float> preprocess(const std::vector<uint8_t>& rgb,int width,int height,int side=ai::side) {
    Letterbox s(width,height,side);
    if(rgb.size()!=size_t(width)*height*3) throw std::runtime_error("Invalid RGB buffer");
    std::vector<float> out(side*side*3,114.f/255);
    for(int y=0;y<s.resized_height;++y) {
        double sy=std::max(0.,(y+.5)*height/s.resized_height-.5);
        int y0=std::min(int(sy),height-1),y1=std::min(y0+1,height-1);double dy=sy-y0;
        for(int x=0;x<s.resized_width;++x) {
            double sx=std::max(0.,(x+.5)*width/s.resized_width-.5);
            int x0=std::min(int(sx),width-1),x1=std::min(x0+1,width-1);double dx=sx-x0;
            for(int c=0;c<3;++c) {
                double a=rgb[(size_t(y0)*width+x0)*3+c]*(1-dx)+rgb[(size_t(y0)*width+x1)*3+c]*dx;
                double b=rgb[(size_t(y1)*width+x0)*3+c]*(1-dx)+rgb[(size_t(y1)*width+x1)*3+c]*dx;
                out[((y+s.top)*side+x+s.left)*3+c]=float(std::floor(a*(1-dy)+b*dy+.5)/255);
            }
        }
    }
    return out;
}
}
