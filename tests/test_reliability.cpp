#include "safety/heartbeat.h"
#include "safety/reconnect.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>

using namespace ara::com::safety;

TEST(ReliabilityTest, HeartbeatProviderStopsPromptly) {
    HeartbeatProvider provider;
    provider.start(5000, [] {});
    const auto begin = std::chrono::steady_clock::now();
    provider.stop();
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - begin);
    EXPECT_LT(elapsed.count(), 500);
}

TEST(ReliabilityTest, HeartbeatMonitorStopsPromptly) {
    HeartbeatMonitor monitor;
    monitor.start(5000, [] {});
    const auto begin = std::chrono::steady_clock::now();
    monitor.stop();
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - begin);
    EXPECT_LT(elapsed.count(), 500);
}

TEST(ReliabilityTest, ReconnectWaitsForAvailabilityConfirmation) {
    ReconnectManager manager;
    std::atomic<int> attempts{0};
    std::atomic<int> restored{0};
    manager.configure(1, 10, 2);
    manager.set_callbacks(
        [&] {
            attempts.fetch_add(1);
            return false; // 仅提交发现请求，尚未确认服务可用
        },
        [&] { restored.fetch_add(1); });

    manager.on_connection_lost();
    for (int i = 0; i < 50 && attempts.load() == 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    EXPECT_GT(attempts.load(), 0);
    EXPECT_EQ(restored.load(), 0);

    manager.on_connection_ok();
    manager.stop();
    EXPECT_EQ(restored.load(), 1);
}
