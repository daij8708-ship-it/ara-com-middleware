/**
 * @file    demo_client.cpp
 * @brief   ServiceProxy 客户端示例。
 *
 * 演示内容：
 *   1. 基于 JSON 配置的服务发现
 *   2. VehicleSpeed Event 订阅
 *   3. SensorRequest 同步 Method 调用
 *   4. 异步 Method 调用
 *   5. E2E 校验与分片重组
 *   6. 心跳监控与自动重连
 *
 * 用法：
 *   ./demo_client [config_path]
 *
 */

#include "common/logger.h"
#include "common/json_parser.h"
#include "common/types.h"
#include "communication/runtime.h"
#include "communication/service_proxy.h"
#include "demo_data_types.h"

#include <chrono>
#include <csignal>
#include <iomanip>
#include <iostream>
#include <thread>

using namespace ara::com;

static std::atomic<bool> g_running{true};
static std::atomic<std::uint32_t> g_event_count{0};

void signal_handler(int /*sig*/) {
    g_running.store(false);
}

int main(int argc, char* argv[]) {
    // ---- 初始化日志 ----
    logger::AsyncLogger::instance().init(LogLevel::DEBUG, "demo_client.log");
    ARA_LOG_INFO("DEMO_CLIENT", "===== ara::com Demo Client Starting =====");

    // ---- 解析配置 ----
    std::string config_path = (argc > 1) ? argv[1] : "../config/services.json";
    config::JsonConfigParser parser;
    if (!parser.load_file(config_path)) {
        ARA_LOG_FATAL("DEMO_CLIENT", "Failed to load config: " + config_path);
        return 1;
    }

    // 查找服务 0x1234 的客户端配置
    auto* svc_cfg = parser.find_service(0x1234);
    if (!svc_cfg) {
        ARA_LOG_FATAL("DEMO_CLIENT", "Client config not found for service 0x1234");
        return 1;
    }

    // 将 Proxy 的 role 调整为 client
    ServiceConfig client_cfg = *svc_cfg;
    client_cfg.role = "client";

    // ---- 创建通信后端 ----
#ifdef ARA_COM_USE_VSOMEIP
    auto backend = runtime::create_backend(runtime::BackendType::VSOMEIP);
#else
    auto backend = runtime::create_backend(runtime::BackendType::NULL_BACKEND);
#endif
    if (!backend->init(client_cfg)) {
        ARA_LOG_FATAL("DEMO_CLIENT", "Backend init failed");
        return 1;
    }
    backend->start();

    // ---- 创建 ServiceProxy ----
    ServiceProxy proxy(backend, client_cfg, parser.e2e_config());
    proxy.set_e2e_enabled(parser.e2e_config().enabled);
    proxy.set_fragmentation_enabled(parser.fragmentation_enabled());
    proxy.set_resume_enabled(parser.resume_enabled());
    proxy.set_rpc_idempotency_enabled(parser.rpc_idempotency_enabled());
    proxy.configure_connection_safety(
        parser.heartbeat_enabled(), parser.heartbeat_timeout_ms(),
        parser.reconnect_enabled(), parser.reconnect_initial_delay_ms(),
        parser.reconnect_max_delay_ms(), parser.reconnect_backoff_multiplier());
    proxy.init();

    // 订阅 VehicleSpeed Event
    proxy.subscribe_event<demo::VehicleSpeed>(
        0x8001,
        [](const demo::VehicleSpeed& speed) {
            g_event_count.fetch_add(1);
        // 每50个 Event 记录一次，避免日志过多
            if (g_event_count.load() % 50 == 0) {
                ARA_LOG_INFO("DEMO_CLIENT",
                    "Event received: ts=" + std::to_string(speed.timestamp_ms) +
                    "ms speed=" + std::to_string(speed.speed_kmh).substr(0, 6) +
                    "km/h conf=" + std::to_string(speed.confidence) + "%" +
                    " [total=" + std::to_string(g_event_count.load()) + "]");
            }
        });

    // 配置服务可用性监控
    proxy.on_availability_change([](ServiceId sid, InstanceId iid, ServiceAvailability avail) {
        const char* state = (avail == ServiceAvailability::AVAILABLE) ? "AVAILABLE" :
                            (avail == ServiceAvailability::UNAVAILABLE) ? "UNAVAILABLE" : "UNKNOWN";
        ARA_LOG_INFO("DEMO_CLIENT", "Service " + std::to_string(sid) + "." +
                     std::to_string(iid) + " -> " + state);
    });

    // 启动 Proxy
    if (!proxy.start()) {
        ARA_LOG_FATAL("DEMO_CLIENT", "Failed to start proxy");
        return 1;
    }

    ARA_LOG_INFO("DEMO_CLIENT", "Proxy running. Receiving events...");

    // ---- 信号处理 ----
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    // ---- 主循环：接收 Event 并周期调用 Method ----
    std::uint32_t tick = 0;
    while (g_running.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        ++tick;

        // 每2秒发起一次同步 Method 调用
        if (tick % 4 == 0) {
            demo::SensorRequest req;
            req.sensor_id = 1;
            req.command = (tick / 4) % 3;  // 轮流发送读取、校准、重置命令

            try {
                auto rsp = proxy.call_method_sync<demo::SensorRequest, demo::SensorResponse>(
                    0x0101, req, 2000);

                ARA_LOG_INFO("DEMO_CLIENT", "Sync RPC result: sensor=" +
                             std::to_string(rsp.sensor_id) +
                             " status=" + std::to_string(rsp.status) +
                             " value=" + std::to_string(rsp.value) +
                             " msg=\"" + rsp.message + "\"");
            } catch (const TimeoutException& e) {
                ARA_LOG_ERROR("DEMO_CLIENT", "Sync RPC timeout: " + std::string(e.what()));
            } catch (const ServiceNotAvailableException& e) {
                ARA_LOG_ERROR("DEMO_CLIENT", "Service not available: " + std::string(e.what()));
            }

            // 同时发起异步调用
            demo::SensorRequest async_req;
            async_req.sensor_id = 2;
            async_req.command = 0;  // 读取
            proxy.call_method_async<demo::SensorRequest, demo::SensorResponse>(
                0x0101, async_req,
                [](ComRet status, const std::vector<std::uint8_t>& /*raw_rsp*/) {
                    ARA_LOG_INFO("DEMO_CLIENT", "Async RPC completed: status=" +
                                 std::to_string(static_cast<int>(status)));
                },
                2000);
        }
    }

    // ---- 平稳停止 ----
    ARA_LOG_INFO("DEMO_CLIENT", "Shutting down. Total events received: " +
                 std::to_string(g_event_count.load()));

    proxy.stop();
    backend->stop();
    logger::AsyncLogger::instance().shutdown();

    std::cout << "Demo client exited cleanly. Events received: "
              << g_event_count.load() << std::endl;
    return 0;
}
