// 用真实 vsomeip 检查 RPC；这里不控制线程调度，不能保证每次都碰到竞态。
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdint>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "common/logger.h"
#include "communication/runtime.h"

using namespace ara::com;
using namespace std::chrono_literals;
using Bytes = std::vector<std::uint8_t>;
constexpr ServiceId SID = 0x1234;
constexpr InstanceId IID = 1;
constexpr MethodId MID = 0x0102;
volatile std::sig_atomic_t stopping = 0;
void on_signal(int) { stopping = 1; }

void append32(Bytes& out, std::uint32_t n) {
    for (int shift = 24; shift >= 0; shift -= 8)
        out.push_back(static_cast<std::uint8_t>(n >> shift));
}
std::uint32_t read32(const Bytes& b, std::size_t offset) {
    std::uint32_t n = 0;
    for (std::size_t i = offset; i < offset + 4; ++i) n = (n << 8) | b[i];
    return n;
}
Bytes request_bytes(std::uint32_t id, std::uint32_t delay) {
    Bytes b;
    append32(b, id);
    append32(b, delay);
    for (unsigned i = 0; i < 24; ++i) b.push_back(static_cast<std::uint8_t>(id + i));
    return b;
}
struct Record {
    std::mutex mutex;
    std::condition_variable cv;
    unsigned callbacks = 0;
    ComRet status = ComRet::INTERNAL_ERR;
    bool bad_payload = false;
    bool accepted = false;
    bool timely_completion = false;
    int expectation = -1;  // -1 表示允许成功或超时，其他值要求返回对应的 ComRet
    std::string name;
    std::uint32_t id = 0;
};

