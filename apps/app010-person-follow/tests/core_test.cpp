// SPDX-License-Identifier: MIT
#include "core.hpp"
#include "tracker.hpp"
#include <iostream>
#include <limits>
void require(bool ok) {if(!ok) throw std::runtime_error("Test failed");}
ai::Box box(float x,float score=.9) {ai::Box b;b.x1=x;b.y1=0;b.x2=x+40;b.y2=100;b.score=score;b.keypoints[0]={x+20,10,1};return b;}
int main() {
    ai::Letterbox shape(1280,720);require(shape.top==91&&shape.resized_height==234);
    auto b=shape.restore(ai::Box{0,91,416,325,.9f,0});require(b.x2==1280&&b.y1==0&&b.y2==720);
    std::vector<float> output(ai::anchors*ai::channels);
    for(size_t i=0;i<2;++i) {
        output[i]=208;output[ai::anchors+i]=208;output[2*ai::anchors+i]=100;output[3*ai::anchors+i]=200;output[4*ai::anchors+i]=.9f-.1f*i;
        for(size_t k=0;k<17;++k) {output[(5+3*k)*ai::anchors+i]=208;output[(6+3*k)*ai::anchors+i]=208;output[(7+3*k)*ai::anchors+i]=.8;}
    }
    auto poses=ai::decode(output,false,shape,.1);require(poses.size()==1&&poses[0].keypoints[0].x==640&&poses[0].keypoints[0].y==360);
    std::vector<float> transposed(output.size());for(size_t i=0;i<ai::anchors;++i)for(size_t c=0;c<ai::channels;++c)transposed[i*ai::channels+c]=output[c*ai::anchors+i];
    require(ai::decode(transposed,true,shape,.1)[0].keypoints[16].x==640);
    output[4*ai::anchors]=std::numeric_limits<float>::quiet_NaN();require(ai::decode(output,false,shape,.85).empty());
    bool failed=false;try {ai::decode({},false,shape,.1);}catch(...) {failed=true;}require(failed);
    ai::Tracker t;auto a=t.update({box(0),box(200)},0);require(a.size()==2);auto id=a[0].track_id;
    a=t.update({box(202),box(2,.2)},1);require(a.size()==2);
    bool found=false;for(const auto& p:a)if(p.track_id==id) {found=true;require(p.keypoints[0].x==22&&p.score==.2f);}require(found);
    require(t.update({},2).empty());a=t.update({box(6),box(206)},3);require(a.size()==2&&a[0].track_id==id);
    require(t.update({},50).empty());t.update({box(6)},51);a=t.update({box(6)},52);require(a.size()==1&&a[0].track_id!=id);
    ai::Tracker reset;require(reset.update({box(0)},0)[0].track_id==1);
    ai::Tracker moving;
    auto moving_id=moving.update({box(0)},0)[0].track_id;
    for(uint64_t f=1;f<=5;++f) require(moving.update({box(float(f*10))},f)[0].track_id==moving_id);
    for(uint64_t f=6;f<=8;++f) require(moving.update({},f).empty());
    auto recovered=moving.update({box(90)},9);
    require(recovered.size()==1&&recovered[0].track_id==moving_id);
    require(recovered[0].keypoints[0].x==110); // Current detection, not predicted pose.
    ai::Tracker fused, plain(30,.25f,.35f,.8f,false);
    auto fused_id=fused.update({box(0)},0)[0].track_id;
    auto plain_id=plain.update({box(0)},0)[0].track_id;
    auto fused_out=fused.update({box(0,.26f),box(4,.9f)},1);
    auto plain_out=plain.update({box(0,.26f),box(4,.9f)},1);
    require(fused_out.size()==1&&fused_out[0].track_id==fused_id&&fused_out[0].keypoints[0].x==24);
    require(plain_out.size()==1&&plain_out[0].track_id==plain_id&&plain_out[0].keypoints[0].x==20);
    failed=false;try {ai::Tracker bad(30,std::numeric_limits<float>::quiet_NaN());}catch(...) {failed=true;}require(failed);
    require(ai::quantize(.5f,.01f,-10,255)==60);
    auto pixels=ai::preprocess(std::vector<uint8_t>(3*7*5,255),7,5);require(pixels.size()==416*416*3);
    require(pixels[(208*416+208)*3]==1&&pixels[0]==114.f/255);
    ai::Letterbox hd(1280,720,640);require(hd.top==140&&hd.resized_height==360);
    auto hd_box=hd.restore(ai::Box{0,140,640,500,.9f,0});require(hd_box.x2==1280&&hd_box.y1==0&&hd_box.y2==720);
    require(ai::anchor_count(640)==8400);
    require(ai::preprocess(std::vector<uint8_t>(3*7*5,255),7,5,640).size()==640*640*3);
    failed=false;try {ai::decode(output,false,hd,.1);}catch(...) {failed=true;}require(failed);
    failed=false;try {ai::Letterbox unsupported(10,10,512);}catch(...) {failed=true;}require(failed);
    std::cout<<"Pose decode, keypoint restore, layout, finite checks, two-stage tracking, occlusion, gap expiration and resize PASS\n";
}
