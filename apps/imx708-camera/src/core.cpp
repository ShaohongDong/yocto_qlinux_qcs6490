// SPDX-License-Identifier: MIT
#include "core.hpp"
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <stdexcept>
#include <sys/statvfs.h>
#include <unistd.h>
#include <vector>

namespace imx708 {
static void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
void Session::preview() { mode = Mode::preview; }
bool Session::can_record() const { return mode == Mode::preview; }
bool Session::can_photo() const { return mode == Mode::preview; }
void Session::record() { require(can_record(), "Preview must be active before recording"); mode = Mode::recording; }
void Session::finish() { require(mode == Mode::recording, "No recording to finish"); mode = Mode::finishing; }
void Session::photo() { require(can_photo(), "Still capture is unavailable during recording"); mode = Mode::photo; }
void Session::saved() {
    require(mode == Mode::photo || mode == Mode::finishing, "No pending output");
    mode = Mode::preview;
}
Output::~Output() { close(); }
void Output::close() { if (fd_ >= 0) { ::close(fd_); fd_ = -1; } }
void Output::reserve(const std::filesystem::path& directory, const std::string& suffix) {
    require(fd_ < 0, "Output already open");
    require(suffix == ".jpg" || suffix == ".mp4", "Unsupported output type");
    std::filesystem::create_directories(directory);
    struct statvfs stats {};
    if (statvfs(directory.c_str(), &stats) != 0) throw std::runtime_error(std::strerror(errno));
    require(static_cast<unsigned long long>(stats.f_bavail) * stats.f_frsize >= 64ULL * 1024 * 1024,
            "Less than 64 MiB available in output directory");
    auto pattern = (directory / ("imx708-XXXXXX" + suffix + ".partial")).string();
    std::vector<char> name(pattern.begin(), pattern.end());
    name.push_back('\0');
    fd_ = mkstemps(name.data(), static_cast<int>(suffix.size() + 8));
    if (fd_ < 0) throw std::runtime_error(std::strerror(errno));
    fcntl(fd_, F_SETFD, FD_CLOEXEC);
    partial_ = name.data();
}
void Output::write(const void* data, size_t size) {
    require(opened(), "Output is not open");
    auto* bytes = static_cast<const char*>(data);
    while (size) {
        const auto count = ::write(fd_, bytes, size);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) throw std::runtime_error(std::strerror(errno));
        bytes += count;
        size -= static_cast<size_t>(count);
    }
}
std::filesystem::path Output::commit() {
    require(opened(), "Output is not open");
    if (fsync(fd_) != 0) throw std::runtime_error(std::strerror(errno));
    auto final = partial_;
    final.replace_extension(); // remove only .partial
    // link() refuses to replace an existing destination, unlike rename().
    if (link(partial_.c_str(), final.c_str()) != 0) throw std::runtime_error(std::strerror(errno));
    close();
    if (unlink(partial_.c_str()) != 0) throw std::runtime_error(std::strerror(errno));
    return final;
}
int self_test() {
    Session session;
    try {
        require(!session.can_record(), "Idle accepted recording");
        session.preview(); session.record();
        require(!session.can_photo(), "Recording accepted still capture");
        session.finish();
        require(!session.can_record(), "Finishing accepted another recording");
        session.saved(); session.photo(); session.saved();
        std::cout << "PASS: session self-test (no camera, ISP or display tested)\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
}
