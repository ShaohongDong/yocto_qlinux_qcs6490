// SPDX-License-Identifier: MIT
#pragma once
#include "core.hpp"
#include "ByteTrack/BYTETracker.h"
#include <map>
namespace ai {
class Tracker {
    byte_track::BYTETracker tracker;
    uint64_t buffer;
    std::map<uint64_t,std::vector<Point>> trails;
    std::map<uint64_t,uint64_t> last_seen;
    uint64_t last_frame=0;
    bool first=true;
public:
    explicit Tracker(int lost_buffer=30,float high=.25,float new_track=.35,float match=.8,bool fuse_score=true):
        tracker(30,lost_buffer,high,new_track,match,fuse_score),buffer(lost_buffer) {
        if(!std::isfinite(high)||!std::isfinite(new_track)||!std::isfinite(match)||high<=0||high>=1||new_track<high||new_track>=1||match<=0||match>=1) throw std::runtime_error("Invalid tracker thresholds");
        if(lost_buffer<1||lost_buffer>300) throw std::runtime_error("Invalid tracker buffer");
    }
    std::vector<Box> update(const std::vector<Box>& detections,uint64_t source_frame) {
        if(!first&&source_frame<=last_frame) throw std::runtime_error("Non-monotonic source frame; reset tracker at loop boundary");
        if(!first) {
            auto gap=std::min<uint64_t>(buffer+2,source_frame-last_frame-1);
            for(uint64_t i=0;i<gap;++i) tracker.update({});
        }
        first=false;last_frame=source_frame;
        std::vector<byte_track::Object> objects;
        for(const auto& b:detections) objects.emplace_back(byte_track::Rect<float>(b.x1,b.y1,b.x2-b.x1,b.y2-b.y1),0,b.score);
        std::vector<Box> result;
        for(const auto& t:tracker.update(objects)) {
            if(t->detection_index>=detections.size()) throw std::runtime_error("Tracker lost detection association");
            auto b=detections[t->detection_index];b.track_id=t->getTrackId();
            auto& trail=trails[b.track_id];trail.push_back({(b.x1+b.x2)/2,b.y2,1});
            if(trail.size()>90) trail.erase(trail.begin());
            b.trail=trail;last_seen[b.track_id]=source_frame;result.push_back(b);
        }
        for(auto i=last_seen.begin();i!=last_seen.end();) {
            if(source_frame-i->second>buffer+2) {trails.erase(i->first);i=last_seen.erase(i);} else ++i;
        }
        return result;
    }
};
}
