/**
 * @file    test_shm_backend.cpp
 * @brief   共享内存通信后端测试。
 *
 * 覆盖单客户端 RPC/Event、多客户端路由与广播、异步超时，
 * 以及服务端重启后的自动重连。
 */

#include "communication/runtime.h"
#include "communication/service_proxy.h"
#include "communication/service_skeleton.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace ara::com;
using namespace std::chrono_literals;

namespace {

constexpr MethodId ECHO_METHOD = 0x0101;
constexpr MethodId SLOW_METHOD = 0x0102;
constexpr EventId COUNT_EVENT = 0x8001;

std::atomic<ServiceId> g_next_service{0x7100};

ServiceId allocate_service_id() {
    return g_next_service.fetch_add(1);
}

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

bool wait_for(const std::function<bool()>& predicate,
              std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) return true;
        std::this_thread::sleep_for(5ms);
    }
    return predicate();
}

struct EventState {
    std::atomic<int> count{0};
    std::atomic<std::int32_t> last{0};
};

struct AsyncState {
    std::atomic<bool> done{false};
    std::atomic<int> status{-1};
    std::mutex text_mutex;
    std::string text;
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

} // namespace

class ShmBackendTest : public ::testing::Test {
protected:
    struct Client {
        std::shared_ptr<runtime::ICommunicationBackend> backend;
        std::unique_ptr<ServiceProxy> proxy;

        void stop() {
            // 先停后端接收线程，避免回调落到已经销毁的 Proxy 上。
            if (backend) {
                backend->stop();
                backend.reset();
            }
            if (proxy) {
                proxy->stop();
                proxy.reset();
            }
        }
    };

    void SetUp() override {
        sid_ = allocate_service_id();
    }

    void TearDown() override {
        for (auto& client : clients_) client->stop();
        clients_.clear();
        stop_server();
    }

    bool start_server(const std::function<void(ServiceSkeleton&)>& setup) {
        server_backend_ = runtime::create_backend(runtime::BackendType::SHM);
        if (!server_backend_) return false;
        if (!server_backend_->init(make_config(sid_, "server"))) return false;
        if (!server_backend_->start()) return false;

        skeleton_ = std::make_unique<ServiceSkeleton>(
            server_backend_, make_config(sid_, "server"));
        if (!skeleton_->init()) return false;
        if (setup) setup(*skeleton_);
        if (!skeleton_->start()) return false;
        skeleton_->enable_heartbeat(500);
        return true;
    }

    void stop_server() {
        if (skeleton_) {
            skeleton_->stop();
            skeleton_.reset();
        }
        if (server_backend_) {
            server_backend_->stop();
            server_backend_.reset();
        }
    }

    Client* add_client(const std::function<void(ServiceProxy&)>& setup = {}) {
        auto client = std::make_unique<Client>();
        client->backend = runtime::create_backend(runtime::BackendType::SHM);
        if (!client->backend) return nullptr;
        if (!client->backend->init(make_config(sid_, "client"))) return nullptr;
        if (!client->backend->start()) return nullptr;

        client->proxy = std::make_unique<ServiceProxy>(
            client->backend, make_config(sid_, "client"));
        if (!client->proxy->init()) return nullptr;
        if (setup) setup(*client->proxy);
        if (!client->proxy->start()) return nullptr;

        clients_.push_back(std::move(client));
        return clients_.back().get();
    }

    bool wait_available(ServiceProxy& proxy,
                        std::chrono::milliseconds timeout = 5000ms) {
        return wait_for([&proxy]() { return proxy.is_available(); }, timeout);
    }

    bool publish_until(CountEvent event, const std::function<bool()>& done,
                       std::chrono::milliseconds timeout = 2000ms) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline) {
            skeleton_->publish_event(COUNT_EVENT, event);
            if (done()) return true;
            std::this_thread::sleep_for(10ms);
        }
        return done();
    }

    ServiceId sid_{0};
    std::shared_ptr<runtime::ICommunicationBackend> server_backend_;
    std::unique_ptr<ServiceSkeleton> skeleton_;
    std::vector<std::unique_ptr<Client>> clients_;
};

