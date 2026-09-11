// SPDX-License-Identifier: MIT
#pragma once
#include <filesystem>
#include <string>

namespace imx708 {
enum class Mode { idle, preview, recording, finishing, photo, error };
struct Session {
    Mode mode = Mode::idle;
    void preview();
    void record();
    void finish();
    void photo();
    void saved();
    bool can_record() const;
    bool can_photo() const;
};

// Reserve a unique name without overwriting an existing file. The .partial
// file is kept after failures so an interrupted recording is never advertised
// as a completed MP4. A successful rename stays on the same filesystem.
class Output {
public:
    Output() = default;
    Output(const Output&) = delete;
    Output& operator=(const Output&) = delete;
    ~Output();
    void reserve(const std::filesystem::path& directory, const std::string& suffix);
    void write(const void* data, size_t size);
    std::filesystem::path commit();
    void close();
    const std::filesystem::path& partial() const { return partial_; }
    bool opened() const { return fd_ >= 0; }
private:
    int fd_ = -1;
    std::filesystem::path partial_;
};
int self_test();
}
