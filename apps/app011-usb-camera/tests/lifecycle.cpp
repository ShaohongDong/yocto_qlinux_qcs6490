// SPDX-License-Identifier: MIT
#define main usb_camera_main
#include "../src/main.cpp"
#undef main

static void require(bool value, const char* message) {
    if(!value) throw std::runtime_error(message);
}
int main() {
    try {
        gst_init(nullptr,nullptr);
        Preview p;p.headless=true;
        const camera::Mode mode{"MJPG",1920,1080,1,30};
        for(int cycle=0;cycle<3;++cycle) {
            require(p.play("",mode,true),"synthetic start failed");
            auto deadline=Clock::now()+std::chrono::seconds(3);
            while(p.frames<5 && Clock::now()<deadline) {require(p.tick(),"unexpected stream failure");g_usleep(1000);}
            require(p.frames>=5,"no frames");p.stop();
            require(!p.pipeline&&!p.sink&&!p.bus&&!p.pixels,"stop retained resources");
        }
        require(p.play("",mode,true),"EOS setup failed");
        gst_element_send_event(p.pipeline,gst_event_new_eos());
        auto deadline=Clock::now()+std::chrono::seconds(3);
        while(p.pipeline && Clock::now()<deadline) {p.tick();g_usleep(1000);}
        require(p.failed&&!p.pipeline,"EOS did not release pipeline");
        require(p.play("",mode,true),"restart after EOS failed");
        p.last=Clock::now()-std::chrono::seconds(6);
        gst_element_set_state(p.pipeline,GST_STATE_PAUSED);
        while(auto* sample=gst_app_sink_try_pull_sample(p.sink,0)) gst_sample_unref(sample);
        require(!p.tick()&&p.failed&&!p.pipeline,"timeout did not release pipeline");
        require(!p.play("/dev/q6a-usb-camera-nonexistent",mode),"invalid device accepted");
        std::cout<<"Lifecycle PASS: restart, EOS, timeout, invalid device, release\n";
        return 0;
    } catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
}
