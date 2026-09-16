// SPDX-License-Identifier: MIT
#include "tracker.hpp"
#include <iostream>
#include <iomanip>
int main(int argc,char** argv) {
    try {
        if(argc!=1&&argc!=5&&argc!=6) throw std::runtime_error("tracker-replay [buffer high new match [fuse_score]]");
        ai::Tracker tracker(argc>=5?std::stoi(argv[1]):30,argc>=5?std::stof(argv[2]):.25f,
                            argc>=5?std::stof(argv[3]):.35f,argc>=5?std::stof(argv[4]):.8f,argc==6&&std::stoi(argv[5])!=0);
        uint64_t frame;size_t count;
        while(std::cin>>frame>>count) {
            if(count>300) throw std::runtime_error("Too many detections");
            std::vector<ai::Box> boxes(count);
            for(auto& b:boxes) {
                std::cin>>b.x1>>b.y1>>b.x2>>b.y2>>b.score;
                for(auto& p:b.keypoints) std::cin>>p.x>>p.y>>p.confidence;
            }
            if(!std::cin) throw std::runtime_error("Truncated frame");
            auto out=tracker.update(boxes,frame);
            std::cout<<std::setprecision(9)<<"{\"source_frame\":"<<frame<<",\"persons\":[";
            bool first=true;
            for(const auto& b:out) {
                if(!first) std::cout<<',';
                first=false;
                std::cout<<"{\"track_id\":"<<b.track_id<<",\"score\":"<<b.score<<",\"box\":["<<b.x1<<','<<b.y1<<','<<b.x2<<','<<b.y2<<"],\"keypoints\":[";
                for(size_t k=0;k<17;++k) {if(k)std::cout<<',';auto& p=b.keypoints[k];std::cout<<'['<<p.x<<','<<p.y<<','<<p.confidence<<']';}
                std::cout<<"]}";
            }
            std::cout<<"]}\n";
        }
        return 0;
    } catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
}
