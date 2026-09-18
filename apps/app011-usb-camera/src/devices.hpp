// SPDX-License-Identifier: MIT
#pragma once
#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <unistd.h>
#include <filesystem>
#include <algorithm>
#include <string>
#include <vector>
#include <tuple>
namespace camera {
struct Mode {
    std::string format;
    unsigned width, height, numerator, denominator;
    std::string label() const {
        return format+" "+std::to_string(width)+"×"+std::to_string(height)+"  "+
            std::to_string(static_cast<double>(denominator)/numerator)+" fps";
    }
};
struct Device { std::string path, name; std::vector<Mode> modes; };
inline bool capture(unsigned caps, const std::string& bus) {
    return (caps & V4L2_CAP_VIDEO_CAPTURE) && (caps & V4L2_CAP_STREAMING) && bus.rfind("usb-",0)==0;
}
inline auto rank(const Mode& m) {
    return std::make_tuple(m.format=="MJPG" && m.width==1920 && m.height==1080 && m.denominator==30*m.numerator,
        m.format=="MJPG", uint64_t(m.width)*m.height, double(m.denominator)/m.numerator);
}
inline std::vector<Device> discover() {
    std::vector<Device> result;
    for (const auto& entry: std::filesystem::directory_iterator("/sys/class/video4linux")) {
        Device d{"/dev/"+entry.path().filename().string(), "", {}};
        int fd=open(d.path.c_str(), O_RDWR|O_NONBLOCK|O_CLOEXEC);
        if(fd<0) continue;
        v4l2_capability c{};
        if(ioctl(fd,VIDIOC_QUERYCAP,&c)<0 || !capture(c.capabilities&V4L2_CAP_DEVICE_CAPS?c.device_caps:c.capabilities,
            reinterpret_cast<char*>(c.bus_info))) { close(fd); continue; }
        d.name=reinterpret_cast<char*>(c.card);
        v4l2_fmtdesc f{}; f.type=V4L2_BUF_TYPE_VIDEO_CAPTURE;
        for(;ioctl(fd,VIDIOC_ENUM_FMT,&f)==0;++f.index) {
            std::string fmt;
            if(f.pixelformat==V4L2_PIX_FMT_MJPEG) fmt="MJPG";
            else if(f.pixelformat==V4L2_PIX_FMT_YUYV) fmt="YUYV";
            else if(f.pixelformat==V4L2_PIX_FMT_NV12) fmt="NV12";
            else continue;
            v4l2_frmsizeenum sz{}; sz.pixel_format=f.pixelformat;
            for(;ioctl(fd,VIDIOC_ENUM_FRAMESIZES,&sz)==0;++sz.index) {
                if(sz.type!=V4L2_FRMSIZE_TYPE_DISCRETE) continue;
                v4l2_frmivalenum iv{}; iv.pixel_format=f.pixelformat; iv.width=sz.discrete.width; iv.height=sz.discrete.height;
                for(;ioctl(fd,VIDIOC_ENUM_FRAMEINTERVALS,&iv)==0;++iv.index) {
                    if(iv.type!=V4L2_FRMIVAL_TYPE_DISCRETE || !iv.discrete.numerator || !iv.discrete.denominator) continue;
                    Mode m{fmt,iv.width,iv.height,iv.discrete.numerator,iv.discrete.denominator};
                    if(std::none_of(d.modes.begin(),d.modes.end(),[&](const Mode& x){return x.label()==m.label();})) d.modes.push_back(m);
                }
            }
        }
        close(fd);
        std::sort(d.modes.begin(),d.modes.end(),[](const Mode& a,const Mode& b){return rank(a)>rank(b);});
        if(!d.modes.empty()) result.push_back(d);
    }
    std::sort(result.begin(),result.end(),[](const Device& a,const Device& b){return a.path<b.path;});
    return result;
}
}
