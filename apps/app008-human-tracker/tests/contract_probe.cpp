// SPDX-License-Identifier: MIT
#include "core.hpp"
#include <fstream>
#include <iostream>
#include <iomanip>
#include <iterator>
int main(int argc,char** argv) {
    try {
        if(argc!=6 && argc!=7) throw std::runtime_error("mode width height input output [input_side]");
        int w=std::stoi(argv[2]),h=std::stoi(argv[3]);
        int side=argc==7?std::stoi(argv[6]):416;
        std::ifstream in(argv[4],std::ios::binary);if(!in) throw std::runtime_error("input");
        std::ofstream out(argv[5],std::ios::binary);if(!out) throw std::runtime_error("output");
        if(std::string(argv[1])=="preprocess") {
            std::vector<uint8_t> rgb((std::istreambuf_iterator<char>(in)),std::istreambuf_iterator<char>());
            auto result=ai::preprocess(rgb,w,h,side);out.write(reinterpret_cast<const char*>(result.data()),result.size()*sizeof(float));
        } else {
            std::vector<float> raw(ai::channels*ai::anchor_count(side));in.read(reinterpret_cast<char*>(raw.data()),raw.size()*sizeof(float));
            if(!in) throw std::runtime_error("tensor");
            out<<std::setprecision(9);
            for(auto& b:ai::decode(raw,false,ai::Letterbox(w,h,side),.001)) {
                out<<b.score<<' '<<b.x1<<' '<<b.y1<<' '<<b.x2<<' '<<b.y2;
                for(auto& p:b.keypoints) out<<' '<<p.x<<' '<<p.y<<' '<<p.confidence;
                out<<'\n';
            }
        }
        return out?0:1;
    } catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
