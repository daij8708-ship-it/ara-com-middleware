#include "communication/dds_backend.h"
#include "communication/service_proxy.h"
#include "communication/service_skeleton.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#if defined(ARA_COM_USE_DDS)

namespace {

using namespace ara::com;
using namespace std::chrono_literals;
using Bytes = std::vector<std::uint8_t>;
using Clock = std::chrono::steady_clock;
constexpr std::uint32_t DOMAIN = 61;
constexpr MethodId ECHO = 0x0101;
constexpr MethodId BLOCKED = 0x0102;
constexpr MethodId THROWING = 0x0103;
constexpr MethodId LARGE_REPLY = 0x0104;
constexpr EventId EVENT = 0x8101;
constexpr EventId OTHER_EVENT = 0x8102;
std::atomic<ServiceId> next_service{0x7400};

struct Wire {
    std::string text;
    Bytes serialize() const { return Bytes(text.begin(), text.end()); }
    static Wire deserialize(const Bytes& bytes) {
        return Wire{std::string(bytes.begin(), bytes.end())};
    }
};

bool wait_until(const std::function<bool()>& predicate,
                std::chrono::milliseconds timeout = 5s) {
    const auto deadline = Clock::now() + timeout;
    do {
        if (predicate()) return true;
        std::this_thread::sleep_for(5ms);
    } while (Clock::now() < deadline);
    return predicate();
}

struct Gate {
    std::mutex mutex;
    std::condition_variable cv;
    unsigned entered{0};
    bool released{false};
    bool expired{false};

    void block() {
        std::unique_lock<std::mutex> lock(mutex);
        ++entered;
        cv.notify_all();
        if (!cv.wait_for(lock, 30s, [this] { return released; })) expired = true;
    }
    bool wait_entered(unsigned count = 1) {
        std::unique_lock<std::mutex> lock(mutex);
        return cv.wait_for(lock, 5s, [this, count] { return entered >= count; });
    }
    void release() {
        std::lock_guard<std::mutex> lock(mutex);
        released = true;
        cv.notify_all();
    }
};

struct Replies {
    struct Item {
        unsigned count{0};
        ComRet status{ComRet::INTERNAL_ERR};
        Bytes bytes;
    };
    explicit Replies(std::size_t count) : items(count) {}
    std::mutex mutex;
    std::condition_variable cv;
    std::vector<Item> items;
    std::set<std::thread::id> threads;
    unsigned total{0};

    void record(std::size_t index, ComRet status, const Bytes& bytes) {
        std::lock_guard<std::mutex> lock(mutex);
        auto& item = items.at(index);
        ++item.count;
        item.status = status;
        item.bytes = bytes;
        threads.insert(std::this_thread::get_id());
        ++total;
        cv.notify_all();
    }
    bool wait(unsigned count, std::chrono::milliseconds timeout = 5s) {
        std::unique_lock<std::mutex> lock(mutex);
        return cv.wait_for(lock, timeout, [this, count] { return total >= count; });
    }
    Item get(std::size_t index) {
        std::lock_guard<std::mutex> lock(mutex);
        return items.at(index);
    }
    unsigned count() {
        std::lock_guard<std::mutex> lock(mutex);
        return total;
    }
};

MethodReplyCallback reply_to(const std::shared_ptr<Replies>& replies, std::size_t index) {
    return [replies, index](ComRet status, const Bytes& bytes) {
        replies->record(index, status, bytes);
    };
}

struct Events {
    std::mutex mutex;
    std::vector<std::string> values;
    void record(const Wire& value) {
        std::lock_guard<std::mutex> lock(mutex);
        values.push_back(value.text);
    }
    bool contains(const std::string& value) {
        std::lock_guard<std::mutex> lock(mutex);
        for (const auto& item : values) if (item == value) return true;
        return false;
    }
    std::size_t count() {
        std::lock_guard<std::mutex> lock(mutex);
        return values.size();
    }
};

struct Endpoint {
    ServiceConfig config;
    std::shared_ptr<runtime::DdsBackend> backend;
    std::unique_ptr<ServiceProxy> proxy;
    std::unique_ptr<ServiceSkeleton> skeleton;
    bool stopped{false};

