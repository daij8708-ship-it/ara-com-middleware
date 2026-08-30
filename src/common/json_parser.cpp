/**
 * @file    json_parser.cpp
 * @brief   JSON 配置解析器。
 *
 * 使用递归下降方式解析 JSON。
 * 用于解析扁平服务配置，不依赖第三方 JSON 库。
 *
 * 支持对象、数组、字符串、数字、布尔值和 null。
 * 暂不支持 Unicode 转义和整数科学计数法。
 */

#include "json_parser.h"

#include "logger.h"

#include <cctype>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>

namespace ara {
namespace com {
namespace config {

// =========================================================================
//  最小 JSON 值表示（内部使用）
// =========================================================================

enum class JsonType { OBJECT, ARRAY, STRING, NUMBER, BOOLEAN, NIL };

struct JsonValue {
    JsonType type = JsonType::NIL;

    // std::string 析构非平凡，使用 union 会引入额外的手工生命周期管理
    std::string                              string_val;
    double                                   number_val = 0.0;
    bool                                     bool_val = false;
    std::vector<std::pair<std::string, JsonValue>> object_members;
    std::vector<JsonValue>                   array_elements;
};

// =========================================================================
//  递归下降 JSON 解析器
// =========================================================================

class JsonTokenizer {
public:
    explicit JsonTokenizer(const std::string& input)
        : m_input(input), m_pos(0) {}

    char peek() const {
        skip_whitespace();
        return m_pos < m_input.size() ? m_input[m_pos] : '\0';
    }

    char advance() {
        skip_whitespace();
        return m_pos < m_input.size() ? m_input[m_pos++] : '\0';
    }

    void expect(char c) {
        char got = advance();
        if (got != c) {
            throw ComException(std::string("JSON parse error: expected '") + c +
                               "' but got '" + got + "'");
        }
    }

    std::size_t pos() const { return m_pos; }

private:
    void skip_whitespace() const {
        while (m_pos < m_input.size() &&
               (m_input[m_pos] == ' ' || m_input[m_pos] == '\t' ||
                m_input[m_pos] == '\n' || m_input[m_pos] == '\r')) {
            ++m_pos;
        }
    }

