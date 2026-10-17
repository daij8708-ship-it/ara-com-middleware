/**
 * @file    test_monitor.cpp
 * @brief   运行监测指标与结构化日志测试。
 */

#include "common/logger.h"
#include "common/monitor.h"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

namespace {

using ara::com::ComRet;
using ara::com::monitor::Counter;
using ara::com::monitor::Monitor;

std::filesystem::path test_root() {
    const auto path = std::filesystem::temp_directory_path() / "ara-com-observability-tests";
    std::filesystem::remove_all(path);
    std::filesystem::create_directories(path);
    return path;
}

std::string read_all(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::in | std::ios::binary);
    if (!input.is_open()) return {};
    return std::string(std::istreambuf_iterator<char>(input),
                       std::istreambuf_iterator<char>());
}

} // namespace

TEST(Monitor, CountersAndSnapshotJson) {
    auto& metrics = Monitor::instance();
    metrics.configure({"server-test", "null", 4242, 1});

    const auto ok_before = metrics.get(Counter::RpcOk);
    const auto submit_before = metrics.get(Counter::RpcSubmitted);
    metrics.inc(Counter::RpcSubmitted);
    metrics.inc(Counter::PendingCalls);
    metrics.inc(Counter::RpcRequestBytes, 12);
    metrics.complete_rpc(ComRet::OK, 250, 20);

    EXPECT_EQ(metrics.get(Counter::RpcSubmitted), submit_before + 1);
    EXPECT_EQ(metrics.get(Counter::RpcOk), ok_before + 1);
    EXPECT_EQ(metrics.get(Counter::PendingCalls), 0U);
    EXPECT_EQ(metrics.get(Counter::RpcLatencyMaxUs), 250U);

    const std::string snapshot = metrics.snapshot_json();
    EXPECT_NE(snapshot.find("\"schema\":\"ara-com-monitor-v1\""), std::string::npos);
    EXPECT_NE(snapshot.find("\"role\":\"server-test\""), std::string::npos);
    EXPECT_NE(snapshot.find("\"rpc_submitted\":"), std::string::npos);
    EXPECT_NE(snapshot.find("\"stopped\":false"), std::string::npos);

    metrics.mark_stopped();
    EXPECT_TRUE(metrics.stopped());
    EXPECT_NE(metrics.snapshot_json().find("\"stopped\":true"), std::string::npos);

    EXPECT_STREQ(ara::com::monitor::com_ret_name(ComRet::TIMEOUT), "TIMEOUT");
    EXPECT_NE(std::string(ara::com::monitor::com_ret_description(ComRet::TIMEOUT))
                  .find("超时"), std::string::npos);
}

TEST(Monitor, SnapshotWriterWritesLatestState) {
    const auto root = test_root();
    const auto directory = root / "writer";
    auto& metrics = Monitor::instance();
    metrics.configure({"snapshot-test", "null", 7, 2});

    ASSERT_TRUE(metrics.start_snapshot_writer(directory.string(), 100));
    EXPECT_FALSE(metrics.start_snapshot_writer(directory.string(), 100));
    std::this_thread::sleep_for(std::chrono::milliseconds(350));
    metrics.inc(Counter::EventsSent, 3);
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    metrics.mark_stopped();
    metrics.stop_snapshot_writer();

    const auto path = metrics.snapshot_path();
    ASSERT_FALSE(path.empty());
    ASSERT_TRUE(std::filesystem::exists(path));
    const std::string content = read_all(path);
    EXPECT_NE(content.find("\"role\":\"snapshot-test\""), std::string::npos);
    EXPECT_NE(content.find("\"events_sent\":3"), std::string::npos);
    EXPECT_NE(content.find("\"stopped\":true"), std::string::npos);
}

TEST(Logger, JsonLinesRotateAndRetain) {
    const auto root = test_root();
    const auto directory = root / "logger";
    std::filesystem::create_directories(directory);
    const auto path = directory / "run.jsonl";

    ara::com::logger::LogOptions options;
    options.json_lines = true;
    options.rotate_bytes = 500;
    options.max_files = 2;
    options.component = "logger-test";
    options.backend = "dds";

    auto& log = ara::com::logger::AsyncLogger::instance();
    ASSERT_TRUE(log.init(ara::com::LogLevel::INFO, path.string(), 64, options));
    for (int i = 0; i < 30; ++i) {
        log.log(ara::com::LogLevel::INFO, "TEST",
                "message " + std::to_string(i) + " quote=\"x\"");
    }
    log.shutdown();
    EXPECT_EQ(log.dropped(), 0U);

    EXPECT_TRUE(std::filesystem::exists(path));
    EXPECT_TRUE(std::filesystem::exists(path.string() + ".1"));
    EXPECT_TRUE(std::filesystem::exists(path.string() + ".2"));
    EXPECT_FALSE(std::filesystem::exists(path.string() + ".3"));

    const std::string main_file = read_all(path);
    EXPECT_NE(main_file.find("\"component\":\"logger-test\""), std::string::npos);
    EXPECT_NE(main_file.find("\"backend\":\"dds\""), std::string::npos);
    EXPECT_NE(main_file.find("quote=\\\"x\\\""), std::string::npos);

    std::size_t lines = 0;
    for (const auto& entry : std::filesystem::directory_iterator(directory)) {
        std::ifstream input(entry.path());
        std::string line;
        while (std::getline(input, line)) {
            ASSERT_FALSE(line.empty());
            EXPECT_EQ(line.front(), '{');
            EXPECT_EQ(line.back(), '}');
            ++lines;
        }
    }
    // 只保留当前文件 + 两个历史文件，因此总行数远小于写入的 30 条。
    EXPECT_GE(lines, 6U);
    EXPECT_LE(lines, 12U);
}
