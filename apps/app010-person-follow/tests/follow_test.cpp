// SPDX-License-Identifier: MIT
#include "follow.hpp"
#include <iostream>
void check(bool ok) {if(!ok) throw std::runtime_error("Follow regression");}
int main() {
    ai::Box a{};a.x1=20;a.y1=10;a.x2=80;a.y2=90;a.score=.9;a.track_id=7;
    ai::Box b=a;b.x1=40;b.x2=60;b.track_id=8;
    ai::Follow f;check(std::string(f.update({a},0,100,100).state)=="unselected");
    f.select(7);auto r=f.update({a,b},0,100,100);check(r.offset && (*r.offset)[0]==0 && (*r.offset)[1]==0);
    check(std::string(f.update({b},.5,100,100).state)=="occluded");
    check(!f.update({b},.7,100,100).offset);
    check(std::string(f.update({a},1,100,100).state)=="tracking");
    check(std::string(f.update({a},2.01,100,100).state)=="lost");
    check(!f.update({a},2.02,100,100).offset); // same ID cannot revive expired selection
    f.select(8);check(f.update({b},3,100,100).offset.has_value());
    f.select(0);check(f.update({},0,100,100).id==0); // loop/restart reset
    f.select(99);check(std::string(f.update({a},10,100,100).state)=="waiting");
    check(ai::hit_test({a,b},100,100,400,200,200,100,.25)==8);
    check(ai::hit_test({a,b},100,100,400,200,25,100,.25)==0);
    check(ai::hit_test({a,b},100,100,400,200,200,100,.95)==0);
    check(ai::hit_test({},100,100,400,200,200,100,.25)==0);
    f.select(7);f.update({a},1,100,100);bool threw=false;
    try {f.update({a},.5,100,100);} catch(const std::runtime_error&) {threw=true;}check(threw);
    ai::FollowSelection click;
    f.select(0);click.queue(7,0);click.apply(f,1);
    check(f.update({a},0,100,100).id==0); // late click from previous loop ignored
    click.queue(7,1);click.apply(f,1);check(f.update({a},0,100,100).id==7);
    click.queue(0,1);click.apply(f,1);check(f.update({a},.1,100,100).id==0);
    click.queue(8,1);click.reset();click.apply(f,1);check(f.update({b},.2,100,100).id==0);
    click.queue(7,1,0.0);click.apply(f,1);
    check(std::string(f.update({},.1,100,100).state)=="occluded");
    check(std::string(f.update({},1.01,100,100).state)=="lost");
    click.queue(7,1,0.0);f.select(0);click.apply(f,2);
    check(f.update({a},0,100,100).id==0);
    std::cout<<"Follow state, timeout, selection and letterbox tests PASS\n";
}
