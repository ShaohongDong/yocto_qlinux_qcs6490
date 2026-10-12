// SPDX-License-Identifier: MIT
#include "raw_protocol.hpp"
#include "pcap.hpp"
#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <linux/if_packet.h>
#include <map>
#include <memory>
#include <net/if.h>
#include <poll.h>
#include <sstream>
#include <stdexcept>
#include <string>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <unordered_set>

namespace {
volatile sig_atomic_t stopped = 0;
void stop(int) { stopped = 1; }
using Clock = std::chrono::steady_clock;
uint64_t ns(Clock::time_point time) {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(time.time_since_epoch()).count();
}
struct Fd {
    int value = -1;
    ~Fd() { if (value >= 0) close(value); }
};
uint64_t integer(const std::string &value, uint64_t low, uint64_t high) {
    if (value.empty() || value.find_first_not_of("0123456789") != std::string::npos)
        throw std::invalid_argument("numeric argument must contain decimal digits");
    const auto number = std::stoull(value);
    if (number < low || number > high) throw std::invalid_argument("numeric argument outside supported range");
    return number;
}
void write_all(int fd, const std::string &text) {
    size_t offset = 0;
    while (offset < text.size()) {
        const auto count = write(fd, text.data() + offset, text.size() - offset);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) throw std::runtime_error("cannot write report");
        offset += count;
    }
}
void usage() {
    std::cout << "q6a-openhd-probe --self-test\n"
              << "q6a-openhd-probe tx|rx --interface NAME --run-id HEX32 --output FILE\n"
              << "  [--seconds 30] [--mbps 12] [--payload 1200] [--mcs 3]\n"
              << "  RX only: [--capture FILE.pcap] (new file, 256 MiB hard limit)\n"
              << "Interface must already be in monitor mode. RX needs an independent peer.\n"
              << "Reports are raw-frame observations; they do not qualify OpenHD/FEC or latency.\n";
}
}
int main(int argc, char **argv) {
    try {
        if (argc == 2 && std::string(argv[1]) == "--self-test") {
            if (!q6a::self_test()) return 1;
            std::cout << "raw protocol self-test passed; wireless remains unqualified\n";
            return 0;
        }
        if (argc == 1 || (argc == 2 && std::string(argv[1]) == "--help")) {
            usage(); return argc == 1 ? 2 : 0;
        }
        const std::string mode = argv[1];
        if (mode != "tx" && mode != "rx") throw std::invalid_argument("mode must be tx or rx");
        std::string interface, run_id, output, capture_path;
        uint64_t seconds = 30, mbps = 12, payload = 1200, mcs = 3;
        for (int i = 2; i < argc; i += 2) {
            if (i + 1 == argc) throw std::invalid_argument("missing option value");
            const std::string key = argv[i], value = argv[i+1];
            if (key == "--interface") interface = value;
            else if (key == "--run-id") run_id = value;
            else if (key == "--output") output = value;
            else if (key == "--seconds") seconds = integer(value, 1, 1800);
            else if (key == "--mbps") mbps = integer(value, 1, 24);
            else if (key == "--payload") payload = integer(value, 64, 1400);
            else if (key == "--mcs") mcs = integer(value, 0, 7);
            else if (key == "--capture") {
                if (value.empty()) throw std::invalid_argument("capture path must not be empty");
                capture_path = value;
            }
            else throw std::invalid_argument("unknown option " + key);
        }
        if (interface.empty() || interface.size() >= IFNAMSIZ || output.empty())
            throw std::invalid_argument("interface and output are required");
        if (!capture_path.empty() && (mode != "rx" || capture_path == output))
            throw std::invalid_argument("capture is RX-only and must differ from the report path");
        // Bound duplicate tracking before transmitting or opening the report.
        if (mbps * 1000000 * seconds / (payload * 8) > 3000000)
            throw std::invalid_argument("trial exceeds three million packets; shorten it or increase payload");
        const auto id = q6a::parse_run_id(run_id);
        const unsigned index = if_nametoindex(interface.c_str());
        if (!index) throw std::runtime_error("interface does not exist");
        Fd sock{socket(AF_PACKET, SOCK_RAW | SOCK_CLOEXEC | SOCK_NONBLOCK, htons(0x0003))};
        if (sock.value < 0) throw std::runtime_error("raw socket requires root/CAP_NET_RAW");
        ifreq request{};
        std::memcpy(request.ifr_name, interface.c_str(), interface.size() + 1);
        if (ioctl(sock.value, SIOCGIFHWADDR, &request) < 0 || request.ifr_hwaddr.sa_family != 803)
            throw std::runtime_error("interface must have Radiotap monitor type (ARPHRD 803)");
        if (ioctl(sock.value, SIOCGIFFLAGS, &request) < 0 || !(request.ifr_flags & IFF_UP))
            throw std::runtime_error("monitor interface is down");
        sockaddr_ll address{};
        address.sll_family = AF_PACKET;
        address.sll_protocol = htons(0x0003);
        address.sll_ifindex = static_cast<int>(index);
        if (bind(sock.value, reinterpret_cast<sockaddr *>(&address), sizeof(address)) < 0)
            throw std::runtime_error("cannot bind raw socket");
        // Match the pinned OpenHD wifibroadcast TX socket option. This does
        // not bypass netdevice queue state or prove firmware transmission.
        if (mode == "tx") {
            const int enabled = 1;
            if (setsockopt(sock.value, SOL_PACKET, PACKET_QDISC_BYPASS,
                           &enabled, sizeof(enabled)) < 0)
                throw std::runtime_error("cannot enable TX qdisc bypass");
        }
        Fd report{open(output.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644)};
        if (report.value < 0) throw std::runtime_error("output must be a new writable file");
        std::unique_ptr<q6a::PcapCapture> capture;
        if (!capture_path.empty()) capture = std::make_unique<q6a::PcapCapture>(capture_path);
        signal(SIGINT, stop); signal(SIGTERM, stop);
        const auto start = Clock::now(), finish = start + std::chrono::seconds(seconds);
        uint64_t attempts = 0, packets = 0, bytes = 0, errors = 0, duplicates = 0, rejected = 0;
        uint64_t highest = 0, reordered = 0;
        uint64_t partial_sends = 0;
        uint64_t truncated_frames = 0;
        std::map<int, uint64_t> socket_errno_counts;
        std::unordered_set<uint64_t> seen;
        bool fatal = false;
        if (mode == "tx") {
            const uint64_t interval = payload * 8 * 1000000000ULL / (mbps * 1000000ULL);
            auto next = start;
            while (!stopped && Clock::now() < finish) {
                auto frame = q6a::make_frame(id, attempts++, ns(Clock::now()), payload, mcs);
                const auto count = send(sock.value, frame.data(), frame.size(), 0);
                if (count == static_cast<ssize_t>(frame.size())) { ++packets; bytes += payload; }
                else {
                    ++errors;
                    if (count < 0) {
                        const int error = errno;
                        ++socket_errno_counts[error];
                        if (error != EAGAIN && error != ENOBUFS && error != EINTR) {
                            fatal = true; break;
                        }
                    } else {
                        ++partial_sends;
                        fatal = true; break;
                    }
                }
                // Never catch up by releasing a burst of old packets.
                next = std::max(next + std::chrono::nanoseconds(interval), Clock::now());
                std::this_thread::sleep_until(next);
            }
        } else {
            std::array<uint8_t, 8192> buffer{};
            while (!stopped && Clock::now() < finish) {
                pollfd ready{sock.value, POLLIN, 0};
                const auto result = poll(&ready, 1, 100);
                if (result < 0 && errno == EINTR) continue;
                if (result < 0 || (ready.revents & (POLLERR | POLLHUP | POLLNVAL))) {
                    if (result < 0) ++socket_errno_counts[errno];
                    ++errors; fatal = true; break;
                }
                if (result == 0) continue;
                sockaddr_ll from{}; socklen_t length = sizeof(from);
                const auto count = recvfrom(sock.value, buffer.data(), buffer.size(), MSG_TRUNC,
                    reinterpret_cast<sockaddr *>(&from), &length);
                if (count < 0) {
                    if (errno != EAGAIN && errno != EINTR) {
                        ++socket_errno_counts[errno];
                        ++errors; fatal = true; break;
                    }
                    continue;
                }
                // The local packet tap sees outgoing frames too. They are NOT peer evidence.
                if (from.sll_pkttype == PACKET_OUTGOING) continue;
                if (static_cast<size_t>(count) > buffer.size()) { ++truncated_frames; continue; }
                const auto packet = q6a::parse_frame(buffer.data(), static_cast<size_t>(count));
                if (!packet || packet->run_id != id) { ++rejected; continue; }
                if (capture) {
                    const auto timestamp = std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::system_clock::now().time_since_epoch()).count();
                    try {
                        capture->append(buffer.data(), static_cast<size_t>(count), timestamp);
                    } catch (const std::exception &error) {
                        std::cerr << "q6a-openhd-probe: " << error.what() << '\n';
                        fatal = true; break;
                    }
                }
                if (!seen.insert(packet->sequence).second) { ++duplicates; continue; }
                if (seen.size() > 3000000) { fatal = true; break; }
                if (packets && packet->sequence < highest) ++reordered;
                highest = std::max(highest, packet->sequence);
                ++packets; bytes += packet->payload_bytes;
            }
        }
        const double elapsed = std::chrono::duration<double>(Clock::now() - start).count();
        tpacket_stats stats{};
        socklen_t stats_size = sizeof(stats);
        const bool stats_available = mode == "rx" &&
            getsockopt(sock.value, SOL_PACKET, PACKET_STATISTICS, &stats, &stats_size) == 0;
        if (capture) {
            try { capture->finish(); }
            catch (const std::exception &error) {
                std::cerr << "q6a-openhd-probe: " << error.what() << '\n';
                fatal = true;
            }
        }
        const bool no_peer = mode == "rx" && !packets;
        const bool tx_errors = mode == "tx" && errors;
        std::ostringstream json;
        json << "{\n  \"schema_version\": 1, \"evidence\": \"raw_diagnostic_frames\",\n"
             << "  \"mode\": \"" << mode << "\", \"run_id\": \"" << run_id << "\",\n"
             << "  \"wireless_qualified\": false, \"openhd_fec_tested\": false,\n"
             << "  \"tx_qdisc_bypass\": " << (mode == "tx" ? "true" : "false") << ",\n"
             << "  \"elapsed_seconds\": " << elapsed << ", \"interrupted\": " << (stopped ? "true" : "false") << ",\n"
             << "  \"result\": \"" << (fatal ? "ERROR" : no_peer ? "NO_PEER_FRAMES" : tx_errors ? "TX_SOCKET_ERRORS" : mode == "tx" ? "TRANSMIT_ONLY" : "PEER_OBSERVATION") << "\",\n"
             << "  \"attempted_packets\": " << attempts << ", \"packets\": " << packets << ",\n"
             << "  \"payload_bytes\": " << bytes << ", \"payload_mbps\": " << (elapsed > 0 ? bytes * 8 / elapsed / 1000000 : 0) << ",\n"
             << "  \"socket_errors\": " << errors << ", \"partial_sends\": " << partial_sends << ",\n"
             << "  \"truncated_frames\": " << truncated_frames << ",\n"
             << "  \"rx_socket_statistics_available\": " << (stats_available ? "true" : "false") << ",\n"
             << "  \"rx_socket_packets\": " << stats.tp_packets << ", \"rx_socket_drops\": " << stats.tp_drops << ",\n"
             << "  \"capture_enabled\": " << (capture ? "true" : "false") << ",\n"
             << "  \"capture_packets\": " << (capture ? capture->packets() : 0) << ",\n"
             << "  \"capture_bytes\": " << (capture ? capture->bytes() : 0) << ",\n"
             << "  \"capture_complete\": " << (capture && !fatal && !capture->write_failed() && !capture->limit_reached() ? "true" : "false") << ",\n"
             << "  \"capture_limit_reached\": " << (capture && capture->limit_reached() ? "true" : "false") << ",\n"
             << "  \"capture_write_failed\": " << (capture && capture->write_failed() ? "true" : "false") << ",\n"
             << "  \"capture_limit_bytes\": " << q6a::PcapCapture::default_limit << ",\n"
             << "  \"capture_timestamp_clock\": \"receiver_realtime\",\n"
             << "  \"socket_errno_counts\": {";
        bool first_errno = true;
        for (const auto &[error, count] : socket_errno_counts) {
            if (!first_errno) json << ", ";
            json << "\"" << error << "\": " << count;
            first_errno = false;
        }
        json << "}, \"duplicate_packets\": " << duplicates << ",\n"
             << "  \"reordered_packets\": " << reordered << ", \"rejected_frames\": " << rejected << ",\n"
             << "  \"highest_sequence\": " << highest << ", \"requested_ht_mcs\": " << mcs << ",\n"
             << "  \"requested_seconds\": " << seconds << ", \"requested_payload_mbps\": " << mbps << ",\n"
             << "  \"requested_payload_size\": " << payload << ", \"driver_parameters_verified\": false\n}\n";
        write_all(report.value, json.str());
        return fatal || no_peer || tx_errors ? 1 : stopped ? 130 : 0;
    } catch (const std::exception &error) {
        std::cerr << "q6a-openhd-probe: " << error.what() << '\n'; return 2;
    }
}