TEST_F(ShmBackendTest, SyncRpcAndEvent) {
    ASSERT_TRUE(start_server([](ServiceSkeleton& skeleton) {
        skeleton.register_method<EchoRequest, EchoResponse>(
            ECHO_METHOD, [](const EchoRequest& request) {
                return EchoResponse{"echo:" + request.text};
            });
        skeleton.register_event<CountEvent>(COUNT_EVENT);
    }));

    auto state = std::make_shared<EventState>();
    auto* client = add_client([state](ServiceProxy& proxy) {
        proxy.subscribe_event<CountEvent>(
            COUNT_EVENT, [state](const CountEvent& event) {
                state->last.store(event.value);
                state->count.fetch_add(1);
            });
    });
    ASSERT_NE(client, nullptr);
    ASSERT_TRUE(wait_available(*client->proxy));

    try {
        const auto response = client->proxy->call_method_sync<EchoRequest, EchoResponse>(
            ECHO_METHOD, EchoRequest{"hello"}, 2000);
        EXPECT_EQ(response.text, "echo:hello");
    } catch (const std::exception& e) {
        FAIL() << "sync call failed: " << e.what();
    }

    EXPECT_TRUE(publish_until(CountEvent{42}, [&]() {
        return state->count.load() > 0;
    }));
    EXPECT_EQ(state->last.load(), 42);
}

TEST_F(ShmBackendTest, MultipleClientsRouteAndBroadcast) {
    ASSERT_TRUE(start_server([](ServiceSkeleton& skeleton) {
        skeleton.register_method<EchoRequest, EchoResponse>(
            ECHO_METHOD, [](const EchoRequest& request) {
                return EchoResponse{"echo:" + request.text};
            });
        skeleton.register_event<CountEvent>(COUNT_EVENT);
    }));

    auto first_state = std::make_shared<EventState>();
    auto second_state = std::make_shared<EventState>();
    auto* first = add_client([first_state](ServiceProxy& proxy) {
        proxy.subscribe_event<CountEvent>(
            COUNT_EVENT, [first_state](const CountEvent&) {
                first_state->count.fetch_add(1);
            });
    });
    auto* second = add_client([second_state](ServiceProxy& proxy) {
        proxy.subscribe_event<CountEvent>(
            COUNT_EVENT, [second_state](const CountEvent&) {
                second_state->count.fetch_add(1);
            });
    });
    ASSERT_NE(first, nullptr);
    ASSERT_NE(second, nullptr);
    ASSERT_TRUE(wait_available(*first->proxy));
    ASSERT_TRUE(wait_available(*second->proxy));

    try {
        const auto first_rsp = first->proxy->call_method_sync<EchoRequest, EchoResponse>(
            ECHO_METHOD, EchoRequest{"alpha"}, 2000);
        const auto second_rsp = second->proxy->call_method_sync<EchoRequest, EchoResponse>(
            ECHO_METHOD, EchoRequest{"beta"}, 2000);
        EXPECT_EQ(first_rsp.text, "echo:alpha");
        EXPECT_EQ(second_rsp.text, "echo:beta");
    } catch (const std::exception& e) {
        FAIL() << "multi-client call failed: " << e.what();
    }

    EXPECT_TRUE(publish_until(CountEvent{7}, [&]() {
        return first_state->count.load() > 0 && second_state->count.load() > 0;
    }));

    first->stop();
    std::this_thread::sleep_for(100ms);

    // 停掉第一个客户端后再取基准，避免把队列里尚未处理完的旧事件算进来。
    const int first_after_stop = first_state->count.load();
    const int second_before = second_state->count.load();

    EXPECT_TRUE(publish_until(CountEvent{8}, [&]() {
        return second_state->count.load() > second_before;
    }));
    EXPECT_EQ(first_state->count.load(), first_after_stop);

    try {
        const auto response = second->proxy->call_method_sync<EchoRequest, EchoResponse>(
            ECHO_METHOD, EchoRequest{"still-here"}, 2000);
        EXPECT_EQ(response.text, "echo:still-here");
    } catch (const std::exception& e) {
        FAIL() << "remaining client call failed: " << e.what();
    }
}

