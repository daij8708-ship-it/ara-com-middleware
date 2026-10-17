/**
 * @file    shm_rpc_pair.cpp
 * @brief   共享内存 RPC 后端双进程示例与验证工具。
 *
 * 服务端发布一个 Method 和一个 Event；客户端通过 ara::com 风格的
 * ServiceProxy 调用 Method 并订阅 Event。
 *
 * 用法：
 *   shm_rpc_pair server <service_id> [run_ms]
 *   shm_rpc_pair client <service_id> <client_id> [requests]
 */

#include "communication/runtime.h"
#include "communication/service_proxy.h"
#include "communication/service_skeleton.h"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace ara::com;
using namespace std::chrono_literals;

constexpr MethodId ECHO_METHOD = 0x0101;
constexpr EventId COUNT_EVENT = 0x8001;

volatile std::sig_atomic_t g_stop = 0;

void on_signal(int) { g_stop = 1; }

struct EchoRequest {
    std::string text;

    std::vector<std::uint8_t> serialize() const {
        return std::vector<std::uint8_t>(text.begin(), text.end());
    }

    static EchoRequest deserialize(const std::vector<std::uint8_t>& data) {
        EchoRequest request;
        request.text.assign(data.begin(), data.end());
        return request;
    }
};

struct EchoResponse {
    std::string text;

    std::vector<std::uint8_t> serialize() const {
        return std::vector<std::uint8_t>(text.begin(), text.end());
    }

    static EchoResponse deserialize(const std::vector<std::uint8_t>& data) {
        EchoResponse response;
        response.text.assign(data.begin(), data.end());
        return response;
    }
};

struct CountEvent {
    std::int32_t value{0};

    std::vector<std::uint8_t> serialize() const {
        const auto v = static_cast<std::uint32_t>(value);
        return {
            static_cast<std::uint8_t>((v >> 24) & 0xFF),
            static_cast<std::uint8_t>((v >> 16) & 0xFF),
            static_cast<std::uint8_t>((v >> 8) & 0xFF),
            static_cast<std::uint8_t>(v & 0xFF)};
    }

    static CountEvent deserialize(const std::vector<std::uint8_t>& data) {
        CountEvent event;
        if (data.size() < 4) return event;
        const std::uint32_t v =
            (static_cast<std::uint32_t>(data[0]) << 24) |
            (static_cast<std::uint32_t>(data[1]) << 16) |
            (static_cast<std::uint32_t>(data[2]) << 8) |
            static_cast<std::uint32_t>(data[3]);
        event.value = static_cast<std::int32_t>(v);
        return event;
    }
};

ServiceConfig make_config(ServiceId sid, const std::string& role) {
    ServiceConfig config;
    config.service_id = sid;
    config.instance_id = 1;
    config.major_version = 1;
    config.minor_version = 0;
    config.role = role;
    return config;
}

bool parse_number(const char* text, unsigned long& value) {
    if (!text || !*text) return false;
    char* end = nullptr;
    value = std::strtoul(text, &end, 0);
    return end && *end == '\0';
}

int run_server(ServiceId sid, std::uint32_t run_ms) {
    auto backend = runtime::create_backend(runtime::BackendType::SHM);
    if (!backend || !backend->init(make_config(sid, "server")) ||
        !backend->start()) {
        std::cerr << "SERVER_FAIL backend\n";
        return 2;
    }

    ServiceSkeleton skeleton(backend, make_config(sid, "server"));
    if (!skeleton.init()) {
        std::cerr << "SERVER_FAIL skeleton init\n";
        return 2;
    }
    skeleton.register_method<EchoRequest, EchoResponse>(
        ECHO_METHOD, [](const EchoRequest& request) {
            return EchoResponse{"srv:" + request.text};
        });
    skeleton.register_event<CountEvent>(COUNT_EVENT);
    if (!skeleton.start()) {
        std::cerr << "SERVER_FAIL skeleton start\n";
        return 2;
    }
    skeleton.enable_heartbeat(1000);

    std::cout << "SERVER_READY sid=" << sid << std::endl;
    std::int32_t value = 0;
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(run_ms);
    while (!g_stop && std::chrono::steady_clock::now() < deadline) {
        skeleton.publish_event(COUNT_EVENT, CountEvent{value++});
        std::this_thread::sleep_for(200ms);
    }

    skeleton.stop();
    backend->stop();
    std::cout << "SERVER_DONE events=" << value << std::endl;
    return 0;
}

