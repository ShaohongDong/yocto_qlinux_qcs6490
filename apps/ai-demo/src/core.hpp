// SPDX-License-Identifier: MIT
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace ai {
constexpr int side = 640;
struct Box { float x1, y1, x2, y2, score; int category; };
struct Letterbox {
    int width, height, resized_width, resized_height, left, top;
    explicit Letterbox(int w, int h) : width(w), height(h) {
        if (w <= 0 || h <= 0) throw std::runtime_error("Invalid image dimensions");
        double scale = std::min(double(side) / w, double(side) / h);
        resized_width = std::max(1, int(std::round(w * scale)));
        resized_height = std::max(1, int(std::round(h * scale)));
        left = (side - resized_width) / 2;
        top = (side - resized_height) / 2;
    }
    Box restore(Box b) const {
        b.x1 = std::clamp((b.x1 - left) * width / resized_width, 0.f, float(width));
        b.x2 = std::clamp((b.x2 - left) * width / resized_width, 0.f, float(width));
        b.y1 = std::clamp((b.y1 - top) * height / resized_height, 0.f, float(height));
        b.y2 = std::clamp((b.y2 - top) * height / resized_height, 0.f, float(height));
        return b;
    }
};
inline float iou(const Box& a, const Box& b) {
    float intersection = std::max(0.f, std::min(a.x2,b.x2)-std::max(a.x1,b.x1)) *
                         std::max(0.f, std::min(a.y2,b.y2)-std::max(a.y1,b.y1));
    float total = (a.x2-a.x1)*(a.y2-a.y1)+(b.x2-b.x1)*(b.y2-b.y1)-intersection;
    return total > 0 ? intersection / total : 0;
}
inline std::vector<Box> decode(const std::vector<float>& output, bool channels_last,
                               const Letterbox& shape, float threshold) {
    constexpr size_t count = 8400, channels = 84;
    if (output.size() != count * channels) throw std::runtime_error("Expected YOLOv8n output [1,84,8400] or [1,8400,84]");
    auto value = [&](size_t i, size_t c) { return output[channels_last ? i*channels+c : c*count+i]; };
    std::vector<Box> candidates, result;
    for (size_t i=0; i<count; ++i) {
        float score=0; int category=0;
        for (int c=0; c<80; ++c) if (value(i,c+4)>score) { score=value(i,c+4); category=c; }
        float x=value(i,0), y=value(i,1), w=value(i,2), h=value(i,3);
        if (score < threshold || !std::isfinite(score) || !std::isfinite(x) ||
            !std::isfinite(y) || !std::isfinite(w) || !std::isfinite(h) || w<=0 || h<=0) continue;
        candidates.push_back({x-w/2,y-h/2,x+w/2,y+h/2,score,category});
    }
    std::stable_sort(candidates.begin(), candidates.end(), [](auto a, auto b){return a.score>b.score;});
    // Bound postprocessing work even on malformed or very noisy outputs.
    if (candidates.size()>3000) candidates.resize(3000);
    std::vector<Box> selected;
    for (auto b : candidates) {
        bool suppressed=false;
        for (auto a : selected) if (a.category==b.category && iou(a,b)>.45f) {suppressed=true; break;}
        if (!suppressed) {
            selected.push_back(b);
            auto restored=shape.restore(b);
            if (restored.x2>restored.x1 && restored.y2>restored.y1) result.push_back(restored);
            if (selected.size()>=300) break;
        }
    }
    return result;
}
inline uint32_t quantize(float value, float scale, int32_t offset, uint32_t maximum) {
    if (!(scale>0) || !std::isfinite(scale) || !std::isfinite(value)) throw std::runtime_error("Invalid quantization parameters/input");
    return uint32_t(std::clamp(std::round(double(value)/scale)-offset, 0., double(maximum)));
}
inline const char* label(int c) {
    static const char* labels[] = {"person","bicycle","car","motorcycle","airplane","bus","train","truck","boat","traffic light","fire hydrant","stop sign","parking meter","bench","bird","cat","dog","horse","sheep","cow","elephant","bear","zebra","giraffe","backpack","umbrella","handbag","tie","suitcase","frisbee","skis","snowboard","sports ball","kite","baseball bat","baseball glove","skateboard","surfboard","tennis racket","bottle","wine glass","cup","fork","knife","spoon","bowl","banana","apple","sandwich","orange","broccoli","carrot","hot dog","pizza","donut","cake","chair","couch","potted plant","bed","dining table","toilet","tv","laptop","mouse","remote","keyboard","cell phone","microwave","oven","toaster","sink","refrigerator","book","clock","vase","scissors","teddy bear","hair drier","toothbrush"};
    return c>=0 && c<80 ? labels[c] : "unknown";
}
}
