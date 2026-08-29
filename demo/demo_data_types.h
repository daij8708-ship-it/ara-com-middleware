/**
 * @file    demo_data_types.h
 * @brief   示例应用数据类型。
 *
 * 演示用户自定义类型如何满足 serialize()/deserialize() 序列化约定。
 *
 */
#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace demo {

// =========================================================================
//  VehicleSpeed：Event 示例结构
// =========================================================================

struct VehicleSpeed {
    std::uint32_t timestamp_ms{0};
    float         speed_kmh{0.0f};
    std::uint8_t  confidence{0};  ///< 0-100

    /// 序列化为字节（简单二进制打包）
    std::vector<std::uint8_t> serialize() const {
        std::vector<std::uint8_t> buf(9);  // 4 + 4 + 1
        buf[0] = (timestamp_ms >> 24) & 0xFF;
        buf[1] = (timestamp_ms >> 16) & 0xFF;
        buf[2] = (timestamp_ms >> 8) & 0xFF;
        buf[3] = timestamp_ms & 0xFF;

        std::uint32_t speed_bits;
        std::memcpy(&speed_bits, &speed_kmh, 4);
        buf[4] = (speed_bits >> 24) & 0xFF;
        buf[5] = (speed_bits >> 16) & 0xFF;
        buf[6] = (speed_bits >> 8) & 0xFF;
        buf[7] = speed_bits & 0xFF;

        buf[8] = confidence;
        return buf;
    }

    /// 从字节反序列化
    static VehicleSpeed deserialize(const std::vector<std::uint8_t>& data) {
        VehicleSpeed vs{};
        if (data.size() >= 9) {
            vs.timestamp_ms = (static_cast<std::uint32_t>(data[0]) << 24) |
                              (static_cast<std::uint32_t>(data[1]) << 16) |
                              (static_cast<std::uint32_t>(data[2]) << 8)  |
                               static_cast<std::uint32_t>(data[3]);
            std::uint32_t speed_bits = (static_cast<std::uint32_t>(data[4]) << 24) |
                                       (static_cast<std::uint32_t>(data[5]) << 16) |
                                       (static_cast<std::uint32_t>(data[6]) << 8)  |
                                        static_cast<std::uint32_t>(data[7]);
            std::memcpy(&vs.speed_kmh, &speed_bits, 4);
            vs.confidence = data[8];
        }
        return vs;
    }
};

// =========================================================================
//  SensorRequest / SensorResponse：RPC Method 示例
// =========================================================================

struct SensorRequest {
    std::uint8_t sensor_id{0};
    std::uint8_t command{0};  ///< 0=read, 1=calibrate, 2=reset

    std::vector<std::uint8_t> serialize() const {
        return {sensor_id, command};
    }

    static SensorRequest deserialize(const std::vector<std::uint8_t>& data) {
        SensorRequest req{};
        if (data.size() >= 2) {
            req.sensor_id = data[0];
            req.command   = data[1];
        }
        return req;
    }
};

struct SensorResponse {
    std::uint8_t  sensor_id{0};
    std::uint8_t  status{0};     ///< 0=OK, 1=ERROR
    std::int32_t  value{0};
    std::string   message;

    std::vector<std::uint8_t> serialize() const {
        std::vector<std::uint8_t> buf;
        buf.push_back(sensor_id);
        buf.push_back(status);
        buf.push_back((value >> 24) & 0xFF);
        buf.push_back((value >> 16) & 0xFF);
        buf.push_back((value >> 8) & 0xFF);
        buf.push_back(value & 0xFF);
        // 简单字符串编码：长度前缀加 ASCII 内容
        buf.push_back(static_cast<std::uint8_t>(message.size()));
        for (char c : message) buf.push_back(static_cast<std::uint8_t>(c));
        return buf;
    }

    static SensorResponse deserialize(const std::vector<std::uint8_t>& data) {
        SensorResponse rsp{};
        if (data.size() >= 7) {
            rsp.sensor_id = data[0];
            rsp.status    = data[1];
            rsp.value     = (static_cast<std::int32_t>(data[2]) << 24) |
                            (static_cast<std::int32_t>(data[3]) << 16) |
                            (static_cast<std::int32_t>(data[4]) << 8)  |
                             static_cast<std::int32_t>(data[5]);
            std::uint8_t msg_len = data[6];
            if (data.size() >= 7 + msg_len) {
                rsp.message.assign(reinterpret_cast<const char*>(&data[7]), msg_len);
            }
        }
        return rsp;
    }
};

} // namespace demo
