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
        std::cerr << "Usage: rpc_concurrency_test server | client [total=100] [window=4]\n";
        return 2;
    }
    const bool server = std::string(argv[1]) == "server";
    unsigned total = 100, window = 4;
    try {
        if (!server && argc > 2) total = static_cast<unsigned>(std::stoul(argv[2]));
        if (!server && argc > 3) window = static_cast<unsigned>(std::stoul(argv[3]));
        if (total < 10 || total > 2000 || window < 2 || window > 64 || window > total)
            throw std::out_of_range("total/window");
    } catch (...) {
        std::cerr << "total must be 10..2000; window must be 2..64 and <= total\n";
        return 2;
    }
    auto& log = logger::AsyncLogger::instance();
    if (!log.init(LogLevel::DEBUG,
                  server ? "rpc-concurrent-server.log" : "rpc-concurrent-client.log"))
        return 2;
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
        std::cout << "SERVER_READY method=0x0102" << std::endl;
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

    // 这里由一个线程连续发请求，同时保留多个在途 RPC。
    // 测的是请求匹配，不是多个调用线程的竞争或吞吐。
    auto active = std::make_shared<std::atomic<unsigned>>(0);
    std::vector<std::shared_ptr<Record>> records;
    records.reserve(total);
    unsigned peak_slots = 0;
    std::uint64_t sum_delay_ms = 0;
    bool watchdog = false;
    auto last_progress = std::chrono::steady_clock::now();
    unsigned previous_active = 0;
    for (unsigned id = 1; id <= total;) {
        const auto count = active->load();
        if (count != previous_active) {
            previous_active = count;
            last_progress = std::chrono::steady_clock::now();
        }
        if (count >= window) {
            if (std::chrono::steady_clock::now() - last_progress > 5s) {
                watchdog = true;
                break;
            }
            std::this_thread::sleep_for(1ms);
            continue;
        }
        unsigned delay, timeout;
        std::string name;
        if (id % 3 == 1) {
            name = "slow";
            delay = 200;
            timeout = 50;
        } else if (id % 3 == 2) {
            name = "fast";
            delay = 0;
            timeout = 500;
        } else {
            name = "boundary";
            delay = 98 + (id % 5);
            timeout = 100;
        }
        sum_delay_ms += delay;
        auto r = std::make_shared<Record>();
        r->id = id;
        r->name = name;
        records.push_back(r);
        const auto bytes = request_bytes(id, delay);
        const auto begin = std::chrono::steady_clock::now();
        const auto reserved = active->fetch_add(1) + 1;
        if (reserved > peak_slots) peak_slots = reserved;
        r->accepted = backend->call_method_async(
            SID, IID, MID, bytes,
            [r, bytes, active, begin](ComRet status, const Bytes& response) {
                std::lock_guard<std::mutex> guard(r->mutex);
                ++r->callbacks;
                const auto now = std::chrono::steady_clock::now();
                if (r->callbacks == 1) r->timely_completion = (now - begin <= 5s);
                r->status = status;
                if (status == ComRet::OK && response != bytes) r->bad_payload = true;
#ifdef ARA_COM_RPC_COMPLETION_TRACE
                const auto us =
                    std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch())
                        .count();
                ARA_LOG_INFO("RPC_TRACE",
                             "event=CALLBACK request_id=" + std::to_string(r->id) +
                                 " status=" + std::to_string(static_cast<int>(status)) +
                                 " count=" + std::to_string(r->callbacks) +
                                 " observed_us=" + std::to_string(us));
#endif
                // 只在第一次回调时释放名额，重复回调不能再次减少在途数。
                if (r->callbacks == 1) active->fetch_sub(1);
            },
            timeout);
        if (!r->accepted) active->fetch_sub(1);
        ++id;
        last_progress = std::chrono::steady_clock::now();
    }
    const auto callback_deadline = std::chrono::steady_clock::now() + 5s;
    while (active->load() != 0 && std::chrono::steady_clock::now() < callback_deadline)
        std::this_thread::sleep_for(1ms);
    if (active->load() != 0) watchdog = true;
    // 本地超时后，服务端可能还没处理完。
    // 按所有处理延迟之和再加余量等待，给迟到响应留时间。
    // 最后仍由日志检查器核对每个请求的响应记录。
    const auto drain_ms = sum_delay_ms + 2000;
    std::cout << "DRAIN wait_ms=" << drain_ms << " submitted=" << records.size() << std::endl;
    std::this_thread::sleep_for(std::chrono::milliseconds(drain_ms));
    unsigned incomplete_before_stop = 0;
    for (auto& r : records) {
        std::lock_guard<std::mutex> guard(r->mutex);
        if (r->accepted && r->callbacks == 0) ++incomplete_before_stop;
    }
    backend->stop();
    unsigned failures = 0, successes = 0, timeouts = 0;
    for (auto& r : records) {
        std::lock_guard<std::mutex> guard(r->mutex);
        const bool allowed = r->status == ComRet::OK || r->status == ComRet::TIMEOUT;
        const bool pass =
            r->accepted && r->timely_completion && r->callbacks == 1 && !r->bad_payload && allowed;
        if (!pass) ++failures;
        if (r->status == ComRet::OK) ++successes;
        if (r->status == ComRet::TIMEOUT) ++timeouts;
        std::cout << "CASE name=" << r->name << " id=" << r->id << " accepted=" << r->accepted
                  << " callbacks=" << r->callbacks << " status=" << static_cast<int>(r->status)
                  << " bad_payload=" << r->bad_payload
                  << " timely_completion=" << r->timely_completion
                  << " result=" << (pass ? "PASS" : "FAIL") << '\n';
    }
    // 不能全是超时；至少要有成功请求，才能检查响应匹配。
    const bool coverage = successes > 0 && timeouts > 0 && peak_slots > 1;
    std::cout << "SUMMARY total=" << records.size() << " requested=" << total
              << " window=" << window << " peak_slots=" << peak_slots << " failures=" << failures
              << " incomplete_before_stop=" << incomplete_before_stop << " successes=" << successes
              << " timeouts=" << timeouts << " watchdog=" << watchdog << " coverage=" << coverage
              << '\n';
    log.shutdown();
    if (failures || incomplete_before_stop || watchdog || records.size() != total) return 1;
    return coverage ? 0 : 3;
}
