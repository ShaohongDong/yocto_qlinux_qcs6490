// SPDX-License-Identifier: MIT
#include "core.hpp"
#include "instance.hpp"
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <unistd.h>

static void check(bool value) { if (!value) throw std::runtime_error("Check failed"); }
int main() {
    char name[] = "/tmp/imx708-output-test-XXXXXX";
    auto* directory = mkdtemp(name);
    if (!directory) return 1;
    const std::filesystem::path root(directory);
    try {
        check(imx708::self_test() == 0);
        {
            imx708::InstanceLock first(root / "native.lock"); check(first.held());
            imx708::InstanceLock duplicate(root / "native.lock"); check(!duplicate.held());
        }
        { imx708::InstanceLock reopened(root / "native.lock"); check(reopened.held()); }
        imx708::Session session;
        bool rejected = false;
        try { session.photo(); } catch (const std::runtime_error&) { rejected = true; }
        check(rejected);
        session.preview(); session.record(); rejected = false;
        try { session.photo(); } catch (const std::runtime_error&) { rejected = true; }
        check(rejected);
        imx708::Output output;
        output.reserve(root, ".jpg");
        output.write("test", 4);
        const auto first = output.commit();
        check(std::filesystem::file_size(first) == 4);
        check(!std::filesystem::exists(output.partial()));
        output.reserve(root, ".jpg"); output.write("next", 4);
        check(output.commit() != first);
        output.reserve(root, ".mp4"); output.write("unfinished", 10);
        const auto partial = output.partial(); output.close();
        check(std::filesystem::exists(partial));
        auto final = partial; final.replace_extension();
        check(!std::filesystem::exists(final));
        rejected = false;
        try { output.reserve(first, ".jpg"); } catch (const std::exception&) { rejected = true; }
        check(rejected);
        output.reserve(root, ".jpg"); output.write("new", 3);
        final = output.partial(); final.replace_extension();
        std::ofstream(final) << "existing";
        rejected = false;
        try { output.commit(); } catch (const std::runtime_error&) { rejected = true; }
        check(rejected); check(std::filesystem::file_size(final) == 8);
        output.close();
        output.reserve(root, ".jpg");
        imx708::Output raw, metadata;
        raw.reserve_related(output.partial(), ".raw");
        metadata.reserve_related(output.partial(), ".json");
        raw.write("raw", 3); metadata.write("{}", 2); output.write("jpeg", 4);
        auto companion = output.partial(); companion.replace_extension(); companion.replace_extension(".raw");
        check(raw.commit() == companion); metadata.commit(); output.commit();
        output.reserve(root, ".jpg");
        raw.reserve_related(output.partial(), ".raw");
        rejected = false;
        try { metadata.reserve_related(output.partial(), ".raw"); } catch (const std::runtime_error&) { rejected = true; }
        check(rejected); raw.close(); output.close();
        std::filesystem::remove_all(root);
        std::cout << "PASS: illegal transitions, unique files, incomplete output, write errors, no overwrite\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n'; std::filesystem::remove_all(root); return 1;
    }
}
