// SPDX-License-Identifier: MIT
#pragma once
#include <gst/gst.h>
#include <string>
#include <vector>

namespace imx708 {
struct Device { unsigned id; std::string name; bool imx708; };
std::vector<Device> enumerate();
void autofocus(GstElement* camera);
void check_still_mode(GstElement* camera);
void capture_still(GstElement* camera);
void set_enum(GstElement* object, const char* property, const char* nick);
}