    const std::string& m_input;
    mutable std::size_t m_pos;
};

// 前置声明
static JsonValue parse_value(JsonTokenizer& tok);

static std::string parse_string(JsonTokenizer& tok) {
    tok.expect('"');
    std::string result;
    while (true) {
        if (tok.pos() >= 4096) {  // 限制字符串解析位置，避免持续扫描
            throw ComException("String too long or unterminated");
        }
        char c = tok.advance();
        if (c == '"') {
            return result;
        }
        if (c == '\\') {
            char esc = tok.advance();
            switch (esc) {
                case '"':  result += '"';  break;
                case '\\': result += '\\'; break;
                case '/':  result += '/';  break;
                case 'b':  result += '\b'; break;
                case 'f':  result += '\f'; break;
                case 'n':  result += '\n'; break;
                case 'r':  result += '\r'; break;
                case 't':  result += '\t'; break;
                default:
                    result += '\\';
                    result += esc;
                    break;
            }
        } else {
            result += c;
        }
    }
}

static double parse_number(JsonTokenizer& tok) {
    std::string num_str;
    char c = tok.peek();
    bool is_float = false;

    if (c == '-') {
        num_str += tok.advance();
    }

    while (std::isdigit(tok.peek())) {
        num_str += tok.advance();
    }

    if (tok.peek() == '.') {
        is_float = true;
        num_str += tok.advance();
        while (std::isdigit(tok.peek())) {
            num_str += tok.advance();
        }
    }

    if (tok.peek() == 'e' || tok.peek() == 'E') {
        is_float = true;
        num_str += tok.advance();
        if (tok.peek() == '+' || tok.peek() == '-') {
            num_str += tok.advance();
        }
        while (std::isdigit(tok.peek())) {
            num_str += tok.advance();
        }
    }

    return std::strtod(num_str.c_str(), nullptr);
}

static JsonValue parse_object(JsonTokenizer& tok) {
    JsonValue val;
    val.type = JsonType::OBJECT;
    tok.expect('{');

    if (tok.peek() == '}') {
        tok.advance();
        return val;
    }

    while (true) {
        std::string key = parse_string(tok);
        tok.expect(':');
        JsonValue member = parse_value(tok);
        val.object_members.push_back({key, std::move(member)});

        if (tok.peek() == '}') {
            tok.advance();
            return val;
        }
        tok.expect(',');
    }
}

static JsonValue parse_array(JsonTokenizer& tok) {
    JsonValue val;
    val.type = JsonType::ARRAY;
    tok.expect('[');

    if (tok.peek() == ']') {
        tok.advance();
        return val;
    }

    while (true) {
        val.array_elements.push_back(parse_value(tok));
        if (tok.peek() == ']') {
            tok.advance();
            return val;
        }
        tok.expect(',');
    }
}

static JsonValue parse_value(JsonTokenizer& tok) {
    char c = tok.peek();

    if (c == '"') {
        JsonValue val;
        val.type = JsonType::STRING;
        val.string_val = parse_string(tok);
        return val;
    }

    if (c == '{') {
        return parse_object(tok);
    }

    if (c == '[') {
        return parse_array(tok);
    }

    if (c == '-' || std::isdigit(c)) {
        JsonValue val;
        val.type = JsonType::NUMBER;
        val.number_val = parse_number(tok);
        return val;
    }

    // true / false / null
    std::string ident;
    while (std::isalpha(tok.peek())) {
        ident += tok.advance();
    }

    JsonValue val;
    if (ident == "true") {
        val.type = JsonType::BOOLEAN;
        val.bool_val = true;
    } else if (ident == "false") {
        val.type = JsonType::BOOLEAN;
        val.bool_val = false;
    } else if (ident == "null") {
        val.type = JsonType::NIL;
    } else {
        throw ComException("Unexpected token: " + ident);
    }
    return val;
}

// =========================================================================
//  按键查找 JSON 对象成员
// =========================================================================

static const JsonValue* find_member(const JsonValue& obj, const std::string& key) {
    if (obj.type != JsonType::OBJECT) return nullptr;
    for (const auto& [k, v] : obj.object_members) {
        if (k == key) return &v;
    }
    return nullptr;
}

static const JsonValue* find_member(const std::vector<JsonValue>& arr, std::size_t idx) {
    if (idx < arr.size()) return &arr[idx];
    return nullptr;
}

// =========================================================================
//  JsonValue 安全取值辅助函数
// =========================================================================

static std::string json_to_string(const JsonValue* v, const std::string& def = "") {
    if (!v || v->type != JsonType::STRING) return def;
    return v->string_val;
}

static double json_to_number(const JsonValue* v, double def = 0.0) {
    if (!v || v->type != JsonType::NUMBER) return def;
    return v->number_val;
}

static bool json_to_bool(const JsonValue* v, bool def = false) {
    if (!v || v->type != JsonType::BOOLEAN) return def;
    return v->bool_val;
}

// =========================================================================
//  JsonConfigParser 实现
// =========================================================================

static ServiceConfig parse_service_entry(const JsonValue& entry) {
    ServiceConfig cfg;
    cfg.service_id   = static_cast<ServiceId>(json_to_number(find_member(entry, "service_id")));
    cfg.instance_id  = static_cast<InstanceId>(json_to_number(find_member(entry, "instance_id"), 1.0));
    cfg.major_version = static_cast<MajorVersion>(json_to_number(find_member(entry, "major_version"), 1.0));
    cfg.minor_version = static_cast<MinorVersion>(json_to_number(find_member(entry, "minor_version"), 0.0));
    cfg.ip_address   = json_to_string(find_member(entry, "ip_address"), "127.0.0.1");
    cfg.port         = static_cast<std::uint16_t>(json_to_number(find_member(entry, "port"), 30500.0));
    cfg.protocol     = json_to_string(find_member(entry, "protocol"), "UDP");
    cfg.role         = json_to_string(find_member(entry, "role"), "server");
    return cfg;
}

bool JsonConfigParser::load_string(const std::string& json_string) {
    try {
        JsonTokenizer tok(json_string);
        JsonValue root = parse_value(tok);

        // 解析 services 数组
        const JsonValue* services_node = find_member(root, "services");
        if (services_node && services_node->type == JsonType::ARRAY) {
            m_services.clear();
            for (const auto& svc_entry : services_node->array_elements) {
                m_services.push_back(parse_service_entry(svc_entry));
            }
            ARA_LOG_INFO("JSON", "Parsed " + std::to_string(m_services.size()) + " service config(s)");
        }

        // 解析 e2e 配置
        const JsonValue* e2e_node = find_member(root, "e2e");
        if (e2e_node) {
            m_e2e_config.enabled     = json_to_bool(find_member(*e2e_node, "enabled"), true);
            m_e2e_config.max_counter = static_cast<std::uint16_t>(
                json_to_number(find_member(*e2e_node, "max_counter"), 15.0));
        }

        // 解析 fragmentation 配置
        const JsonValue* frag_node = find_member(root, "fragmentation");
        if (frag_node) {
            m_fragmentation_enabled = json_to_bool(find_member(*frag_node, "enabled"), true);
            m_fragmentation_mtu = static_cast<std::uint16_t>(
                json_to_number(find_member(*frag_node, "mtu"), 1400.0));
        }

        // 解析 heartbeat 配置
        const JsonValue* hb_node = find_member(root, "heartbeat");
        if (hb_node) {
            m_heartbeat_enabled    = json_to_bool(find_member(*hb_node, "enabled"), true);
            m_heartbeat_interval_ms = static_cast<std::uint32_t>(
                json_to_number(find_member(*hb_node, "interval_ms"), 1000.0));
            m_heartbeat_timeout_ms = static_cast<std::uint32_t>(
                json_to_number(find_member(*hb_node, "timeout_ms"), 3000.0));
        }

        // 解析 reconnect 配置
        const JsonValue* recon_node = find_member(root, "reconnect");
        if (recon_node) {
            m_reconnect_enabled          = json_to_bool(find_member(*recon_node, "enabled"), true);
            m_reconnect_initial_delay_ms = static_cast<std::uint32_t>(
                json_to_number(find_member(*recon_node, "initial_delay_ms"), 100.0));
            m_reconnect_max_delay_ms = static_cast<std::uint32_t>(
                json_to_number(find_member(*recon_node, "max_delay_ms"), 5000.0));
            m_reconnect_backoff_multiplier = static_cast<std::uint32_t>(
                json_to_number(find_member(*recon_node, "backoff_multiplier"), 2.0));
        }

        return true;

    } catch (const ComException& e) {
        ARA_LOG_ERROR("JSON", std::string("Parse error: ") + e.what());
        return false;
    } catch (const std::exception& e) {
        ARA_LOG_ERROR("JSON", std::string("Parse error: ") + e.what());
        return false;
    }
}

bool JsonConfigParser::load_file(const std::string& file_path) {
    std::ifstream ifs(file_path);
    if (!ifs.is_open()) {
        ARA_LOG_ERROR("JSON", "Cannot open file: " + file_path);
        return false;
    }

    std::ostringstream oss;
    oss << ifs.rdbuf();
    return load_string(oss.str());
}

const ServiceConfig* JsonConfigParser::find_service(ServiceId service_id) const {
    for (const auto& svc : m_services) {
        if (svc.service_id == service_id) {
            return &svc;
        }
    }
    return nullptr;
}

std::vector<const ServiceConfig*> JsonConfigParser::services_by_role(const std::string& role) const {
    std::vector<const ServiceConfig*> result;
    for (const auto& svc : m_services) {
        if (svc.role == role) {
            result.push_back(&svc);
        }
    }
    return result;
}

} // namespace config
} // namespace com
} // namespace ara
