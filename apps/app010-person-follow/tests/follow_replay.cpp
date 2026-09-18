// SPDX-License-Identifier: MIT
#include "follow.hpp"
#include <iostream>
#include <iomanip>
int main() {
    try {
        ai::Follow follow;
        double pts;int w,h;int64_t selection;size_t n;
        while(std::cin>>pts>>w>>h>>selection>>n) {
            if(n>300 || selection< -1) throw std::runtime_error("Invalid replay input");
            if(selection>=0) follow.select(uint64_t(selection));
            std::vector<ai::Box> boxes(n);
            for(auto& b:boxes) std::cin>>b.track_id>>b.x1>>b.y1>>b.x2>>b.y2;
            if(!std::cin) throw std::runtime_error("Truncated replay input");
            auto f=follow.update(boxes,pts,w,h);
            std::cout<<std::setprecision(9)<<"{\"id\":"<<f.id<<",\"state\":\""<<f.state<<"\",\"offset\":";
            if(f.offset) std::cout<<'['<<(*f.offset)[0]<<','<<(*f.offset)[1]<<']';else std::cout<<"null";
            std::cout<<"}\n";
        }
        return 0;
    } catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
}
