/**
 * @file    shm_pair.cpp
 * @brief   共享内存通道双进程压测工具。
 *
 * 服务端把收到的消息原样回显；客户端用固定窗口发送带序号和校验模式的消息，
 * 逐条校验回显并输出吞吐与延迟分位数。
 *
 * 用法：
 *   shm_pair server <name> [count]           count=0 表示一直运行到 SIGTERM
 *   shm_pair client <name> [count] [payload] [window]
 *
 * payload 是应用数据长度；消息在通道里还会带 8 字节测试头，因此上限是 65528。
 */

#include "ipc/shared_memory.h"

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace {

using Channel = ara::com::ipc::SharedMemoryChannel;
using Bytes = std::vector<std::uint8_t>;
using Clock = std::chrono::steady_clock;

volatile std::sig_atomic_t g_stop = 0;

void on_signal(int) { g_stop = 1; }

std::int64_t now_us() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
               Clock::now().time_since_epoch())
        .count();
}

void append32(Bytes& out, std::uint32_t value) {
    for (int shift = 24; shift >= 0; shift -= 8) {
        out.push_back(static_cast<std::uint8_t>(value >> shift));
    }
}

std::uint32_t read32(const Bytes& data, std::size_t offset) {
    std::uint32_t value = 0;
    for (std::size_t i = 0; i < 4; ++i) {
        value = (value << 8) | data[offset + i];
    }
    return value;
}

Bytes make_message(std::uint32_t sequence, std::uint32_t payload_size) {
    Bytes message;
    message.reserve(8 + payload_size);
    append32(message, sequence);
    append32(message, payload_size);
    for (std::uint32_t i = 0; i < payload_size; ++i) {
        message.push_back(static_cast<std::uint8_t>((sequence + i) & 0xFFU));
    }
    return message;
}

bool verify_message(const Bytes& message, std::uint32_t sequence, std::uint32_t payload_size) {
    if (message.size() != 8U + payload_size) return false;
    if (read32(message, 0) != sequence) return false;
    if (read32(message, 4) != payload_size) return false;
    for (std::uint32_t i = 0; i < payload_size; ++i) {
        if (message[8U + i] != static_cast<std::uint8_t>((sequence + i) & 0xFFU)) {
            return false;
        }
    }
    return true;
}

int run_server(const std::string& name, std::uint32_t count) {
    Channel channel(Channel::Config{name});
    if (!channel.init_server()) {
        std::cerr << "server init failed: " << channel.last_error() << "\n";
        return 2;
    }

    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    std::uint32_t received = 0;
    auto last_activity = Clock::now();

    while (!g_stop && (count == 0 || received < count)) {
        Bytes message;
        if (!channel.wait_receive(message, 200)) {
            if (count != 0 && Clock::now() - last_activity > std::chrono::seconds(30)) {
                std::cerr << "server timeout waiting for client\n";
                return 3;
            }
            continue;
        }

        last_activity = Clock::now();
        if (message.size() < 8 || read32(message, 4) != message.size() - 8U) {
            std::cerr << "server received a malformed message\n";
            return 4;
        }

        const auto deadline = Clock::now() + std::chrono::seconds(10);
        while (!channel.send(message)) {
            if (g_stop) break;
            if (Clock::now() > deadline) {
                std::cerr << "server echo blocked (client not consuming)\n";
                return 5;
            }
            std::this_thread::sleep_for(std::chrono::microseconds(50));
        }
        ++received;
    }

    std::cout << "SERVER received=" << received << std::endl;
    return 0;
}

