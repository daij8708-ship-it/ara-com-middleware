#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdint>
#include <iostream>
#include <memory>
#include <mutex>
#include <numeric>
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
constexpr MethodId MID = 0x0104;
volatile std::sig_atomic_t stop_flag = 0;
void sig(int) { stop_flag = 1; }
void put32(Bytes& b, std::uint32_t x) {
    for (int s = 24; s >= 0; s -= 8) b.push_back(x >> s);
}
Bytes payload(std::uint32_t id) {
    Bytes b;
    put32(b, id);
    for (unsigned i = 4; i < 64; i++) b.push_back(static_cast<std::uint8_t>(id + i));
    return b;
}
struct Stats {
    std::atomic<std::uint64_t> submitted{0}, completed{0}, ok{0}, timeouts{0}, errors{0}, bad{0},
        duplicates{0};
    std::atomic<unsigned> inflight{0}, peak{0};
    std::mutex m;
    std::vector<double> us;
};
int main(int argc, char** argv) {
#ifndef ARA_COM_USE_VSOMEIP
    return 2;
#endif
    if (argc < 2) return 2;
    std::string mode = argv[1];
    bool server = mode == "server", router = mode == "router", client = mode == "client";
    if (!server && !router && !client) return 2;
    unsigned window = 1, warm = 5, duration = 20;
    if (client) {
        if (argc != 5) return 2;
        try {
            window = std::stoul(argv[2]);
            warm = std::stoul(argv[3]);
            duration = std::stoul(argv[4]);
        } catch (...) {
            return 2;
        }
        if (window < 1 || window > 256 || warm < 1 || duration < 1) return 2;
    }
    auto& lg = logger::AsyncLogger::instance();
    if (!lg.init(LogLevel::WARN, "vsomeip-capacity-" + mode + ".log")) return 2;
    ServiceConfig cfg;
    cfg.service_id = SID;
    cfg.instance_id = IID;
    cfg.major_version = 1;
    cfg.minor_version = 0;
    cfg.role = mode;
    auto backend = runtime::create_backend(runtime::BackendType::VSOMEIP);
    auto finish = [&](int n) {
        backend->stop();
        lg.shutdown();
        return n;
    };
    if (!backend->init(cfg)) return finish(2);
    if (router) {
        if (!backend->start()) return finish(2);
        std::signal(SIGTERM, sig);
        std::signal(SIGINT, sig);
        std::cout << "ROUTER_READY" << std::endl;
        while (!stop_flag) std::this_thread::sleep_for(50ms);
        return finish(0);
    }
    if (server) {
        if (!backend->register_method_handler(SID, IID, MID, [](const Bytes& r) { return r; }))
            return finish(2);
        if (!backend->start() || !backend->offer_service(SID, IID, 1, 0)) return finish(2);
        std::signal(SIGTERM, sig);
        std::signal(SIGINT, sig);
        std::cout << "SERVER_READY" << std::endl;
        while (!stop_flag) std::this_thread::sleep_for(50ms);
        backend->stop_offer_service(SID, IID);
        backend->stop_offer_service(SID, IID);
        return finish(0);
    }
    std::atomic<bool> available{false};
    backend->register_availability_callback([&](ServiceId s, InstanceId i, ServiceAvailability a) {
        if (s == SID && i == IID) available = a == ServiceAvailability::AVAILABLE;
    });
    if (!backend->start() || !backend->request_service(SID, IID, 1, 0)) return finish(2);
    auto ae = std::chrono::steady_clock::now() + 10s;
    while (!available && std::chrono::steady_clock::now() < ae) std::this_thread::sleep_for(10ms);
    if (!available) {
        std::cerr << "SETUP_FAIL availability\n";
        return finish(2);
    }
    Bytes out;
    auto w = payload(0);
    if (backend->call_method_sync(SID, IID, MID, w, out, 2000) != ComRet::OK || out != w) {
        std::cerr << "SETUP_FAIL echo\n";
        return finish(2);
    }
    // 预热和正式测量走同一条发送路径，但预热数据不计入结果。
    auto run_phase = [&](unsigned seconds, bool measured, Stats& st, std::uint32_t& next) {
        auto end = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
        while (std::chrono::steady_clock::now() < end) {
            if (st.inflight.load() >= window) {
                // 窗口满了先让出 CPU，等回调释放名额。
                std::this_thread::yield();
                continue;
            }
            auto id = next++;
            auto req = payload(id);
            auto begin = std::chrono::steady_clock::now();
            auto count = std::make_shared<std::atomic<unsigned>>(0);
            st.inflight++;
            unsigned p = st.inflight.load(), old = st.peak.load();
            while (p > old && !st.peak.compare_exchange_weak(old, p)) {
            }
            if (measured) st.submitted++;
            bool accepted = backend->call_method_async(
                SID, IID, MID, req,
                [&, req, begin, count, measured](ComRet status, const Bytes& rsp) {
                    unsigned n = ++*count;
                    if (n > 1) {
                        // 重复回调只记错误，不再减少在途数。
                        if (measured) st.duplicates++;
                        return;
                    }
                    auto elapsed = std::chrono::duration<double, std::micro>(
                                       std::chrono::steady_clock::now() - begin)
                                       .count();
                    if (measured) {
                        st.completed++;
                        if (status == ComRet::OK) {
                            st.ok++;
                            if (rsp != req) st.bad++;
                        } else if (status == ComRet::TIMEOUT)
                            st.timeouts++;
                        else
                            st.errors++;
                        std::lock_guard<std::mutex> g(st.m);
                        st.us.push_back(elapsed);
                    }
                    st.inflight--;
                },
                2000);
            if (!accepted) {
                st.inflight--;
                if (measured) st.errors++;
            }
        }
        // 到时间后不再发新请求，再等已经发出的请求结束。
        auto drain = std::chrono::steady_clock::now() + 5s;
        while (st.inflight && std::chrono::steady_clock::now() < drain)
            std::this_thread::sleep_for(1ms);
        return st.inflight.load() == 0;
    };
    std::uint32_t next = 1;
    Stats warmstats;
    if (!run_phase(warm, false, warmstats, next)) {
        std::cerr << "WARMUP_DRAIN_FAIL\n";
        return finish(1);
    }
    Stats st;
    st.us.reserve(1000000);
    auto mb = std::chrono::steady_clock::now();
    bool drained = run_phase(duration, true, st, next);
    auto me = std::chrono::steady_clock::now();
    backend->stop();
    std::sort(st.us.begin(), st.us.end());
    auto pct = [&](double p) {
        return st.us.empty() ? 0.0 : st.us[static_cast<std::size_t>((st.us.size() - 1) * p)];
    };
    double secs = std::chrono::duration<double>(me - mb).count();
    double mean =
        st.us.empty() ? 0 : std::accumulate(st.us.begin(), st.us.end(), 0.0) / st.us.size();
    // 和离线检查器用同一套标准：请求全部成功，没有错误或超时，
    // 而且实际在途数确实达到设定窗口。
    const bool pass = drained && st.submitted > 0 && st.submitted == st.completed &&
                      st.completed == st.ok && st.errors == 0 && st.timeouts == 0 && st.bad == 0 &&
                      st.duplicates == 0 && st.peak == window;
    const double timeout_rate = st.submitted ? 100.0 * st.timeouts / st.submitted : 100.0;
    std::cout.setf(std::ios::fixed);
    std::cout.precision(3);
    std::cout << "RESULT window=" << window << " warmup_s=" << warm << " duration_s=" << duration
              << " payload_bytes=64 submitted=" << st.submitted << " completed=" << st.completed
              << " ok=" << st.ok << " timeouts=" << st.timeouts << " errors=" << st.errors
              << " bad=" << st.bad << " duplicates=" << st.duplicates << " peak=" << st.peak
              << " throughput_rps=" << (st.ok / secs) << " mean_us=" << mean
              << " p50_us=" << pct(.50) << " p95_us=" << pct(.95) << " p99_us=" << pct(.99)
              << " max_us=" << (st.us.empty() ? 0 : st.us.back())
              << " timeout_rate_pct=" << timeout_rate << " drained=" << drained << " pass=" << pass
              << '\n';
    lg.shutdown();
    return pass ? 0 : 1;
}
