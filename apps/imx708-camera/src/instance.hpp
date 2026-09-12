// SPDX-License-Identifier: MIT
#pragma once
#include <cerrno>
#include <filesystem>
#include <stdexcept>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

namespace imx708 {
class InstanceLock {
    int fd_ = -1;
    bool held_ = false;
public:
    explicit InstanceLock(const std::filesystem::path& path) {
        fd_ = open(path.c_str(), O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
        if (fd_ < 0) throw std::runtime_error("Cannot open native preview instance lock");
        held_ = flock(fd_, LOCK_EX | LOCK_NB) == 0;
        if (!held_ && errno != EWOULDBLOCK && errno != EAGAIN) {
            close(fd_); fd_ = -1;
            throw std::runtime_error("Cannot lock native preview instance");
        }
    }
    ~InstanceLock() { if (fd_ >= 0) close(fd_); }
    InstanceLock(const InstanceLock&) = delete;
    InstanceLock& operator=(const InstanceLock&) = delete;
    bool held() const { return held_; }
};
}