int run_client(const std::string& name, std::uint32_t count,
               std::uint32_t payload_size, std::uint32_t window) {
    Channel channel(Channel::Config{name});
    if (!channel.init_client()) {
        std::cerr << "client init failed: " << channel.last_error() << "\n";
        return 2;
    }

    if (window == 0) window = 1;

    std::vector<std::int64_t> sent_us(count, 0);
    std::vector<std::int64_t> latency_us;
    latency_us.reserve(count);

    std::uint32_t sent = 0;
    std::uint32_t received = 0;
    const auto start = Clock::now();
    auto last_progress = start;

    while (received < count) {
        while (sent < count && sent - received < window) {
            const Bytes message = make_message(sent, payload_size);
            if (!channel.send(message)) break;
            sent_us[sent] = now_us();
            ++sent;
        }

        Bytes echo;
        if (!channel.wait_receive(echo, 200)) {
            if (!channel.is_connected()) {
                std::cerr << "peer lost after " << received << " echoes\n";
                return 3;
            }
            if (Clock::now() - last_progress > std::chrono::seconds(30)) {
                std::cerr << "client timeout waiting for echo\n";
                return 4;
            }
            continue;
        }

        last_progress = Clock::now();
        const std::uint32_t sequence = read32(echo, 0);
        if (sequence != received || !verify_message(echo, sequence, payload_size)) {
            std::cerr << "echo mismatch at " << received << " (got " << sequence << ")\n";
            return 5;
        }

        latency_us.push_back(now_us() - sent_us[sequence]);
        ++received;
    }

    const auto elapsed = std::chrono::duration<double>(Clock::now() - start).count();
    std::sort(latency_us.begin(), latency_us.end());

    const auto percentile = [&](double ratio) {
        if (latency_us.empty()) return std::int64_t{0};
        const auto index = static_cast<std::size_t>(
            std::min<double>(ratio * static_cast<double>(latency_us.size() - 1),
                             static_cast<double>(latency_us.size() - 1)));
        return latency_us[index];
    };

    const double throughput_msg = static_cast<double>(count) / elapsed;
    const double throughput_mb =
        static_cast<double>(count) * static_cast<double>(8U + payload_size) /
        (1024.0 * 1024.0) / elapsed;

    std::cout << "RESULT total=" << count
              << " ok=" << received
              << " payload=" << payload_size
              << " seconds=" << elapsed
              << " throughput_msg_s=" << throughput_msg
              << " throughput_mb_s=" << throughput_mb
              << " p50_us=" << percentile(0.50)
              << " p99_us=" << percentile(0.99)
              << " max_us=" << latency_us.back()
              << std::endl;
    return 0;
}

void usage() {
    std::cerr << "Usage:\n"
              << "  shm_pair server <name> [count]                 count=0 表示一直运行\n"
              << "  shm_pair client <name> [count] [payload] [window]\n";
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        usage();
        return 2;
    }

    const std::string role = argv[1];
    const std::string name = argv[2];

    try {
        if (role == "server") {
            const std::uint32_t count =
                argc >= 4 ? static_cast<std::uint32_t>(std::stoul(argv[3])) : 100000U;
            return run_server(name, count);
        }
        if (role == "client") {
            const std::uint32_t count =
                argc >= 4 ? static_cast<std::uint32_t>(std::stoul(argv[3])) : 100000U;
            const std::uint32_t payload =
                argc >= 5 ? static_cast<std::uint32_t>(std::stoul(argv[4])) : 64U;
            const std::uint32_t window =
                argc >= 6 ? static_cast<std::uint32_t>(std::stoul(argv[5])) : 64U;

            if (count == 0 || count > 2000000U) {
                std::cerr << "count must be in [1, 2000000]\n";
                return 2;
            }
            if (payload > 65528U) {
                std::cerr << "payload must be <= 65528 (8-byte test header + payload <= 64 KiB)\n";
                return 2;
            }
            if (window == 0 || window > 4096U) {
                std::cerr << "window must be in [1, 4096]\n";
                return 2;
            }
            return run_client(name, count, payload, window);
        }
    } catch (const std::exception& error) {
        std::cerr << "invalid argument: " << error.what() << "\n";
        return 2;
    }

    usage();
    return 2;
}