int run_client(ServiceId sid, int client_id, unsigned requests) {
    auto backend = runtime::create_backend(runtime::BackendType::SHM);
    if (!backend || !backend->init(make_config(sid, "client")) ||
        !backend->start()) {
        std::cerr << "CLIENT_FAIL backend\n";
        return 2;
    }

    ServiceProxy proxy(backend, make_config(sid, "client"));
    if (!proxy.init()) {
        std::cerr << "CLIENT_FAIL proxy init\n";
        return 2;
    }

    std::atomic<int> events{0};
    proxy.subscribe_event<CountEvent>(
        COUNT_EVENT, [&](const CountEvent&) { events.fetch_add(1); });
    if (!proxy.start()) {
        std::cerr << "CLIENT_FAIL proxy start\n";
        return 2;
    }

    const auto ready_deadline = std::chrono::steady_clock::now() + 5s;
    while (!proxy.is_available() &&
           std::chrono::steady_clock::now() < ready_deadline) {
        std::this_thread::sleep_for(10ms);
    }
    if (!proxy.is_available()) {
        std::cout << "CLIENT_RESULT id=" << client_id
                  << " ok=0 fail=" << requests << " events=0 available=0"
                  << std::endl;
        backend->stop();
        return 2;
    }

    unsigned ok = 0;
    unsigned fail = 0;
    for (unsigned i = 0; i < requests; ++i) {
        const std::string text = "c" + std::to_string(client_id) + "-" +
                                 std::to_string(i);
        try {
            const auto response =
                proxy.call_method_sync<EchoRequest, EchoResponse>(
                    ECHO_METHOD, EchoRequest{text}, 2000);
            if (response.text == "srv:" + text) {
                ++ok;
            } else {
                ++fail;
            }
        } catch (const std::exception&) {
            ++fail;
        }
    }

    const auto event_deadline = std::chrono::steady_clock::now() + 2s;
    while (events.load() == 0 &&
           std::chrono::steady_clock::now() < event_deadline) {
        std::this_thread::sleep_for(10ms);
    }

    std::cout << "CLIENT_RESULT id=" << client_id
              << " ok=" << ok << " fail=" << fail
              << " events=" << events.load() << " available=1" << std::endl;

    proxy.stop();
    backend->stop();
    return (ok == requests && fail == 0 && events.load() > 0) ? 0 : 1;
}

} // namespace

int main(int argc, char* argv[]) {
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    if (argc < 3) {
        std::cerr << "用法: shm_rpc_pair server <service_id> [run_ms]\n"
                  << "      shm_rpc_pair client <service_id> <client_id> [requests]\n";
        return 2;
    }

    const std::string mode = argv[1];
    unsigned long sid = 0;
    if (!parse_number(argv[2], sid) || sid == 0 || sid > 0xFFFF) {
        std::cerr << "无效的 service_id\n";
        return 2;
    }

    if (mode == "server") {
        unsigned long run_ms = 15000;
        if (argc >= 4 && !parse_number(argv[3], run_ms)) {
            std::cerr << "无效的 run_ms\n";
            return 2;
        }
        return run_server(static_cast<ServiceId>(sid),
                          static_cast<std::uint32_t>(run_ms));
    }

    if (mode == "client") {
        if (argc < 4) {
            std::cerr << "client 模式需要 client_id\n";
            return 2;
        }
        unsigned long client_id = 0;
        unsigned long requests = 100;
        if (!parse_number(argv[3], client_id) ||
            (argc >= 5 && !parse_number(argv[4], requests))) {
            std::cerr << "无效的 client_id 或 requests\n";
            return 2;
        }
        return run_client(static_cast<ServiceId>(sid),
                          static_cast<int>(client_id),
                          static_cast<unsigned>(requests));
    }

    std::cerr << "未知模式: " << mode << "\n";
    return 2;
}
