// SPDX-License-Identifier: MIT
#include "core.hpp"
#include <iostream>
void require(bool ok) {if(!ok) throw std::runtime_error("Test failed");}
int main() {
    ai::Letterbox shape(1280,720);
    require(shape.top==140 && shape.left==0);
    auto b=shape.restore({0,140,640,500,.9f,0});require(b.x1==0 && b.y1==0 && b.x2==1280 && b.y2==720);
    auto clipped=shape.restore({-20,0,700,640,.5f,2});require(clipped.x1==0 && clipped.y1==0 && clipped.x2==1280 && clipped.y2==720);
    ai::Letterbox odd(333,777);require(odd.resized_height==640 && odd.left>0);
    require(ai::quantize(.5f,.01f,-10,255)==60 && ai::quantize(-10,.1f,0,255)==0 && ai::quantize(100,.1f,0,255)==255);
    std::vector<float> output(84*8400);
    auto box=[&](int i,int c,float score) {output[i]=320;output[8400+i]=320;output[2*8400+i]=100;output[3*8400+i]=100;output[(4+c)*8400+i]=score;};
    box(0,0,.9);box(1,0,.8);box(2,2,.85);
    auto boxes=ai::decode(output,false,shape,.25f);require(boxes.size()==2 && boxes[0].category==0 && boxes[1].category==2);
    std::vector<float> transposed(output.size());for(int i=0;i<8400;++i) for(int c=0;c<84;++c) transposed[i*84+c]=output[c*8400+i];
    auto other=ai::decode(transposed,true,shape,.25f);require(other.size()==boxes.size() && other[0].x1==boxes[0].x1);
    require(ai::decode(output,false,shape,.95f).empty());
    bool failed=false;try {ai::decode({},false,shape,.25f);} catch(...) {failed=true;}require(failed);
    std::cout<<"Letterbox, clipping, quantization, class-aware NMS and layout tests PASS\n";
}