int main(int argc, char** argv) {
#ifndef ARA_COM_USE_VSOMEIP
    std::cerr << "Rebuild with ARA_COM_USE_VSOMEIP=ON; NullBackend is forbidden.\n";
    return 2;
#endif
    if (argc < 2 || (std::string(argv[1]) != "server" && std::string(argv[1]) != "client")) {
        std::cerr << "Usage: rpc_completion_test server | client [boundary_rounds=100]\n";
        return 2;
    }
    const bool server = std::string(argv[1]) == "server";
    unsigned rounds = 100;
    try {
        if (!server && argc > 2) rounds = static_cast<unsigned>(std::stoul(argv[2]));
        if (rounds == 0 || rounds > 10000) throw std::out_of_range("rounds");
    } catch (...) {
        std::cerr << "boundary_rounds must be 1..10000\n";
        return 2;
    }
    auto& log = logger::AsyncLogger::instance();
    if (!log.init(LogLevel::DEBUG, server ? "rpc-server.log" : "rpc-client.log")) return 2;
    ServiceConfig cfg;
    cfg.service_id = SID;
    cfg.instance_id = IID;
    cfg.major_version = 1;
    cfg.minor_version = 0;
    cfg.role = server ? "server" : "client";
    auto backend = runtime::create_backend(runtime::BackendType::VSOMEIP);
    auto finish = [&](int code) {
        backend->stop();
        log.shutdown();
        return code;
    };
    if (!backend->init(cfg)) return finish(2);

    if (server) {
        if (!backend->register_method_handler(SID, IID, MID, [](const Bytes& req) {
                if (req.size() != 32) return Bytes{};
                const auto delay = read32(req, 4);
                if (delay > 1000) return Bytes{};
                std::this_thread::sleep_for(std::chrono::milliseconds(delay));
                return req;
            }))
            return finish(2);
        if (!backend->start() || !backend->offer_service(SID, IID, 1, 0)) return finish(2);
        std::signal(SIGINT, on_signal);
        std::signal(SIGTERM, on_signal);
        std::cout << "SERVER_READY method=0x0102\n";
        while (!stopping) std::this_thread::sleep_for(100ms);
        backend->stop_offer_service(SID, IID);
        return finish(0);
    }

    auto available = std::make_shared<std::atomic<bool>>(false);
    backend->register_availability_callback(
        [available](ServiceId sid, InstanceId iid, ServiceAvailability state) {
            if (sid == SID && iid == IID) available->store(state == ServiceAvailability::AVAILABLE);
        });
    if (!backend->start() || !backend->request_service(SID, IID, 1, 0)) return finish(2);
    const auto discovery_deadline = std::chrono::steady_clock::now() + 10s;
    while (!available->load() && std::chrono::steady_clock::now() < discovery_deadline)
        std::this_thread::sleep_for(10ms);
    if (!available->load()) {
        std::cerr << "SETUP_FAIL: service discovery\n";
        return finish(2);
    }
    Bytes warm_response;
    const auto warm_request = request_bytes(0, 0);
    if (backend->call_method_sync(SID, IID, MID, warm_request, warm_response, 2000) != ComRet::OK ||
        warm_response != warm_request) {
        std::cerr << "SETUP_FAIL: warm-up RPC; check server and transport configuration\n";
        return finish(2);
    }

    std::vector<std::shared_ptr<Record>> records;
    std::uint32_t next_id = 1;
    auto run = [&](const std::string& name, unsigned delay, unsigned timeout, int expected) {
        auto r = std::make_shared<Record>();
        r->id = next_id++;
        r->name = name;
        r->expectation = expected;
        records.push_back(r);  // 记录保留到后端停止，方便统计迟到和重复回调。
        const auto bytes = request_bytes(r->id, delay);
        r->accepted = backend->call_method_async(
            SID, IID, MID, bytes,
            [r, bytes](ComRet status, const Bytes& response) {
                std::lock_guard<std::mutex> guard(r->mutex);
                ++r->callbacks;
#ifdef ARA_COM_RPC_COMPLETION_TRACE
                const auto observed_us = std::chrono::duration_cast<std::chrono::microseconds>(
                                             std::chrono::steady_clock::now().time_since_epoch())
                                             .count();
                ARA_LOG_INFO("RPC_TRACE",
                             "event=CALLBACK request_id=" + std::to_string(r->id) +
                                 " status=" + std::to_string(static_cast<int>(status)) +
                                 " count=" + std::to_string(r->callbacks) +
                                 " observed_us=" + std::to_string(observed_us));
#endif
                r->status = status;
                if (status == ComRet::OK && response != bytes) r->bad_payload = true;
                r->cv.notify_all();
            },
            timeout);
        {
            std::unique_lock<std::mutex> guard(r->mutex);
            if (r->accepted)
                r->timely_completion = r->cv.wait_for(guard, 2s, [&] { return r->callbacks > 0; });
        }
        // 多等一会儿观察迟到响应，是否收到还要看响应日志。
        std::this_thread::sleep_for(std::chrono::milliseconds(delay + 150));
    };
    for (unsigned i = 0; i < 10; ++i) run("early", 0, 500, static_cast<int>(ComRet::OK));
    for (unsigned i = 0; i < 10; ++i) run("late", 200, 50, static_cast<int>(ComRet::TIMEOUT));
    for (unsigned i = 0; i < rounds; ++i) run("boundary", 98 + i % 5, 100, -1);

    // 先保留后端和请求记录，让迟到响应有机会被处理。
    std::this_thread::sleep_for(2s);
    unsigned incomplete_before_stop = 0;
    for (const auto& r : records) {
        std::lock_guard<std::mutex> guard(r->mutex);
        if (r->accepted && r->callbacks == 0) ++incomplete_before_stop;
    }
    backend->stop();  // 先等处理线程退出，再汇总结果。
    unsigned failures = 0, boundary_ok = 0, boundary_timeout = 0;
    for (const auto& r : records) {
        std::lock_guard<std::mutex> guard(r->mutex);
        const bool allowed = r->expectation < 0
                                 ? (r->status == ComRet::OK || r->status == ComRet::TIMEOUT)
                                 : static_cast<int>(r->status) == r->expectation;
        const bool pass =
            r->accepted && r->timely_completion && r->callbacks == 1 && !r->bad_payload && allowed;
        if (!pass) ++failures;
        if (r->name == "boundary" && r->status == ComRet::OK) ++boundary_ok;
        if (r->name == "boundary" && r->status == ComRet::TIMEOUT) ++boundary_timeout;
        std::cout << "CASE name=" << r->name << " id=" << r->id << " accepted=" << r->accepted
                  << " callbacks=" << r->callbacks << " status=" << static_cast<int>(r->status)
                  << " bad_payload=" << r->bad_payload
                  << " timely_completion=" << r->timely_completion
                  << " result=" << (pass ? "PASS" : "FAIL") << '\n';
    }
    std::cout << "SUMMARY total=" << records.size() << " failures=" << failures
              << " incomplete_before_stop=" << incomplete_before_stop
              << " boundary_ok=" << boundary_ok << " boundary_timeout=" << boundary_timeout << '\n';
    log.shutdown();
    return failures == 0 && incomplete_before_stop == 0 ? 0 : 1;
}