TEST_F(ShmBackendTest, AsyncReplyAndTimeout) {
    ASSERT_TRUE(start_server([](ServiceSkeleton& skeleton) {
        skeleton.register_method<EchoRequest, EchoResponse>(
            ECHO_METHOD, [](const EchoRequest& request) {
                return EchoResponse{"echo:" + request.text};
            });
        skeleton.register_method<EchoRequest, EchoResponse>(
            SLOW_METHOD, [](const EchoRequest& request) {
                std::this_thread::sleep_for(300ms);
                return EchoResponse{"slow:" + request.text};
            });
    }));

    auto reply_state = std::make_shared<AsyncState>();
    auto timeout_state = std::make_shared<AsyncState>();
    auto* client = add_client();
    ASSERT_NE(client, nullptr);
    ASSERT_TRUE(wait_available(*client->proxy));

    client->proxy->call_method_async<EchoRequest, EchoResponse>(
        ECHO_METHOD, EchoRequest{"async"},
        [reply_state](ComRet status, const std::vector<std::uint8_t>& bytes) {
            {
                std::lock_guard<std::mutex> lock(reply_state->text_mutex);
                reply_state->text = EchoResponse::deserialize(bytes).text;
            }
            reply_state->status.store(static_cast<int>(status));
            reply_state->done.store(true, std::memory_order_release);
        },
        2000);
    EXPECT_TRUE(wait_for([&]() {
        return reply_state->done.load(std::memory_order_acquire);
    }, 2s));
    EXPECT_EQ(reply_state->status.load(), static_cast<int>(ComRet::OK));
    {
        std::lock_guard<std::mutex> lock(reply_state->text_mutex);
        EXPECT_EQ(reply_state->text, "echo:async");
    }

    client->proxy->call_method_async<EchoRequest, EchoResponse>(
        SLOW_METHOD, EchoRequest{"late"},
        [timeout_state](ComRet status, const std::vector<std::uint8_t>&) {
            timeout_state->status.store(static_cast<int>(status));
            timeout_state->done.store(true, std::memory_order_release);
        },
        50);
    EXPECT_TRUE(wait_for([&]() {
        return timeout_state->done.load(std::memory_order_acquire);
    }, 2s));
    EXPECT_EQ(timeout_state->status.load(), static_cast<int>(ComRet::TIMEOUT));
}

TEST_F(ShmBackendTest, ReconnectsAfterServerRestart) {
    const auto setup = [](ServiceSkeleton& skeleton) {
        skeleton.register_method<EchoRequest, EchoResponse>(
            ECHO_METHOD, [](const EchoRequest& request) {
                return EchoResponse{"echo:" + request.text};
            });
    };
    ASSERT_TRUE(start_server(setup));

    auto* client = add_client();
    ASSERT_NE(client, nullptr);
    ASSERT_TRUE(wait_available(*client->proxy));

    try {
        const auto response = client->proxy->call_method_sync<EchoRequest, EchoResponse>(
            ECHO_METHOD, EchoRequest{"before"}, 2000);
        EXPECT_EQ(response.text, "echo:before");
    } catch (const std::exception& e) {
        FAIL() << "initial call failed: " << e.what();
    }

    stop_server();
    EXPECT_TRUE(wait_for([&]() { return !client->proxy->is_available(); }, 5000ms));

    ASSERT_TRUE(start_server(setup));
    ASSERT_TRUE(wait_available(*client->proxy, 10000ms));

    try {
        const auto response = client->proxy->call_method_sync<EchoRequest, EchoResponse>(
            ECHO_METHOD, EchoRequest{"after"}, 2000);
        EXPECT_EQ(response.text, "echo:after");
    } catch (const std::exception& e) {
        FAIL() << "call after restart failed: " << e.what();
    }
}
