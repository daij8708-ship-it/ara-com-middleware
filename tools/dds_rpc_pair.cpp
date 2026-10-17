#include "communication/dds_backend.h"
#include "communication/service_proxy.h"
#include "communication/service_skeleton.h"
#include "demo_observability.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <functional>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#if !defined(ARA_COM_USE_DDS)
#error "dds_rpc_pair 必须启用真实 DDS 后端"
#endif

namespace {

using namespace ara::com;
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;
constexpr MethodId ECHO_METHOD = 0x0101;
constexpr EventId DATA_EVENT = 0x8101;
volatile std::sig_atomic_t stop_requested = 0;

void on_signal(int) { stop_requested = 1; }

struct Wire {
    std::string text;
    std::vector<std::uint8_t> serialize() const { return {text.begin(), text.end()}; }
    static Wire deserialize(const std::vector<std::uint8_t>& bytes) {
        return Wire{std::string(bytes.begin(), bytes.end())};
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

bool parse_number(const char* text, unsigned long long maximum, unsigned long long& value) {
    if (!text || *text < '0' || *text > '9') return false;
    errno = 0;
    char* end = nullptr;
    value = std::strtoull(text, &end, 0);
    return errno == 0 && end != text && *end == '\0' && value <= maximum;
}

bool wait_until(const std::function<bool()>& predicate, Clock::time_point deadline) {
    while (!stop_requested && Clock::now() < deadline) {
        if (predicate()) return true;
        std::this_thread::sleep_for(5ms);
    }
    return !stop_requested && predicate();
}

struct Session {
    std::shared_ptr<runtime::DdsBackend> backend;
    std::unique_ptr<ServiceProxy> proxy;
    std::unique_ptr<ServiceSkeleton> skeleton;
    bool stopped{false};

    bool start_backend(ServiceId sid, std::uint32_t domain, const std::string& role) {
        backend = std::make_shared<runtime::DdsBackend>(runtime::DdsBackendOptions{domain});
        return backend->init(make_config(sid, role)) && backend->start();
    }
    void stop() {
        if (stopped) return;
        stopped = true;
        // 所有退出路径都先停止后端，保证上层对象和回调状态仍然有效。
        if (backend) backend->stop();
        if (proxy) proxy->stop();
        if (skeleton) skeleton->stop();
    }
    ~Session() { stop(); }
};

std::string request_text(std::uint32_t client_id, unsigned seq) {
    return "client=" + std::to_string(client_id) + ";seq=" + std::to_string(seq);
}

std::string event_prefix(ServiceId sid, std::uint32_t domain) {
    return "event:sid=" + std::to_string(sid) + ";domain=" + std::to_string(domain) + ";seq=";
}

int run_server(ServiceId sid, std::uint32_t domain, std::uint32_t run_ms) {
    demo::Observability observability("dds-server", "dds", sid, 1);
    Session session;
    if (!session.start_backend(sid, domain, "server")) {
        std::cerr << "SERVER_FAIL backend\n";
        return 2;
    }
    session.skeleton = std::make_unique<ServiceSkeleton>(session.backend, make_config(sid, "server"));
    auto& skeleton = *session.skeleton;
    skeleton.set_e2e_enabled(false);
    skeleton.set_fragmentation_enabled(false);
    if (!skeleton.init()) {
        std::cerr << "SERVER_FAIL skeleton_init\n";
        return 2;
    }
    auto calls = std::make_shared<std::atomic<unsigned>>(0);
    skeleton.register_method<Wire, Wire>(ECHO_METHOD, [calls](const Wire& request) {
        ++*calls;
        return Wire{"reply:" + request.text};
    });
    skeleton.register_event<Wire>(DATA_EVENT);
    if (!skeleton.start()) {
        std::cerr << "SERVER_FAIL offer\n";
        return 2;
    }
    std::cout << "SERVER_READY sid=" << sid << " domain=" << domain << std::endl;
    ARA_LOG_INFO("DEMO", "SERVER_READY sid=" + std::to_string(sid) +
                 " domain=" + std::to_string(domain));
    const auto deadline = Clock::now() + std::chrono::milliseconds(run_ms);
    const auto prefix = event_prefix(sid, domain);
    unsigned events = 0;
    while (!stop_requested && Clock::now() < deadline) {
        if (!skeleton.publish_event(DATA_EVENT, Wire{prefix + std::to_string(events)})) {
            std::cerr << "SERVER_FAIL event_publish\n";
            return 1;
        }
        ++events;
        // 此处控制事件发布频率，客户端就绪另由发现和实收事件确认。
        std::this_thread::sleep_for(50ms);
    }
    session.stop();
    std::cout << "SERVER_DONE requests=" << calls->load() << " events=" << events << std::endl;
    ARA_LOG_INFO("DEMO", "SERVER_DONE requests=" + std::to_string(calls->load()) +
                 " events=" + std::to_string(events));
    return 0;
}

struct ClientState {
    ClientState(std::uint32_t id, unsigned requests, std::string prefix)
        : client_id(id), seen(requests, 0), prefix(std::move(prefix)) {}
    const std::uint32_t client_id;
    std::mutex mutex;
    std::condition_variable cv;
    std::vector<unsigned> seen;
    const std::string prefix;
    unsigned ok{0};
    unsigned callbacks{0};
    unsigned duplicates{0};
    unsigned events{0};
    unsigned bad_events{0};

    void complete(unsigned seq, bool asynchronous, ComRet status,
                   const std::vector<std::uint8_t>& bytes) {
        std::lock_guard<std::mutex> lock(mutex);
        if (asynchronous) ++callbacks;
        if (++seen.at(seq) != 1) {
            ++duplicates;
        } else if (status == ComRet::OK &&
                   Wire::deserialize(bytes).text == "reply:" + request_text(client_id, seq)) {
            ++ok;
        }
        cv.notify_all();
    }
    void event(const Wire& value) {
        bool valid = value.text.compare(0, prefix.size(), prefix) == 0 && value.text.size() > prefix.size();
        for (std::size_t i = prefix.size(); valid && i < value.text.size(); ++i)
            valid = value.text[i] >= '0' && value.text[i] <= '9';
        std::lock_guard<std::mutex> lock(mutex);
        if (valid) ++events;
        else ++bad_events;
        cv.notify_all();
    }
    bool has_event() {
        std::lock_guard<std::mutex> lock(mutex);
        return events > 0;
    }
    bool wait_batch(unsigned end, Clock::time_point deadline) {
        std::unique_lock<std::mutex> lock(mutex);
        const auto complete = [this, end] {
            for (unsigned i = 0; i < end; ++i) if (seen[i] == 0) return false;
            return true;
        };
        while (!complete() && !stop_requested) {
            if (cv.wait_until(lock, deadline) == std::cv_status::timeout) break;
        }
        return !stop_requested && complete() && ok == end && duplicates == 0 && bad_events == 0;
    }
};

int run_client(ServiceId sid, std::uint32_t domain, std::uint32_t client_id, unsigned requests) {
    demo::Observability observability("dds-client-" + std::to_string(client_id),
                                      "dds", sid, 1);
    Session session;
    auto state = std::make_shared<ClientState>(client_id, requests, event_prefix(sid, domain));
    bool ready = false;
    bool connected_at_end = false;
    unsigned sync_submitted = 0;
    unsigned async_submitted = 0;
    bool protocol_ok = true;
    try {
        if (!session.start_backend(sid, domain, "client")) throw std::runtime_error("backend");
        session.proxy = std::make_unique<ServiceProxy>(session.backend, make_config(sid, "client"));
        auto& proxy = *session.proxy;
        proxy.configure_connection_safety(false, 3000, false, 10, 100, 2);
        proxy.set_e2e_enabled(false);
        proxy.set_fragmentation_enabled(false);
        if (!proxy.init()) throw std::runtime_error("proxy_init");
        proxy.subscribe_event<Wire>(DATA_EVENT, [state](const Wire& value) { state->event(value); });
        if (!proxy.start()) throw std::runtime_error("proxy_start");
        ready = wait_until([&proxy, state] { return proxy.is_available() && state->has_event(); }, Clock::now() + 12s);
        if (!ready) throw std::runtime_error("discovery_or_event_timeout");
        std::cout << "CLIENT_CONNECTED id=" << client_id << " sid=" << sid << " domain=" << domain << std::endl;
        ARA_LOG_INFO("DEMO", "CLIENT_CONNECTED id=" + std::to_string(client_id) +
                     " sid=" + std::to_string(sid));
        const auto deadline = Clock::now() + 40s;
        constexpr unsigned batch_size = 16;
        for (unsigned seq = 0; seq < requests; ++seq) {
            if (stop_requested || Clock::now() >= deadline) throw std::runtime_error("request_deadline");
            const Wire request{request_text(client_id, seq)};
            if (seq % 2 == 0) {
                ++async_submitted;
                // Proxy 的异步接口不返回提交状态，批次屏障会把缺少回调判为失败。
                proxy.call_method_async<Wire, Wire>(ECHO_METHOD, request,
                    [state, seq](ComRet status, const std::vector<std::uint8_t>& bytes) {
                        state->complete(seq, true, status, bytes);
                    }, 3000);
            } else {
                ++sync_submitted;
                const auto response = proxy.call_method_sync<Wire, Wire>(ECHO_METHOD, request, 3000);
                state->complete(seq, false, ComRet::OK, response.serialize());
            }
            if ((seq + 1) % batch_size == 0 || seq + 1 == requests) {
                const auto batch_deadline = std::min(deadline, Clock::now() + 5s);
                if (!state->wait_batch(seq + 1, batch_deadline)) throw std::runtime_error("reply_count_or_payload");
            }
        }
        connected_at_end = proxy.is_available();
    } catch (const std::exception& error) {
        protocol_ok = false;
        std::cerr << "CLIENT_FAIL id=" << client_id << " reason=" << error.what() << '\n';
        ARA_LOG_ERROR("DEMO", "CLIENT_FAIL id=" + std::to_string(client_id) +
                      " reason=" + error.what());
    }
    // 停止后计数不再变化，迟到或重复回调也纳入最终核对。
    session.stop();
    std::lock_guard<std::mutex> lock(state->mutex);
    const unsigned fail = requests - state->ok;
    const unsigned expected_async = (requests + 1) / 2;
    const bool success = protocol_ok && ready && connected_at_end && !stop_requested &&
        state->ok == requests && state->callbacks == expected_async &&
        async_submitted == expected_async && sync_submitted == requests / 2 &&
        state->duplicates == 0 && state->events > 0 && state->bad_events == 0;
    std::cout << "CLIENT_RESULT id=" << client_id << " requests=" << requests
              << " sync=" << sync_submitted << " async=" << async_submitted
              << " ok=" << state->ok << " fail=" << fail
              << " callbacks=" << state->callbacks << " duplicates=" << state->duplicates
              << " events=" << state->events << " bad_events=" << state->bad_events
              << " available=" << (connected_at_end ? 1 : 0) << std::endl;
    ARA_LOG_INFO("DEMO", "CLIENT_RESULT id=" + std::to_string(client_id) +
                 " ok=" + std::to_string(state->ok) +
                 " fail=" + std::to_string(fail) +
                 " events=" + std::to_string(state->events) +
                 " duplicates=" + std::to_string(state->duplicates) +
                 " bad_events=" + std::to_string(state->bad_events) +
                 " success=" + (success ? "1" : "0"));
    return success ? 0 : 1;
}

int run_recovery(ServiceId sid, std::uint32_t domain, std::uint32_t client_id) {
    demo::Observability observability("dds-recovery-" + std::to_string(client_id),
                                      "dds", sid, 1);
    Session session;
    auto state = std::make_shared<ClientState>(client_id, 0, event_prefix(sid, domain));
    auto losses = std::make_shared<std::atomic<unsigned>>(0);
    try {
        if (!session.start_backend(sid, domain, "client")) throw std::runtime_error("backend");
        session.proxy = std::make_unique<ServiceProxy>(session.backend, make_config(sid, "client"));
        auto& proxy = *session.proxy;
        proxy.configure_connection_safety(false, 3000, false, 10, 100, 2);
        proxy.set_e2e_enabled(false);
        proxy.set_fragmentation_enabled(false);
        if (!proxy.init()) throw std::runtime_error("proxy_init");
        proxy.on_availability_change([losses](ServiceId, InstanceId, ServiceAvailability status) {
            if (status == ServiceAvailability::UNAVAILABLE) ++*losses;
        });
        proxy.subscribe_event<Wire>(DATA_EVENT, [state](const Wire& value) { state->event(value); });
        if (!proxy.start()) throw std::runtime_error("proxy_start");
        if (!wait_until([&proxy, state] { return proxy.is_available() && state->has_event(); }, Clock::now() + 12s))
            throw std::runtime_error("initial_discovery");
        const auto before = proxy.call_method_sync<Wire, Wire>(ECHO_METHOD, Wire{"before-restart"}, 3000);
        if (before.text != "reply:before-restart") throw std::runtime_error("initial_reply");
        std::cout << "CLIENT_RECOVERY_READY id=" << client_id << std::endl;
        if (!wait_until([losses] { return losses->load() > 0; }, Clock::now() + 12s))
            throw std::runtime_error("missing_unavailable");
        unsigned previous_events;
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            previous_events = state->events;
        }
        std::cout << "CLIENT_RECOVERY_LOST id=" << client_id << std::endl;
        if (!wait_until([&proxy, state, previous_events] {
                std::lock_guard<std::mutex> lock(state->mutex);
                return proxy.is_available() && state->events > previous_events;
            }, Clock::now() + 15s)) throw std::runtime_error("recovery_or_event_timeout");
        const auto after = proxy.call_method_sync<Wire, Wire>(ECHO_METHOD, Wire{"after-restart"}, 3000);
        if (after.text != "reply:after-restart") throw std::runtime_error("recovered_reply");
        session.stop();
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            if (state->bad_events != 0) throw std::runtime_error("bad_recovered_event");
        }
        std::cout << "RECOVERY_RESULT id=" << client_id << " unavailable=1 recovered=1 rpc=1 events=1" << std::endl;
        ARA_LOG_INFO("DEMO", "RECOVERY_RESULT id=" + std::to_string(client_id) +
                     " unavailable=1 recovered=1 rpc=1 events=1");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "RECOVERY_FAIL " << error.what() << std::endl;
        ARA_LOG_ERROR("DEMO", "RECOVERY_FAIL id=" + std::to_string(client_id) +
                      " reason=" + error.what());
        return 1;
    }
}

void usage() {
    std::cerr << "用法: dds_rpc_pair server <sid> <domain> [run_ms]\n"
              << "      dds_rpc_pair client <sid> <domain> <client_id> [requests]\n"
              << "      dds_rpc_pair recover <sid> <domain> <client_id>\n";
}

}

