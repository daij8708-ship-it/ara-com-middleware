/**
 * @file    test_json_parser.cpp
 * @brief   JSON 配置解析器单元测试。
 */

#include "common/json_parser.h"
#include <gtest/gtest.h>

using namespace ara::com::config;

// =========================================================================
//  基本解析
// =========================================================================

TEST(JsonParserTest, ParseServicesArray) {
    JsonConfigParser parser;
    const char* json = R"({
        "services": [
            {
                "service_id": 4660,
                "instance_id": 1,
                "major_version": 1,
                "minor_version": 0,
                "ip_address": "192.168.1.100",
                "port": 30500,
                "protocol": "UDP",
                "role": "server"
            },
            {
                "service_id": 4660,
                "instance_id": 1,
                "major_version": 1,
                "minor_version": 0,
                "ip_address": "192.168.1.100",
                "port": 30500,
                "protocol": "UDP",
                "role": "client"
            }
        ]
    })";

    ASSERT_TRUE(parser.load_string(json));
    EXPECT_EQ(parser.services().size(), 2);

    auto* svc = parser.find_service(4660);
    ASSERT_NE(svc, nullptr);
    EXPECT_EQ(svc->service_id, 4660);
    EXPECT_EQ(svc->ip_address, "192.168.1.100");
    EXPECT_EQ(svc->port, 30500);
    EXPECT_EQ(svc->protocol, "UDP");
}

// =========================================================================
//  按角色查找
// =========================================================================

TEST(JsonParserTest, FilterByRole) {
    JsonConfigParser parser;
    const char* json = R"({
        "services": [
            {"service_id": 100, "role": "server", "port": 10001},
            {"service_id": 200, "role": "client", "port": 10002},
            {"service_id": 300, "role": "server", "port": 10003}
        ]
    })";

    ASSERT_TRUE(parser.load_string(json));

    auto servers = parser.services_by_role("server");
    EXPECT_EQ(servers.size(), 2);

    auto clients = parser.services_by_role("client");
    EXPECT_EQ(clients.size(), 1);
}

// =========================================================================
//  E2E 配置
// =========================================================================

TEST(JsonParserTest, ParseE2EConfig) {
    JsonConfigParser parser;
    const char* json = R"({
        "services": [],
        "e2e": {
            "enabled": true,
            "max_counter": 14
        }
    })";

    ASSERT_TRUE(parser.load_string(json));
    EXPECT_TRUE(parser.e2e_config().enabled);
    EXPECT_EQ(parser.e2e_config().max_counter, 14);
}

// =========================================================================
//  心跳配置
// =========================================================================

TEST(JsonParserTest, ParseHeartbeatConfig) {
    JsonConfigParser parser;
    const char* json = R"({
        "services": [],
        "heartbeat": {
            "enabled": true,
            "interval_ms": 500,
            "timeout_ms": 1500
        }
    })";

    ASSERT_TRUE(parser.load_string(json));
    EXPECT_TRUE(parser.heartbeat_enabled());
    EXPECT_EQ(parser.heartbeat_interval_ms(), 500);
    EXPECT_EQ(parser.heartbeat_timeout_ms(), 1500);
}

// =========================================================================
//  重连配置
// =========================================================================

TEST(JsonParserTest, ParseReconnectConfig) {
    JsonConfigParser parser;
    const char* json = R"({
        "services": [],
        "reconnect": {
            "enabled": true,
            "initial_delay_ms": 200,
            "max_delay_ms": 10000,
            "backoff_multiplier": 3
        }
    })";

    ASSERT_TRUE(parser.load_string(json));
    EXPECT_TRUE(parser.reconnect_enabled());
    EXPECT_EQ(parser.reconnect_initial_delay_ms(), 200);
    EXPECT_EQ(parser.reconnect_max_delay_ms(), 10000);
    EXPECT_EQ(parser.reconnect_backoff_multiplier(), 3);
}

// =========================================================================
//  默认值
// =========================================================================

TEST(JsonParserTest, DefaultValues) {
    JsonConfigParser parser;
    const char* json = R"({
        "services": [
            {"service_id": 1}
        ]
    })";

    ASSERT_TRUE(parser.load_string(json));
    auto* svc = parser.find_service(1);
    ASSERT_NE(svc, nullptr);
    EXPECT_EQ(svc->instance_id, 1);
    EXPECT_EQ(svc->ip_address, "127.0.0.1");
    EXPECT_EQ(svc->port, 30500);
    EXPECT_EQ(svc->protocol, "UDP");
    EXPECT_EQ(svc->role, "server");
}

// =========================================================================
//  服务缺失
// =========================================================================

TEST(JsonParserTest, FindMissingService) {
    JsonConfigParser parser;
    const char* json = R"({"services": []})";

    ASSERT_TRUE(parser.load_string(json));
    EXPECT_EQ(parser.find_service(9999), nullptr);
    EXPECT_EQ(parser.services().size(), 0);
}

// =========================================================================
//  分片配置
// =========================================================================

TEST(JsonParserTest, ParseFragmentationConfig) {
    JsonConfigParser parser;
    const char* json = R"({
        "services": [],
        "fragmentation": {
            "enabled": true,
            "mtu": 1500
        }
    })";

    ASSERT_TRUE(parser.load_string(json));
    EXPECT_TRUE(parser.fragmentation_enabled());
    EXPECT_EQ(parser.fragmentation_mtu(), 1500);
}

// =========================================================================
//  非法 JSON
// =========================================================================

TEST(JsonParserTest, InvalidJson) {
    JsonConfigParser parser;
    EXPECT_FALSE(parser.load_string("this is not json"));
    EXPECT_FALSE(parser.load_string("{"));
    EXPECT_FALSE(parser.load_string(""));
}

// =========================================================================
//  空服务列表
// =========================================================================

TEST(JsonParserTest, EmptyServices) {
    JsonConfigParser parser;
    const char* json = R"({"services": []})";

    ASSERT_TRUE(parser.load_string(json));
    EXPECT_EQ(parser.services().size(), 0);
    auto servers = parser.services_by_role("server");
    EXPECT_EQ(servers.size(), 0);
}
