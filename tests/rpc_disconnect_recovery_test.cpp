#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdint>
#include <fstream>
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
constexpr MethodId MID = 0x0103;
volatile std::sig_atomic_t stopping = 0;
void sig(int) { stopping = 1; }
void put32(Bytes& b, std::uint32_t x) {
    for (int s = 24; s >= 0; s -= 8) b.push_back(x >> s);
}
std::uint32_t get32(const Bytes& b, unsigned o) {
    std::uint32_t x = 0;
    for (unsigned i = o; i < o + 4; i++) x = (x << 8) | b[i];
    return x;
}
Bytes payload(std::uint32_t id, std::uint32_t delay) {
    Bytes b;
    put32(b, id);
    put32(b, delay);
    for (unsigned i = 0; i < 24; i++) b.push_back(id + i);
    return b;
}
struct R {
    std::mutex m;
    unsigned callbacks = 0;
    ComRet status = ComRet::INTERNAL_ERR;
    bool bad = false, accepted = false;
};
int main(int argc, char** argv) {
#ifndef ARA_COM_USE_VSOMEIP
    std::cerr << "vsomeip required\n";
    return 2;
#endif
    if (argc < 2) {
        std::cerr << "Usage: rpc_disconnect_recovery_test router | server | client CONTROL_DIR\n";
        return 2;
    }
    const std::string mode = argv[1];
    bool server = mode == "server", router = mode == "router";
    if (!server && !router && (argc != 3 || mode != "client")) return 2;
    auto& log = logger::AsyncLogger::instance();
    if (!log.init(LogLevel::DEBUG,
                  router ? "rpc-disconnect-router.log"
                         : (server ? "rpc-disconnect-server.log" : "rpc-disconnect-client.log")))
        return 2;
    ServiceConfig cfg;
    cfg.service_id = SID;
    cfg.instance_id = IID;
    cfg.major_version = 1;
    cfg.minor_version = 0;
    cfg.role = mode;
    auto backend = runtime::create_backend(runtime::BackendType::VSOMEIP);
    auto finish = [&](int n) {
        backend->stop();
        log.shutdown();
        return n;
    };
    if (!backend->init(cfg)) return finish(2);
    if (router) {
        if (!backend->start()) return finish(2);
        std::signal(SIGINT, sig);
        std::signal(SIGTERM, sig);
        std::cout << "ROUTER_READY" << std::endl;
        while (!stopping) std::this_thread::sleep_for(50ms);
        return finish(0);
    }
    if (server) {
        if (!backend->register_method_handler(SID, IID, MID, [](const Bytes& req) {
                if (req.size() != 32) return Bytes{};
                auto d = get32(req, 4);
                if (d > 3000) return Bytes{};
                std::this_thread::sleep_for(std::chrono::milliseconds(d));
                return req;
            }))
            return finish(2);
        if (!backend->start() || !backend->offer_service(SID, IID, 1, 0)) return finish(2);
        std::signal(SIGINT, sig);
        std::signal(SIGTERM, sig);
        std::cout << "SERVER_READY" << std::endl;
        while (!stopping) std::this_thread::sleep_for(50ms);
        backend->stop_offer_service(SID, IID);
        backend->stop_offer_service(SID, IID);
        return finish(0);
    }
    std::string dir = argv[2];
    std::atomic<ServiceAvailability> avail{ServiceAvailability::UNKNOWN};
    std::atomic<unsigned> unavailable_events{0}, available_events{0};
    backend->register_availability_callback([&](ServiceId s, InstanceId i, ServiceAvailability a) {
        if (s == SID && i == IID) {
            avail.store(a);
            if (a == ServiceAvailability::AVAILABLE)
                available_events++;
            else if (a == ServiceAvailability::UNAVAILABLE)
                unavailable_events++;
            std::cout << "AVAIL state=" << static_cast<int>(a)
                      << " available_events=" << available_events
                      << " unavailable_events=" << unavailable_events << std::endl;
        }
    });
    if (!backend->start() || !backend->request_service(SID, IID, 1, 0)) return finish(2);
    auto waitstate = [&](ServiceAvailability wanted, std::chrono::seconds limit) {
        auto end = std::chrono::steady_clock::now() + limit;
        while (avail.load() != wanted && std::chrono::steady_clock::now() < end)
            std::this_thread::sleep_for(10ms);
        return avail.load() == wanted;
    };
    if (!waitstate(ServiceAvailability::AVAILABLE, 10s)) {
        std::cerr << "SETUP_FAIL initial availability\n";
        return finish(2);
    }
    Bytes rsp;
    auto warm = payload(0, 0);
    if (backend->call_method_sync(SID, IID, MID, warm, rsp, 2000) != ComRet::OK || rsp != warm) {
        std::cerr << "SETUP_FAIL warmup\n";
        return finish(2);
    }
    // 先留一批请求在服务端处理，再通知脚本停止服务。
    constexpr unsigned N = 16;
    std::vector<std::shared_ptr<R>> rec;
    rec.reserve(N);
    for (unsigned id = 1; id <= N; id++) {
        auto r = std::make_shared<R>();
        rec.push_back(r);
        auto req = payload(id, 1500);
        r->accepted = backend->call_method_async(
            SID, IID, MID, req,
            [r, req, id](ComRet st, const Bytes& out) {
                std::lock_guard<std::mutex> g(r->m);
                r->callbacks++;
                r->status = st;
                if (st == ComRet::OK && out != req) r->bad = true;
                std::cout << "CALLBACK id=" << id << " status=" << static_cast<int>(st)
                          << " count=" << r->callbacks << " bad=" << r->bad << std::endl;
            },
            2500);
    }
    {
        std::ofstream f(dir + "/ready-to-kill");
        f << "submitted=16\n";
    }
    std::cout << "READY_TO_KILL submitted=16" << std::endl;
    if (!waitstate(ServiceAvailability::UNAVAILABLE, 10s)) {
        std::cerr << "FAIL no unavailable transition\n";
        return finish(1);
    }
    auto complete_end = std::chrono::steady_clock::now() + 5s;
    while (std::chrono::steady_clock::now() < complete_end) {
        unsigned n = 0;
        for (auto& r : rec) {
            std::lock_guard<std::mutex> g(r->m);
            n += r->callbacks > 0;
        }
        if (n == N) break;
        std::this_thread::sleep_for(10ms);
    }
    {
        std::ofstream f(dir + "/ready-to-restart");
        f << "unavailable=1\n";
    }
    std::cout << "READY_TO_RESTART" << std::endl;
    if (!waitstate(ServiceAvailability::AVAILABLE, 15s)) {
        std::cerr << "FAIL no restored availability\n";
        return finish(1);
    }
    Bytes after;
    auto probe = payload(1000, 0);
    auto post = backend->call_method_sync(SID, IID, MID, probe, after, 3000);
    bool recovered = post == ComRet::OK && after == probe;
    std::this_thread::sleep_for(2s);
    backend->stop();
    unsigned failures = 0, once = 0, successes = 0, timeouts = 0, other = 0;
    for (unsigned i = 0; i < N; i++) {
        auto& r = rec[i];
        std::lock_guard<std::mutex> g(r->m);
        bool pass = r->accepted && r->callbacks == 1 && !r->bad &&
                    (r->status == ComRet::OK || r->status == ComRet::TIMEOUT ||
                     r->status == ComRet::NOT_AVAILABLE);
        if (pass)
            once++;
        else
            failures++;
        if (r->status == ComRet::OK)
            successes++;
        else if (r->status == ComRet::TIMEOUT)
            timeouts++;
        else if (r->status != ComRet::NOT_AVAILABLE)
            other++;
        std::cout << "CASE id=" << i + 1 << " accepted=" << r->accepted
                  << " callbacks=" << r->callbacks << " status=" << static_cast<int>(r->status)
                  << " bad=" << r->bad << " result=" << (pass ? "PASS" : "FAIL") << '\n';
    }
    bool coverage = unavailable_events > 0 && available_events >= 2 && recovered;
    std::cout << "SUMMARY submitted=16 once=" << once << " failures=" << failures
              << " successes=" << successes << " timeouts=" << timeouts << " other=" << other
              << " unavailable_events=" << unavailable_events
              << " available_events=" << available_events << " recovered=" << recovered
              << " coverage=" << coverage << '\n';
    log.shutdown();
    return failures == 0 && coverage ? 0 : 1;
}
