// SPDX-License-Identifier: MIT
#pragma once
#include "core.hpp"
#include <optional>
#include <string>
#include <utility>
#include <limits>

namespace ai {
struct FollowResult {
    uint64_t id=0;
    const char* state="unselected";
    std::optional<std::array<double,2>> offset;
};
// All times are source presentation timestamps, never wall-clock time.
class Follow {
    uint64_t id_=0;
    bool seen_=false, lost_=false;
    double last_=0;
public:
    void select(uint64_t id,std::optional<double> observed_at=std::nullopt) {
        if(observed_at && (!std::isfinite(*observed_at)||*observed_at<0))
            throw std::runtime_error("Invalid selection timestamp");
        id_=id;seen_=id && observed_at.has_value();lost_=false;last_=observed_at.value_or(0);
    }
    FollowResult update(const std::vector<Box>& boxes,double pts,int width,int height) {
        if(!std::isfinite(pts)||pts<0||width<=0||height<=0)
            throw std::runtime_error("Invalid follow frame geometry or timestamp");
        if(!id_) return {};
        if(seen_ && pts<last_) throw std::runtime_error("Follow timestamp moved backwards; reset at loop boundary");
        if(seen_ && pts-last_>1.0) lost_=true;
        if(lost_) return {id_,"lost",std::nullopt};
        for(const auto& b:boxes) if(b.track_id==id_) {
            seen_=true;last_=pts;
            return {id_,"tracking",std::array<double,2>{
                std::clamp(double(b.x1+b.x2)/width-1.0,-1.0,1.0),
                std::clamp(double(b.y1+b.y2)/height-1.0,-1.0,1.0)}};
        }
        return {id_,seen_?"occluded":"waiting",std::nullopt};
    }
};
// A GUI click belongs to the displayed video loop. Discard stale clicks across
// restart boundaries instead of allowing a reused track ID to select someone else.
class FollowSelection {
    struct Request {uint64_t id,loop;std::optional<double> observed_at;};
    std::optional<Request> pending;
public:
    void queue(uint64_t id,uint64_t loop,std::optional<double> observed_at=std::nullopt) {pending=Request{id,loop,observed_at};}
    void reset() {pending.reset();}
    void apply(Follow& follow,uint64_t loop) {
        if(pending && pending->loop==loop) follow.select(pending->id,pending->observed_at);
        pending.reset();
    }
};
// Letterboxed widget coordinates. Background clicks do not cancel selection.
inline uint64_t hit_test(const std::vector<Box>& boxes,int iw,int ih,
                         double ww,double wh,double x,double y,float threshold) {
    if(iw<=0||ih<=0||ww<=0||wh<=0) return 0;
    double scale=std::min(ww/iw,wh/ih);
    x=(x-(ww-iw*scale)/2)/scale;y=(y-(wh-ih*scale)/2)/scale;
    if(x<0||y<0||x>=iw||y>=ih) return 0;
    uint64_t id=0;double area=std::numeric_limits<double>::infinity();
    for(const auto& b:boxes) {
        double a=(b.x2-b.x1)*(b.y2-b.y1);
        if(b.track_id&&b.score>=threshold&&a>0&&a<area&&x>=b.x1&&x<=b.x2&&y>=b.y1&&y<=b.y2) {area=a;id=b.track_id;}
    }
    return id;
}
inline const char* follow_label(const FollowResult& f) {
    std::string s=f.state;
    if(s=="tracking") return "跟踪中";
    if(s=="occluded") return "暂时遮挡";
    if(s=="lost") return "已丢失，请重新选择";
    if(s=="waiting") return "等待目标出现";
    return "未选择：点击人物锁定";
}
}