int main(int argc, char* argv[]) {
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
    if (argc < 4) {
        usage();
        return 2;
    }
    unsigned long long sid = 0;
    unsigned long long domain = 0;
    if (!parse_number(argv[2], 65535, sid) || sid == 0 || !parse_number(argv[3], 232, domain)) {
        std::cerr << "无效的服务编号或 DDS 域，域范围为 0..232\n";
        return 2;
    }
    const std::string mode = argv[1];
    try {
        if (mode == "server" && (argc == 4 || argc == 5)) {
            unsigned long long run_ms = 90000;
            if (argc == 5 && (!parse_number(argv[4], std::numeric_limits<std::uint32_t>::max(), run_ms) || run_ms == 0)) {
                usage();
                return 2;
            }
            return run_server(static_cast<ServiceId>(sid), static_cast<std::uint32_t>(domain),
                              static_cast<std::uint32_t>(run_ms));
        }
        if (mode == "recover" && argc == 5) {
            unsigned long long client_id = 0;
            if (!parse_number(argv[4], std::numeric_limits<std::uint32_t>::max(), client_id)) return 2;
            return run_recovery(static_cast<ServiceId>(sid), static_cast<std::uint32_t>(domain),
                                static_cast<std::uint32_t>(client_id));
        }
        if (mode == "client" && (argc == 5 || argc == 6)) {
            unsigned long long client_id = 0;
            unsigned long long requests = 200;
            if (!parse_number(argv[4], std::numeric_limits<std::uint32_t>::max(), client_id) ||
                (argc == 6 && !parse_number(argv[5], 100000, requests)) || requests == 0) {
                usage();
                return 2;
            }
            return run_client(static_cast<ServiceId>(sid), static_cast<std::uint32_t>(domain),
                              static_cast<std::uint32_t>(client_id), static_cast<unsigned>(requests));
        }
    } catch (const std::exception& error) {
        std::cerr << "DDS_FAIL " << error.what() << '\n';
        return 2;
    }
    usage();
    return 2;
}
