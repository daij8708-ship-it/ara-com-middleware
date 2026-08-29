/**
 * @file    demo_server.cpp
 * @brief   ServiceSkeleton 服务端示例。
 *
 * 演示内容：
 *   1. 从 JSON 配置初始化服务
 *   2. 每100毫秒发布 VehicleSpeed Event
 *   3. 注册 SensorRequest/Response Method
 *   4. E2E 保护与分片
 *   5. 心跳发送
 *
 * 用法：
 *   ./demo_server [config_path]
 *
 */

#include "common/logger.h"
#include "common/json_parser.h"
#include "common/types.h"
#include "communication/runtime.h"
#include "communication/service_skeleton.h"
#include "demo_data_types.h"

#include <chrono>
#include <cmath>
#include <csignal>
#include <iostream>
#include <thread>

using namespace ara::com;

// 平稳停止全局标志
static std::atomic<bool> g_running{true};

void signal_handler(int /*sig*/) {
    g_running.store(false);
}

int main(int argc, char* argv[]) {
    // ---- 初始化日志 ----
    logger::AsyncLogger::instance().init(LogLevel::DEBUG, "demo_server.log");
    ARA_LOG_INFO("DEMO_SERVER", "===== ara::com Demo Server Starting =====");

    // ---- 解析配置 ----
    std::string config_path = (argc > 1) ? argv[1] : "../config/services.json";
    config::JsonConfigParser parser;
    if (!parser.load_file(config_path)) {
        ARA_LOG_FATAL("DEMO_SERVER", "Failed to load config: " + config_path);
        return 1;
    }

    // 查找 service_id 0x1234 的服务端配置
    auto* svc_cfg = parser.find_service(0x1234);
    if (!svc_cfg) {
        ARA_LOG_FATAL("DEMO_SERVER", "Server config not found for service 0x1234");
        return 1;
    }

    // ---- 创建通信后端 ----
#ifdef ARA_COM_USE_VSOMEIP
    auto backend = runtime::create_backend(runtime::BackendType::VSOMEIP);
#else
    auto backend = runtime::create_backend(runtime::BackendType::NULL_BACKEND);
#endif
    if (!backend->init(*svc_cfg)) {
        ARA_LOG_FATAL("DEMO_SERVER", "Backend init failed");
        return 1;
    }
    backend->start();

    // ---- 创建 ServiceSkeleton ----
    ServiceSkeleton skeleton(backend, *svc_cfg, parser.e2e_config());
    skeleton.init();

    // SOME/IP Event ID 最高位为1，Method ID 最高位为0
    skeleton.register_event<demo::VehicleSpeed>(0x8001);

    // 注册传感器 RPC Method（method_id = 0x0101）
    skeleton.register_method<demo::SensorRequest, demo::SensorResponse>(
        0x0101,
        [](const demo::SensorRequest& req) -> demo::SensorResponse {
            ARA_LOG_INFO("DEMO_SERVER", "Received sensor request: id=" +
                         std::to_string(req.sensor_id) + " cmd=" +
                         std::to_string(req.command));

            demo::SensorResponse rsp;
            rsp.sensor_id = req.sensor_id;
            rsp.status = 0;  // OK

            switch (req.command) {
                case 0:  // read
                    rsp.value = 42;  // Simulated sensor value
                    rsp.message = "OK: sensor read successful";
                    break;
                case 1:  // calibrate
                    rsp.value = 0;
                    rsp.message = "OK: calibration complete";
                    break;
                case 2:  // reset
                    rsp.value = 0;
                    rsp.message = "OK: sensor reset";
                    break;
                default:
                    rsp.status = 1;
                    rsp.message = "ERROR: unknown command";
                    break;
            }
            return rsp;
        });

    // 按配置启用 E2E 与分片
    skeleton.set_e2e_enabled(parser.e2e_config().enabled);
    skeleton.set_fragmentation_enabled(parser.fragmentation_enabled());

    if (parser.fragmentation_enabled()) {
        skeleton.set_mtu(parser.fragmentation_mtu());
    }

    // 启动服务
    if (!skeleton.start()) {
        ARA_LOG_FATAL("DEMO_SERVER", "Failed to start service");
        return 1;
    }

    // 启用心跳
    if (parser.heartbeat_enabled()) {
        skeleton.enable_heartbeat(parser.heartbeat_interval_ms());
    }

    ARA_LOG_INFO("DEMO_SERVER", "Service running. Publishing speed events...");

    // ---- 主循环：周期发布 Event ----
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    std::uint32_t tick = 0;
    while (g_running.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        // 模拟0至120 km/h变化的车速
        demo::VehicleSpeed speed;
        speed.timestamp_ms = tick * 100;
        speed.speed_kmh = 60.0f + 40.0f * std::sin(static_cast<float>(tick) * 0.1f);
        speed.confidence = 95;

        skeleton.publish_event(0x8001, speed);

        if (tick % 50 == 0) {  // Log every 5 seconds
            ARA_LOG_INFO("DEMO_SERVER", "Published event #" + std::to_string(tick) +
                         " speed=" + std::to_string(speed.speed_kmh) + " km/h");
        }
        ++tick;
    }

    // ---- 平稳停止 ----
    ARA_LOG_INFO("DEMO_SERVER", "Shutting down...");
    skeleton.stop();
    backend->stop();
    logger::AsyncLogger::instance().shutdown();

    std::cout << "Demo server exited cleanly." << std::endl;
    return 0;
}