    void stop() {
        if (stopped) return;
        stopped = true;
        // 后端工作线程退出后才能销毁其回调引用的上层对象。
        if (backend) backend->stop();
        if (proxy) proxy->stop();
        if (skeleton) skeleton->stop();
        proxy.reset();
        skeleton.reset();
    }
    ~Endpoint() { stop(); }
};

class DdsBackendTest : public ::testing::Test {
protected:
    ServiceId sid{next_service.fetch_add(1)};
    std::vector<std::unique_ptr<Endpoint>> endpoints;
    std::vector<std::shared_ptr<Gate>> gates;
    std::vector<std::thread> workers;

    ServiceConfig config(const std::string& role) const {
        ServiceConfig result;
        result.service_id = sid;
        result.instance_id = 1;
        result.major_version = 1;
        result.minor_version = 0;
        result.role = role;
        return result;
    }
    void TearDown() override {
        for (const auto& gate : gates) gate->release();
        // 先唤醒客户端调用并汇合测试线程，再停止服务端。
        for (auto& endpoint : endpoints) {
            if (endpoint->config.role == "client" && endpoint->backend)
                endpoint->backend->stop();
        }
        join_workers();
        for (auto& endpoint : endpoints) endpoint->stop();
        for (const auto& gate : gates) {
            std::lock_guard<std::mutex> lock(gate->mutex);
            EXPECT_FALSE(gate->expired);
        }
        endpoints.clear();
    }
    void join_workers() {
        for (auto& worker : workers) if (worker.joinable()) worker.join();
        workers.clear();
    }
    std::shared_ptr<Gate> make_gate() {
        auto gate = std::make_shared<Gate>();
        gates.push_back(gate);
        return gate;
    }
    Endpoint* make_endpoint(ServiceConfig cfg, std::uint32_t domain) {
        auto endpoint = std::make_unique<Endpoint>();
        endpoint->config = std::move(cfg);
        endpoint->backend = std::make_shared<runtime::DdsBackend>(
            runtime::DdsBackendOptions{domain});
        auto* result = endpoint.get();
        endpoints.push_back(std::move(endpoint));
        if (!result->backend->init(result->config) || !result->backend->start()) return nullptr;
        return result;
    }
    Endpoint* server(const std::function<void(ServiceSkeleton&)>& setup = {},
                     bool offer = true) {
        return server_with(config("server"), DOMAIN, setup, offer);
    }
    Endpoint* server_with(ServiceConfig cfg, std::uint32_t domain,
                          const std::function<void(ServiceSkeleton&)>& setup = {},
                          bool offer = true) {
        auto* endpoint = make_endpoint(std::move(cfg), domain);
        if (!endpoint) return nullptr;
        endpoint->skeleton = std::make_unique<ServiceSkeleton>(endpoint->backend, endpoint->config);
        auto& skeleton = *endpoint->skeleton;
        skeleton.set_e2e_enabled(false);
        skeleton.set_fragmentation_enabled(false);
        if (!skeleton.init()) return nullptr;
        skeleton.register_method<Wire, Wire>(ECHO, [](const Wire& request) { return request; });
        skeleton.register_event<Wire>(EVENT);
        if (setup) setup(skeleton);
        if (offer && !skeleton.start()) return nullptr;
        return endpoint;
    }
    Endpoint* client(const std::shared_ptr<Events>& events = {}) {
        return client_with(config("client"), DOMAIN, events);
    }
    Endpoint* client_with(ServiceConfig cfg, std::uint32_t domain,
                          const std::shared_ptr<Events>& events = {}) {
        auto* endpoint = make_endpoint(std::move(cfg), domain);
        if (!endpoint) return nullptr;
        endpoint->proxy = std::make_unique<ServiceProxy>(endpoint->backend, endpoint->config);
        auto& proxy = *endpoint->proxy;
        proxy.configure_connection_safety(false, 3000, false, 10, 100, 2);
        proxy.set_e2e_enabled(false);
        proxy.set_fragmentation_enabled(false);
        if (!proxy.init()) return nullptr;
        if (events) proxy.subscribe_event<Wire>(EVENT, [events](const Wire& value) { events->record(value); });
        if (!proxy.start()) return nullptr;
        return endpoint;
    }
    bool available(Endpoint* endpoint) {
        return wait_until([endpoint] { return endpoint->proxy->is_available(); }, 10s);
    }
    ComRet sync(Endpoint* endpoint, MethodId method, const Bytes& request,
                Bytes& response, std::uint32_t timeout = 3000) {
        const auto& cfg = endpoint->config;
        return endpoint->backend->call_method_sync(cfg.service_id, cfg.instance_id,
                                                   method, request, response, timeout);
    }
    bool async(Endpoint* endpoint, MethodId method, const Bytes& request,
               MethodReplyCallback callback, std::uint32_t timeout = 3000) {
        const auto& cfg = endpoint->config;
        return endpoint->backend->call_method_async(cfg.service_id, cfg.instance_id,
                                                    method, request, std::move(callback), timeout);
    }
    bool publish_until(Endpoint* endpoint, const std::string& text,
                        const std::function<bool()>& received) {
        return wait_until([endpoint, &text, &received] {
            endpoint->skeleton->publish_event(EVENT, Wire{text});
            return received();
        });
    }
    void expect_echo(Endpoint* endpoint, const std::string& text) {
        try {
            const auto response = endpoint->proxy->call_method_sync<Wire, Wire>(ECHO, Wire{text}, 3000);
            EXPECT_EQ(response.text, text);
        } catch (const std::exception& error) {
            ADD_FAILURE() << error.what();
        }
    }
    void expect_reply(const std::shared_ptr<Replies>& replies, std::size_t index,
                      ComRet status, const Bytes& bytes = {}) {
        const auto item = replies->get(index);
        EXPECT_EQ(item.count, 1u) << index;
        EXPECT_EQ(item.status, status) << index;
        if (status == ComRet::OK) EXPECT_EQ(item.bytes, bytes) << index;
    }
};

TEST_F(DdsBackendTest, FactoryAndDomainOptionsUseRealDds) {
    EXPECT_EQ(runtime::DdsBackendOptions{}.domain_id, 0u);
    EXPECT_EQ(runtime::DdsBackend::MAX_PAYLOAD_SIZE, 65536u);
    EXPECT_EQ(runtime::DdsBackend::MAX_PENDING_CALLS, 128u);
    auto backend = runtime::create_backend(runtime::BackendType::DDS);
    ASSERT_NE(backend, nullptr);
    EXPECT_NE(dynamic_cast<runtime::DdsBackend*>(backend.get()), nullptr);
    backend->stop();
    for (const auto domain : {0u, 232u}) {
        runtime::DdsBackend valid(runtime::DdsBackendOptions{domain});
        EXPECT_TRUE(valid.init(config("client"))) << domain;
        valid.stop();
    }
    for (const auto domain : {233u, std::numeric_limits<std::uint32_t>::max()}) {
        runtime::DdsBackend invalid(runtime::DdsBackendOptions{domain});
        EXPECT_FALSE(invalid.init(config("client"))) << domain;
        invalid.stop();
    }
}

TEST_F(DdsBackendTest, ClientFirstStartDoesNotOfferThenProxyRpcAndEvent) {
    auto events = std::make_shared<Events>();
    auto* c = client(events);
    ASSERT_NE(c, nullptr);
    auto* s = server({}, false);
    ASSERT_NE(s, nullptr);
    EXPECT_FALSE(wait_until([c] { return c->proxy->is_available(); }, 350ms));
    Bytes response;
    EXPECT_EQ(sync(c, ECHO, Wire{"before-offer"}.serialize(), response), ComRet::NOT_AVAILABLE);
    auto rejected = std::make_shared<Replies>(1);
    EXPECT_FALSE(async(c, ECHO, {}, reply_to(rejected, 0)));
    auto rejected_proxy = std::make_shared<Replies>(1);
    c->proxy->call_method_async<Wire, Wire>(ECHO, Wire{"before-offer"}, reply_to(rejected_proxy, 0));
    expect_reply(rejected_proxy, 0, ComRet::INTERNAL_ERR);
    ASSERT_TRUE(s->skeleton->start());
    ASSERT_TRUE(available(c));
    expect_echo(c, "client=1;seq=0");
    EXPECT_TRUE(publish_until(s, "first-event", [events] { return events->contains("first-event"); }));
    c->stop();
    EXPECT_EQ(rejected->count(), 0u);
}

TEST_F(DdsBackendTest, TwoConcurrentClientsKeepRepliesSeparateAndBroadcast) {
    auto* s = server();
    ASSERT_NE(s, nullptr);
    auto first_events = std::make_shared<Events>();
    auto second_events = std::make_shared<Events>();
    auto* first = client(first_events);
    auto* second = client(second_events);
    ASSERT_NE(first, nullptr);
    ASSERT_NE(second, nullptr);
    ASSERT_TRUE(available(first));
    ASSERT_TRUE(available(second));
    constexpr unsigned requests = 32;
    auto replies = std::make_shared<Replies>(requests * 2);
    auto launch = make_gate();
    for (unsigned id = 0; id < 2; ++id) {
        auto backend = (id == 0 ? first : second)->backend;
        workers.emplace_back([backend, replies, launch, id, service = sid] {
            launch->block();
            for (unsigned seq = 0; seq < requests; ++seq) {
                const auto payload = Wire{"client=" + std::to_string(id) + ";seq=" + std::to_string(seq)}.serialize();
                const auto index = id * requests + seq;
                if (!backend->call_method_async(service, 1, ECHO, payload, reply_to(replies, index), 5000))
                    replies->record(index, ComRet::INTERNAL_ERR, {});
            }
        });
    }
    ASSERT_TRUE(launch->wait_entered(2));
    launch->release();
    join_workers();
    ASSERT_TRUE(replies->wait(requests * 2));
    EXPECT_TRUE(publish_until(s, "broadcast", [first_events, second_events] {
        return first_events->contains("broadcast") && second_events->contains("broadcast");
    }));
    first->stop();
    const auto stopped_count = first_events->count();
    EXPECT_TRUE(publish_until(s, "survivor", [second_events] { return second_events->contains("survivor"); }));
    expect_echo(second, "client=1;seq=32");
    second->stop();
    EXPECT_EQ(first_events->count(), stopped_count);
    EXPECT_EQ(replies->count(), requests * 2);
    for (unsigned id = 0; id < 2; ++id)
        for (unsigned seq = 0; seq < requests; ++seq)
            expect_reply(replies, id * requests + seq, ComRet::OK,
                         Wire{"client=" + std::to_string(id) + ";seq=" + std::to_string(seq)}.serialize());
}

TEST_F(DdsBackendTest, EightThreadsMixSyncAndAsyncWithSingleSerialHandlerWorker) {
    struct HandlerState {
        std::mutex mutex;
        std::set<std::thread::id> threads;
        std::atomic<unsigned> active{0};
        std::atomic<unsigned> overlap{0};
        std::atomic<unsigned> calls{0};
    };
    auto handlers = std::make_shared<HandlerState>();
    auto* s = server([handlers](ServiceSkeleton& skeleton) {
        skeleton.register_method<Wire, Wire>(ECHO, [handlers](const Wire& request) {
            if (handlers->active.fetch_add(1) != 0) ++handlers->overlap;
            {
                std::lock_guard<std::mutex> lock(handlers->mutex);
                handlers->threads.insert(std::this_thread::get_id());
            }
            ++handlers->calls;
            --handlers->active;
            return request;
        });
    });
    ASSERT_NE(s, nullptr);
    auto* c = client();
    ASSERT_NE(c, nullptr);
    ASSERT_TRUE(available(c));
    constexpr unsigned thread_count = 8;
    constexpr unsigned per_thread = 8;
    constexpr unsigned total = thread_count * per_thread * 2;
    auto replies = std::make_shared<Replies>(total);
    auto launch = make_gate();
    for (unsigned thread = 0; thread < thread_count; ++thread) {
        workers.emplace_back([backend = c->backend, replies, launch, thread, service = sid] {
            launch->block();
            for (unsigned seq = 0; seq < per_thread; ++seq) {
                const unsigned index = (thread * per_thread + seq) * 2;
                const auto first = Wire{"client=single;seq=" + std::to_string(index)}.serialize();
                const auto second = Wire{"client=single;seq=" + std::to_string(index + 1)}.serialize();
                if (!backend->call_method_async(service, 1, ECHO, first, reply_to(replies, index), 5000))
                    replies->record(index, ComRet::INTERNAL_ERR, {});
                Bytes response;
                const auto ret = backend->call_method_sync(service, 1, ECHO, second, response, 5000);
                replies->record(index + 1, ret, response);
            }
        });
    }
    ASSERT_TRUE(launch->wait_entered(thread_count));
    launch->release();
    join_workers();
    ASSERT_TRUE(replies->wait(total));
    c->stop();
    s->stop();
    EXPECT_EQ(replies->count(), total);
    EXPECT_EQ(handlers->calls.load(), total);
    EXPECT_EQ(handlers->overlap.load(), 0u);
    EXPECT_EQ(handlers->threads.size(), 1u);
    for (unsigned index = 0; index < total; ++index)
        expect_reply(replies, index, ComRet::OK, Wire{"client=single;seq=" + std::to_string(index)}.serialize());
}

TEST_F(DdsBackendTest, SyncAndAsyncTimeoutIgnoreLateRepliesExactlyOnce) {
    auto gate = make_gate();
    auto* s = server([gate](ServiceSkeleton& skeleton) {
        skeleton.register_method<Wire, Wire>(BLOCKED, [gate](const Wire& request) {
            gate->block();
            return request;
        });
    });
    ASSERT_NE(s, nullptr);
    auto* c = client();
    ASSERT_NE(c, nullptr);
    ASSERT_TRUE(available(c));
    auto replies = std::make_shared<Replies>(2);
    ASSERT_TRUE(async(c, BLOCKED, Wire{"late-async"}.serialize(), reply_to(replies, 0), 300));
    ASSERT_TRUE(gate->wait_entered());
    Bytes response;
    EXPECT_EQ(sync(c, ECHO, Wire{"late-sync"}.serialize(), response, 100), ComRet::TIMEOUT);
    ASSERT_TRUE(replies->wait(1));
    expect_reply(replies, 0, ComRet::TIMEOUT);
    gate->release();
    // 同一响应流中的屏障应答确认迟到应答已经越过接收路径。
    ASSERT_TRUE(async(c, ECHO, Wire{"barrier"}.serialize(), reply_to(replies, 1)));
    ASSERT_TRUE(replies->wait(2));
    expect_echo(c, "after-timeout");
    c->stop();
    EXPECT_EQ(replies->count(), 2u);
    expect_reply(replies, 0, ComRet::TIMEOUT);
    expect_reply(replies, 1, ComRet::OK, Wire{"barrier"}.serialize());
}

TEST_F(DdsBackendTest, AsyncCallbackCanSubmitAnotherAsyncWithoutInternalLock) {
    auto* s = server();
    ASSERT_NE(s, nullptr);
    auto* c = client();
    ASSERT_NE(c, nullptr);
    ASSERT_TRUE(available(c));
    auto replies = std::make_shared<Replies>(2);
    auto accepted = std::make_shared<std::atomic<bool>>(false);
    ASSERT_TRUE(async(c, ECHO, Wire{"outer"}.serialize(),
        [backend = c->backend, replies, accepted, service = sid](ComRet status, const Bytes& bytes) {
            replies->record(0, status, bytes);
            accepted->store(backend->call_method_async(service, 1, ECHO,
                Wire{"inner"}.serialize(), reply_to(replies, 1), 3000));
        }));
    ASSERT_TRUE(replies->wait(2));
    c->stop();
    EXPECT_TRUE(accepted->load());
    EXPECT_EQ(replies->count(), 2u);
    EXPECT_EQ(replies->threads.size(), 1u);
    expect_reply(replies, 0, ComRet::OK, Wire{"outer"}.serialize());
    expect_reply(replies, 1, ComRet::OK, Wire{"inner"}.serialize());
}

TEST_F(DdsBackendTest, StopOfferAndReofferRestoreRpcAndEvents) {
    auto* s = server();
    ASSERT_NE(s, nullptr);
    auto events = std::make_shared<Events>();
    auto* c = client(events);
    ASSERT_NE(c, nullptr);
    ASSERT_TRUE(available(c));
    expect_echo(c, "before-withdraw");
    ASSERT_TRUE(publish_until(s, "before-withdraw", [events] { return events->contains("before-withdraw"); }));
    s->skeleton->stop();
    ASSERT_TRUE(wait_until([c] { return !c->proxy->is_available(); }));
    Bytes response;
    EXPECT_EQ(sync(c, ECHO, {}, response), ComRet::NOT_AVAILABLE);
    ASSERT_TRUE(s->skeleton->start());
    ASSERT_TRUE(available(c));
    expect_echo(c, "after-reoffer");
    EXPECT_TRUE(publish_until(s, "after-reoffer", [events] { return events->contains("after-reoffer"); }));
}

TEST_F(DdsBackendTest, ServerRestartRestoresRpcAndEventsWithoutResubscribe) {
    auto* s = server();
    ASSERT_NE(s, nullptr);
    auto events = std::make_shared<Events>();
    auto* c = client(events);
    ASSERT_NE(c, nullptr);
    ASSERT_TRUE(available(c));
    expect_echo(c, "before-restart");
    ASSERT_TRUE(publish_until(s, "old-server", [events] { return events->contains("old-server"); }));
    s->stop();
    ASSERT_TRUE(wait_until([c] { return !c->proxy->is_available(); }, 10s));
    auto* restarted = server();
    ASSERT_NE(restarted, nullptr);
    ASSERT_TRUE(available(c));
    expect_echo(c, "after-restart");
    EXPECT_TRUE(publish_until(restarted, "new-server", [events] { return events->contains("new-server"); }));
}

TEST_F(DdsBackendTest, DomainServiceInstanceAndExactVersionsAreIsolated) {
    auto* s = server();
    ASSERT_NE(s, nullptr);
    auto* good = client();
    ASSERT_NE(good, nullptr);
    ASSERT_TRUE(available(good));
    std::vector<Endpoint*> isolated;
    std::vector<std::shared_ptr<Events>> events;
    std::vector<std::uint32_t> domains;
    for (unsigned variant = 0; variant < 5; ++variant) {
        auto cfg = config("client");
        const auto domain = variant == 0 ? DOMAIN + 1 : DOMAIN;
        if (variant == 1) ++cfg.major_version;
        if (variant == 2) ++cfg.minor_version;
        if (variant == 3) ++cfg.instance_id;
        if (variant == 4) cfg.service_id = next_service.fetch_add(1);
        auto received = std::make_shared<Events>();
        auto* c = client_with(cfg, domain, received);
        ASSERT_NE(c, nullptr);
        isolated.push_back(c);
        events.push_back(received);
        domains.push_back(domain);
    }
    EXPECT_FALSE(wait_until([&] {
        s->skeleton->publish_event(EVENT, Wire{"wrong-service"});
        for (std::size_t i = 0; i < isolated.size(); ++i)
            if (isolated[i]->proxy->is_available() || events[i]->count() != 0) return true;
        return false;
    }, 350ms));
    for (std::size_t i = 0; i < isolated.size(); ++i) {
        SCOPED_TRACE(i);
        Bytes response;
        EXPECT_EQ(sync(isolated[i], ECHO, {}, response), ComRet::NOT_AVAILABLE);
        auto cfg = isolated[i]->config;
        cfg.role = "server";
        auto* matching = server_with(cfg, domains[i]);
        ASSERT_NE(matching, nullptr);
        ASSERT_TRUE(available(isolated[i]));
        expect_echo(isolated[i], "matching=" + std::to_string(i));
        const auto marker = "matched-event=" + std::to_string(i);
        EXPECT_TRUE(publish_until(matching, marker, [received = events[i], marker] { return received->contains(marker); }));
        EXPECT_FALSE(events[i]->contains("wrong-service"));
    }
    expect_echo(good, "original-still-available");
}

TEST_F(DdsBackendTest, PayloadBoundaryRequestsRepliesAndEvents) {
    auto* s = server([](ServiceSkeleton& skeleton) {
        skeleton.register_method<Wire, Wire>(LARGE_REPLY, [](const Wire&) {
            return Wire{std::string(65537, 'r')};
        });
    });
    ASSERT_NE(s, nullptr);
    auto events = std::make_shared<Events>();
    auto* c = client(events);
    ASSERT_NE(c, nullptr);
    ASSERT_TRUE(available(c));
    Bytes maximum(65536);
    for (std::size_t i = 0; i < maximum.size(); ++i) maximum[i] = static_cast<std::uint8_t>(i % 251);
    Bytes oversized(65537, 0x5a);
    Bytes response;
    EXPECT_EQ(sync(c, ECHO, maximum, response, 5000), ComRet::OK);
    EXPECT_EQ(response, maximum);
    EXPECT_EQ(sync(c, ECHO, oversized, response), ComRet::INTERNAL_ERR);
    EXPECT_EQ(sync(c, LARGE_REPLY, {}, response), ComRet::INTERNAL_ERR);
    auto replies = std::make_shared<Replies>(3);
    ASSERT_TRUE(async(c, ECHO, maximum, reply_to(replies, 0), 5000));
    EXPECT_FALSE(async(c, ECHO, oversized, reply_to(replies, 1)));
    ASSERT_TRUE(async(c, LARGE_REPLY, {}, reply_to(replies, 2)));
    ASSERT_TRUE(replies->wait(2));
    const std::string event_text(maximum.begin(), maximum.end());
    EXPECT_TRUE(publish_until(s, event_text, [events, event_text] { return events->contains(event_text); }));
    EXPECT_FALSE(s->backend->send_event(sid, 1, EVENT, oversized));
    c->stop();
    EXPECT_EQ(replies->count(), 2u);
    expect_reply(replies, 0, ComRet::OK, maximum);
    EXPECT_EQ(replies->get(1).count, 0u);
    expect_reply(replies, 2, ComRet::INTERNAL_ERR);
}

TEST_F(DdsBackendTest, UnknownMethodAndThrowingHandlerReturnExactErrors) {
    auto* s = server([](ServiceSkeleton& skeleton) {
        skeleton.register_method<Wire, Wire>(THROWING, [](const Wire&) -> Wire {
            throw std::runtime_error("测试服务端异常");
        });
    });
    ASSERT_NE(s, nullptr);
    auto* c = client();
    ASSERT_NE(c, nullptr);
    ASSERT_TRUE(available(c));
    Bytes response;
    EXPECT_EQ(sync(c, 0x7fff, {}, response), ComRet::NOT_AVAILABLE);
    EXPECT_EQ(sync(c, THROWING, {}, response), ComRet::INTERNAL_ERR);
    auto replies = std::make_shared<Replies>(2);
    ASSERT_TRUE(async(c, 0x7fff, {}, reply_to(replies, 0)));
    ASSERT_TRUE(async(c, THROWING, {}, reply_to(replies, 1)));
    ASSERT_TRUE(replies->wait(2));
    expect_echo(c, "worker-survives-exception");
    c->stop();
    EXPECT_EQ(replies->count(), 2u);
    expect_reply(replies, 0, ComRet::NOT_AVAILABLE);
    expect_reply(replies, 1, ComRet::INTERNAL_ERR);
}

TEST_F(DdsBackendTest, PendingCapacityRejectsWithoutCallbackAndCanBeReused) {
    auto gate = make_gate();
    auto* s = server([gate](ServiceSkeleton& skeleton) {
        skeleton.register_method<Wire, Wire>(BLOCKED, [gate](const Wire& request) {
            gate->block();
            return request;
        });
    });
    ASSERT_NE(s, nullptr);
    auto* c = client();
    ASSERT_NE(c, nullptr);
    ASSERT_TRUE(available(c));
    constexpr auto capacity = runtime::DdsBackend::MAX_PENDING_CALLS;
    auto replies = std::make_shared<Replies>(capacity + 1);
    ASSERT_TRUE(async(c, BLOCKED, Wire{"0"}.serialize(), reply_to(replies, 0), 20000));
    ASSERT_TRUE(gate->wait_entered());
    for (std::size_t i = 1; i < capacity; ++i)
        ASSERT_TRUE(async(c, ECHO, Wire{std::to_string(i)}.serialize(), reply_to(replies, i), 20000)) << i;
    EXPECT_FALSE(async(c, ECHO, {}, reply_to(replies, capacity), 20000));
    EXPECT_EQ(replies->count(), 0u);
    gate->release();
    ASSERT_TRUE(replies->wait(static_cast<unsigned>(capacity), 10s));
    expect_echo(c, "capacity-reused");
    c->stop();
    EXPECT_EQ(replies->count(), capacity);
    EXPECT_EQ(replies->get(capacity).count, 0u);
    for (std::size_t i = 0; i < capacity; ++i)
        expect_reply(replies, i, ComRet::OK, Wire{std::to_string(i)}.serialize());
}

TEST_F(DdsBackendTest, ClientStopWakesPendingSyncAndAllAsyncAsNotAvailable) {
    auto gate = make_gate();
    auto* s = server([gate](ServiceSkeleton& skeleton) {
        skeleton.register_method<Wire, Wire>(BLOCKED, [gate](const Wire& request) {
            gate->block();
            return request;
        });
    });
    ASSERT_NE(s, nullptr);
    auto* c = client();
    ASSERT_NE(c, nullptr);
    ASSERT_TRUE(available(c));
    constexpr unsigned pending_async = 8;
    auto replies = std::make_shared<Replies>(pending_async + 2);
    workers.emplace_back([backend = c->backend, replies, service = sid] {
        Bytes response;
        const auto status = backend->call_method_sync(service, 1, BLOCKED, {}, response, 20000);
        replies->record(0, status, response);
    });
    ASSERT_TRUE(gate->wait_entered());
    for (unsigned i = 1; i <= pending_async; ++i)
        ASSERT_TRUE(async(c, ECHO, {}, reply_to(replies, i), 20000));
    const auto before = Clock::now();
    c->backend->stop();
    EXPECT_TRUE(replies->wait(pending_async + 1, 2s));
    EXPECT_LT(Clock::now() - before, 3s);
    join_workers();
    EXPECT_FALSE(async(c, ECHO, {}, reply_to(replies, pending_async + 1)));
    gate->release();
    s->stop();
    c->stop();
    EXPECT_EQ(replies->count(), pending_async + 1);
    for (unsigned i = 0; i <= pending_async; ++i) expect_reply(replies, i, ComRet::NOT_AVAILABLE);
    EXPECT_EQ(replies->get(pending_async + 1).count, 0u);
}

TEST_F(DdsBackendTest, EventTopicsSeparateIdsAndGroupsAndDoNotReplayHistory) {
    auto* s = server({}, false);
    ASSERT_NE(s, nullptr);
    ASSERT_TRUE(s->backend->register_event(sid, 1, OTHER_EVENT, 2));
    ASSERT_TRUE(s->skeleton->start());
    auto early_events = std::make_shared<Events>();
    auto* early = client(early_events);
    ASSERT_NE(early, nullptr);
    ASSERT_TRUE(available(early));
    ASSERT_TRUE(publish_until(s, "historical", [early_events] { return early_events->contains("historical"); }));
    auto late_events = std::make_shared<Events>();
    auto other_events = std::make_shared<Events>();
    auto wrong_events = std::make_shared<Events>();
    auto* late = client(late_events);
    auto* other = client();
    auto* wrong = client();
    ASSERT_NE(late, nullptr);
    ASSERT_NE(other, nullptr);
    ASSERT_NE(wrong, nullptr);
    ASSERT_TRUE(other->backend->subscribe_event(sid, 1, OTHER_EVENT, 2,
        [other_events](const Bytes& bytes) { other_events->record(Wire::deserialize(bytes)); }));
    ASSERT_TRUE(wrong->backend->subscribe_event(sid, 1, EVENT, 2,
        [wrong_events](const Bytes& bytes) { wrong_events->record(Wire::deserialize(bytes)); }));
    ASSERT_TRUE(available(late));
    ASSERT_TRUE(available(other));
    ASSERT_TRUE(available(wrong));
    ASSERT_TRUE(wait_until([&] {
        s->backend->send_event(sid, 1, EVENT, Wire{"current-primary"}.serialize());
        s->backend->send_event(sid, 1, OTHER_EVENT, Wire{"current-other"}.serialize());
        return late_events->contains("current-primary") && other_events->contains("current-other");
    }));
    EXPECT_FALSE(wait_until([wrong_events] { return wrong_events->count() != 0; }, 250ms));
    late->stop();
    other->stop();
    wrong->stop();
    EXPECT_FALSE(late_events->contains("historical"));
    EXPECT_FALSE(late_events->contains("current-other"));
    EXPECT_FALSE(other_events->contains("current-primary"));
    EXPECT_EQ(wrong_events->count(), 0u);
}

}

#endif
