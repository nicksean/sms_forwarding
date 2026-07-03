#include "idf_web.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include <string>
#include <stdlib.h>
#include <new>

#include "driver/temperature_sensor.h"
#include "esp_ota_ops.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "idf_config.h"
#include "idf_inbox.h"
#include "idf_log.h"
#include "idf_modem.h"
#include "idf_push.h"
#include "idf_sms.h"
#include "idf_wifi.h"
#include "mbedtls/base64.h"
#include "web_assets.h"

static const char* TAG = "idf_web";
static httpd_handle_t s_server = nullptr;
static SemaphoreHandle_t s_cell_job_mutex = nullptr;
static bool s_scheduler_started = false;

struct WebAsyncJob {
    bool running = false;
    bool done = false;
    bool success = false;
    bool queued = false;
    std::string url;
    std::string message;
};

static WebAsyncJob s_ping_job;
static WebAsyncJob s_keepalive_job;

static void json_escape_append(std::string& out, const std::string& value)
{
    for (char ch : value) {
        switch (ch) {
            case '\\': out += "\\\\"; break;
            case '"': out += "\\\""; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(ch) < 0x20) {
                    char buf[7];
                    snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned char>(ch));
                    out += buf;
                } else {
                    out += ch;
                }
        }
    }
}

static void json_prop(std::string& out, const char* key, const std::string& value)
{
    out += "\"";
    out += key;
    out += "\":\"";
    json_escape_append(out, value);
    out += "\"";
}

static bool epoch_valid(uint32_t epoch)
{
    return epoch >= 1700000000u;
}

static std::string format_tz_offset(int tz_offset_min)
{
    if (tz_offset_min == 0) return "UTC";
    char buf[16];
    int total = tz_offset_min < 0 ? -tz_offset_min : tz_offset_min;
    int hh = total / 60;
    int mm = total % 60;
    if (mm == 0) {
        snprintf(buf, sizeof(buf), "UTC%c%d", tz_offset_min < 0 ? '-' : '+', hh);
    } else {
        snprintf(buf, sizeof(buf), "UTC%c%d:%02d", tz_offset_min < 0 ? '-' : '+', hh, mm);
    }
    return std::string(buf);
}

static std::string format_epoch_local(uint32_t epoch, int tz_offset_min)
{
    if (!epoch_valid(epoch)) return std::string();
    time_t shifted = static_cast<time_t>(static_cast<int64_t>(epoch) + static_cast<int64_t>(tz_offset_min) * 60LL);
    struct tm tmv = {};
    gmtime_r(&shifted, &tmv);
    char buf[48];
    snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d %s",
             tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
             tmv.tm_hour, tmv.tm_min, tmv.tm_sec,
             format_tz_offset(tz_offset_min).c_str());
    return std::string(buf);
}

// 芯片内部温度(概览页显示)；驱动懒加载，失败只记一次不再重试
static bool read_chip_temp(float& out)
{
    static temperature_sensor_handle_t s_tsens = nullptr;
    static bool s_tsens_failed = false;
    if (!s_tsens && !s_tsens_failed) {
        temperature_sensor_config_t cfg = TEMPERATURE_SENSOR_CONFIG_DEFAULT(-10, 80);
        if (temperature_sensor_install(&cfg, &s_tsens) != ESP_OK ||
            temperature_sensor_enable(s_tsens) != ESP_OK) {
            s_tsens_failed = true;
            return false;
        }
    }
    return s_tsens && temperature_sensor_get_celsius(s_tsens, &out) == ESP_OK;
}

static bool auth_matches_config(const char* auth)
{
    static constexpr const char* prefix = "Basic ";
    if (strncmp(auth, prefix, strlen(prefix)) != 0) return false;

    unsigned char decoded[384] = {};
    size_t decoded_len = 0;
    const unsigned char* encoded = reinterpret_cast<const unsigned char*>(auth + strlen(prefix));
    int rc = mbedtls_base64_decode(decoded, sizeof(decoded) - 1, &decoded_len, encoded, strlen(auth + strlen(prefix)));
    if (rc != 0) return false;
    decoded[decoded_len] = '\0';

    // 用窄访问器锁内比对：每个请求(含 2s 一次的 /status 轮询)做全量配置深拷贝
    // 是持续的堆抖动源
    char* colon = strchr(reinterpret_cast<char*>(decoded), ':');
    if (!colon) return false;
    *colon = '\0';
    return idf_config_check_web_auth(reinterpret_cast<char*>(decoded), colon + 1);
}

static bool check_auth(httpd_req_t* req)
{
    char auth[512] = {};
    if (httpd_req_get_hdr_value_str(req, "Authorization", auth, sizeof(auth)) == ESP_OK &&
        auth_matches_config(auth)) {
        return true;
    }

    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"SMS Forwarding\"");
    httpd_resp_sendstr(req, "Unauthorized");
    return false;
}

static bool etag_matches(httpd_req_t* req, const WebAsset& asset)
{
    char inm[96] = {};
    if (httpd_req_get_hdr_value_str(req, "If-None-Match", inm, sizeof(inm)) != ESP_OK) {
        return false;
    }
    return strstr(inm, asset.etag) != nullptr;
}

static esp_err_t send_gzip_asset(httpd_req_t* req, const WebAsset& asset, const char* cache_control)
{
    httpd_resp_set_hdr(req, "Cache-Control", cache_control);
    httpd_resp_set_hdr(req, "ETag", asset.etag);
    httpd_resp_set_hdr(req, "Vary", "Authorization, Accept-Encoding");

    if (etag_matches(req, asset)) {
        httpd_resp_set_status(req, "304 Not Modified");
        return httpd_resp_send(req, nullptr, 0);
    }

    httpd_resp_set_type(req, asset.mime);
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    return httpd_resp_send(req, reinterpret_cast<const char*>(asset.data), asset.length);
}

static esp_err_t handle_root(httpd_req_t* req)
{
    if (!check_auth(req)) return ESP_OK;
    return send_gzip_asset(req, WEB_INDEX, "private, no-cache, must-revalidate");
}

static esp_err_t handle_asset(httpd_req_t* req)
{
    if (!check_auth(req)) return ESP_OK;
    if (strstr(req->uri, "/app.css")) {
        return send_gzip_asset(req, WEB_APP_CSS, "private, max-age=31536000, immutable");
    }
    if (strstr(req->uri, "/app.js")) {
        return send_gzip_asset(req, WEB_APP_JS, "private, max-age=31536000, immutable");
    }
    httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Not Found");
    return ESP_OK;
}

static esp_err_t handle_ui_panel(httpd_req_t* req)
{
    if (!check_auth(req)) return ESP_OK;

    char query[96] = {};
    char panel[32] = {};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, "panel", panel, sizeof(panel)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing panel");
        return ESP_OK;
    }

    const WebAsset* asset = findWebPanelAsset(panel);
    if (!asset) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Unknown panel");
        return ESP_OK;
    }
    return send_gzip_asset(req, *asset, "private, max-age=31536000, immutable");
}

static esp_err_t handle_status(httpd_req_t* req)
{
    if (!check_auth(req)) return ESP_OK;
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store, max-age=0");
    httpd_resp_set_hdr(req, "Pragma", "no-cache");

    const IdfConfig& cfg = idf_config_get();
    IdfWifiStatus wifi = idf_wifi_get_status();
    IdfModemStatus modem = idf_modem_get_status();
    IdfSmsStatus sms = idf_sms_get_status();
    time_t now = time(nullptr);
    uint64_t uptime = esp_timer_get_time() / 1000000ULL;

    std::string body;
    body.reserve(2300);
    char buf[256];
    snprintf(buf, sizeof(buf),
             "{\"version\":\"%s\",\"idfPort\":true,\"modemReady\":%s,"
             "\"modemInitPhase\":\"%s\",\"ceregStat\":%d,"
             "\"signalFresh\":%s,\"identityFresh\":%s,"
             "\"webAssetHash\":\"%s\",",
             IDF_FW_VERSION,
             modem.modemReady ? "true" : "false",
             modem.phase.c_str(), modem.ceregStat,
             modem.signalFresh ? "true" : "false",
             modem.identityFresh ? "true" : "false",
             WEB_ASSET_HASH);
    body += buf;
    snprintf(buf, sizeof(buf), "\"tz\":%d,", cfg.tzOffsetMin);
    body += buf;
    json_prop(body, "tzName", format_tz_offset(cfg.tzOffsetMin)); body += ",";
    snprintf(buf, sizeof(buf), "\"nowEpoch\":%ld,", static_cast<long>(now));
    body += buf;
    json_prop(body, "nowLocal", format_epoch_local(static_cast<uint32_t>(now), cfg.tzOffsetMin)); body += ",";
    snprintf(buf, sizeof(buf),
             "\"uptime\":%llu,\"resetReason\":%d,"
             "\"freeHeap\":%u,\"minFreeHeap\":%u,\"maxAllocHeap\":%u,",
             static_cast<unsigned long long>(uptime), static_cast<int>(esp_reset_reason()),
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_8BIT)),
             static_cast<unsigned>(esp_get_minimum_free_heap_size()),
             static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)));
    body += buf;
    snprintf(buf, sizeof(buf),
             "\"smsTotal\":%u,\"lastSmsEpoch\":%u,",
             static_cast<unsigned>(sms.total),
             static_cast<unsigned>(sms.lastSmsEpoch));
    body += buf;
    json_prop(body, "lastSmsLocal", format_epoch_local(sms.lastSmsEpoch, cfg.tzOffsetMin)); body += ",";
    snprintf(buf, sizeof(buf),
             "\"inboxCount\":%u,"
             "\"fwdQueueDepth\":%d,\"queueDepth\":%d,\"outSmsQueueDepth\":%d,\"emailQueueDepth\":%d,"
             "\"slowBusy\":%s,\"timeSynced\":%s,",
             static_cast<unsigned>(idf_inbox_count()),
             idf_push_forward_queue_depth(),
             idf_push_retry_queue_depth(),
             idf_sms_outgoing_queue_depth(),
             idf_push_email_queue_depth(),
             idf_push_busy() ? "true" : "false",
             now > 100000 ? "true" : "false");
    body += buf;
    if (modem.csq >= 0) {
        snprintf(buf, sizeof(buf), "\"csq\":%d,\"ber\":%d,", modem.csq, modem.ber);
        body += buf;
    } else {
        body += "\"csq\":null,\"ber\":99,";
    }
    if (modem.rsrp != 999) {
        snprintf(buf, sizeof(buf), "\"rsrp\":%d,", modem.rsrp);
        body += buf;
    } else {
        body += "\"rsrp\":null,";
    }
    if (modem.rsrq != 999) {
        snprintf(buf, sizeof(buf), "\"rsrq\":%d,", modem.rsrq);
        body += buf;
    } else {
        body += "\"rsrq\":null,";
    }
    if (modem.sinr != 999) {
        snprintf(buf, sizeof(buf), "\"sinr\":%d,", modem.sinr);
        body += buf;
    } else {
        body += "\"sinr\":null,";
    }

    snprintf(buf, sizeof(buf),
             "\"dataEnabled\":%s,\"emailEnabled\":%s,\"emailConfigured\":%s,"
             "\"pushEnabled\":%s,\"pushEnabledCount\":%d,\"apMode\":%s,",
             cfg.dataEnabled ? "true" : "false",
             cfg.emailEnabled ? "true" : "false",
             idf_config_email_configured() ? "true" : "false",
             cfg.pushEnabled ? "true" : "false",
             idf_config_enabled_push_count(),
             wifi.apMode ? "true" : "false");
    body += buf;

    json_prop(body, "ssid", wifi.staConnected ? wifi.ssid : wifi.apSsid); body += ",";
    json_prop(body, "ip", wifi.staConnected ? wifi.ip : wifi.apIp); body += ",";
    json_prop(body, "gw", wifi.gw); body += ",";
    json_prop(body, "mask", wifi.mask); body += ",";
    json_prop(body, "dns", wifi.dns); body += ",";
    json_prop(body, "mac", wifi.mac); body += ",";
    json_prop(body, "bssid", wifi.bssid); body += ",";
    if (wifi.staConnected) {
        snprintf(buf, sizeof(buf), "\"rssi\":%d,\"chan\":%d,", wifi.rssi, wifi.channel);
        body += buf;
    } else {
        body += "\"rssi\":null,\"chan\":0,";
    }

    json_prop(body, "adminPhone", cfg.adminPhone); body += ",";
    json_prop(body, "phone", modem.phone.empty() ? cfg.phoneNumber : modem.phone); body += ",";
    json_prop(body, "apn", cfg.apn); body += ",";
    json_prop(body, "apnSim", modem.apnSim); body += ",";
    json_prop(body, "cellIp", modem.cellIp); body += ",";
    json_prop(body, "operator", modem.operatorName); body += ",";
    json_prop(body, "mfr", modem.mfr); body += ",";
    json_prop(body, "model", modem.model); body += ",";
    json_prop(body, "fwver", modem.fwver); body += ",";
    json_prop(body, "imei", modem.imei); body += ",";
    json_prop(body, "iccid", modem.iccid); body += ",";
    json_prop(body, "imsi", modem.imsi);
    float temp = 0;
    if (read_chip_temp(temp)) {
        snprintf(buf, sizeof(buf), ",\"chipTemp\":%.1f}", static_cast<double>(temp));
        body += buf;
    } else {
        body += ",\"chipTemp\":null}";
    }

    return httpd_resp_send(req, body.c_str(), body.size());
}

static esp_err_t send_config_json(httpd_req_t* req)
{
    if (!check_auth(req)) return ESP_OK;
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store, max-age=0");
    httpd_resp_set_hdr(req, "Pragma", "no-cache");

    const IdfConfig& cfg = idf_config_get();
    std::string body;
    body.reserve(4096);
    char buf[256];
    body += "{";
    json_prop(body, "webUser", cfg.webUser); body += ",";
    json_prop(body, "webPass", cfg.webPass); body += ",";
    json_prop(body, "smtpServer", cfg.smtpServer); body += ",";
    snprintf(buf, sizeof(buf), "\"smtpPort\":%d,", cfg.smtpPort); body += buf;
    json_prop(body, "smtpUser", cfg.smtpUser); body += ",";
    json_prop(body, "smtpPass", cfg.smtpPass); body += ",";
    json_prop(body, "smtpSendTo", cfg.smtpSendTo); body += ",";
    json_prop(body, "adminPhone", cfg.adminPhone); body += ",";
    json_prop(body, "numberBlackList", cfg.numberBlackList); body += ",";
    json_prop(body, "forwardRules", cfg.forwardRules); body += ",";
    snprintf(buf, sizeof(buf),
             "\"emailEnabled\":%s,\"emailConfigured\":%s,\"pushEnabled\":%s,"
             "\"pushEnabledCount\":%d,\"modemReady\":%s,\"inboxMax\":50,",
             cfg.emailEnabled ? "true" : "false",
             idf_config_email_configured() ? "true" : "false",
             cfg.pushEnabled ? "true" : "false",
             idf_config_enabled_push_count(),
             idf_modem_get_status().modemReady ? "true" : "false");
    body += buf;
    json_prop(body, "ntpServer", cfg.ntpServer); body += ",";
    snprintf(buf, sizeof(buf),
             "\"tzOffsetMin\":%d,\"rebootEnabled\":%s,\"rebootHour\":%d,"
             "\"hbEnabled\":%s,\"hbHour\":%d,\"dataEnabled\":%s,",
             cfg.tzOffsetMin,
             cfg.rebootEnabled ? "true" : "false", cfg.rebootHour,
             cfg.hbEnabled ? "true" : "false", cfg.hbHour,
             cfg.dataEnabled ? "true" : "false");
    body += buf;
    json_prop(body, "apn", cfg.apn); body += ",";
    json_prop(body, "phoneNumber", cfg.phoneNumber); body += ",";
    json_prop(body, "operatorPlmn", cfg.operatorPlmn); body += ",";
    body += "\"pushChannels\":[";
    for (int i = 0; i < IDF_MAX_PUSH_CHANNELS; ++i) {
        if (i) body += ",";
        const IdfPushChannel& ch = cfg.pushChannels[i];
        snprintf(buf, sizeof(buf), "{\"enabled\":%s,\"type\":%u,",
                 ch.enabled ? "true" : "false", static_cast<unsigned>(ch.type));
        body += buf;
        json_prop(body, "name", ch.name); body += ",";
        json_prop(body, "url", ch.url); body += ",";
        json_prop(body, "key1", ch.key1); body += ",";
        json_prop(body, "key2", ch.key2); body += ",";
        json_prop(body, "customBody", ch.customBody);
        body += "}";
    }
    {
        uint64_t up_s = esp_timer_get_time() / 1000000ULL;
        unsigned days = static_cast<unsigned>(up_s / 86400ULL);
        unsigned hours = static_cast<unsigned>((up_s / 3600ULL) % 24ULL);
        unsigned mins = static_cast<unsigned>((up_s / 60ULL) % 60ULL);
        char up_buf[64];
        if (days > 0) snprintf(up_buf, sizeof(up_buf), "%u天%u小时%u分", days, hours, mins);
        else if (hours > 0) snprintf(up_buf, sizeof(up_buf), "%u小时%u分", hours, mins);
        else snprintf(up_buf, sizeof(up_buf), "%u分钟", mins);
        body += "],\"uptimeText\":\"";
        body += up_buf;
        body += "\"}";
    }
    return httpd_resp_send(req, body.c_str(), body.size());
}

static int hex_value(char ch)
{
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

static std::string url_decode(const char* data, size_t len)
{
    std::string out;
    out.reserve(len);
    for (size_t i = 0; i < len; ++i) {
        char ch = data[i];
        if (ch == '+') {
            out += ' ';
        } else if (ch == '%' && i + 2 < len) {
            int hi = hex_value(data[i + 1]);
            int lo = hex_value(data[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out += static_cast<char>((hi << 4) | lo);
                i += 2;
            } else {
                out += ch;
            }
        } else {
            out += ch;
        }
    }
    return out;
}

static esp_err_t read_body(httpd_req_t* req, std::string& body, size_t max_len = 8192)
{
    if (req->content_len > max_len) {
        httpd_resp_send_err(req, HTTPD_413_CONTENT_TOO_LARGE, "Body too large");
        return ESP_FAIL;
    }
    body.assign(req->content_len, '\0');
    size_t received = 0;
    int timeouts = 0;
    while (received < body.size()) {
        int ret = httpd_req_recv(req, body.data() + received, body.size() - received);
        if (ret <= 0) {
            // 超时最多容忍 3 次(~15s)：httpd 单任务串行处理请求，
            // 一个挂着不发数据的客户端会把整个 Web UI 卡死到重启
            if (ret == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts <= 3) continue;
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Receive failed");
            return ESP_FAIL;
        }
        timeouts = 0;
        received += static_cast<size_t>(ret);
    }
    return ESP_OK;
}

// AT 通道被长任务(保号 MHTTP 最长约 4 分钟)占用时，Web 的 AT 类路由立即返回"正忙"，
// 而不是把唯一的 httpd 工作线程压在互斥锁上等待——那会让所有页面一起失去响应
static bool modem_busy_reply(httpd_req_t* req)
{
    if (idf_modem_at_idle()) return false;
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"success\":false,\"message\":\"模组串口正忙(可能正在执行保号/诊断任务)，请稍后重试\"}");
    return true;
}

static IdfFormFields parse_urlencoded(const std::string& body)
{
    IdfFormFields fields;
    size_t pos = 0;
    while (pos <= body.size()) {
        size_t amp = body.find('&', pos);
        if (amp == std::string::npos) amp = body.size();
        size_t eq = body.find('=', pos);
        if (eq == std::string::npos || eq > amp) eq = amp;
        std::string key = url_decode(body.data() + pos, eq - pos);
        std::string value;
        if (eq < amp) value = url_decode(body.data() + eq + 1, amp - eq - 1);
        if (!key.empty()) fields.emplace_back(std::move(key), std::move(value));
        if (amp == body.size()) break;
        pos = amp + 1;
    }
    return fields;
}

static const std::string* find_field(const IdfFormFields& fields, const char* key)
{
    for (const auto& field : fields) {
        if (field.first == key) return &field.second;
    }
    return nullptr;
}

static bool get_query_param(httpd_req_t* req, const char* key, std::string& out, size_t max_query = 192)
{
    std::string query(max_query, '\0');
    if (httpd_req_get_url_query_str(req, query.data(), query.size()) != ESP_OK) return false;
    char raw[128] = {};
    if (httpd_query_key_value(query.c_str(), key, raw, sizeof(raw)) != ESP_OK) return false;
    out = url_decode(raw, strlen(raw));
    return true;
}

static std::string first_line_containing(const std::string& resp, const char* needle)
{
    size_t p = resp.find(needle);
    if (p == std::string::npos) return {};
    size_t start = resp.rfind('\n', p);
    start = (start == std::string::npos) ? 0 : start + 1;
    size_t end = resp.find('\n', p);
    if (end == std::string::npos) end = resp.size();
    std::string line = resp.substr(start, end - start);
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
    while (!line.empty() && isspace(static_cast<unsigned char>(line.front()))) line.erase(0, 1);
    return line;
}

static std::string first_digits(const std::string& resp)
{
    for (size_t i = 0; i < resp.size(); ++i) {
        if (!isdigit(static_cast<unsigned char>(resp[i]))) continue;
        size_t j = i;
        while (j < resp.size() && isdigit(static_cast<unsigned char>(resp[j]))) ++j;
        if (j - i >= 10) return resp.substr(i, j - i);
        i = j;
    }
    return {};
}

static esp_err_t handle_at(httpd_req_t* req)
{
    if (!check_auth(req)) return ESP_OK;
    char query[192] = {};
    char cmd_raw[96] = {};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, "cmd", cmd_raw, sizeof(cmd_raw)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing cmd");
        return ESP_OK;
    }

    std::string cmd = url_decode(cmd_raw, strlen(cmd_raw));
    if (cmd.size() > 80 || cmd.find('\r') != std::string::npos || cmd.find('\n') != std::string::npos) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid cmd");
        return ESP_OK;
    }
    if (modem_busy_reply(req)) return ESP_OK;

    std::string resp;
    esp_err_t err = idf_modem_send_at(cmd, 5000, resp);
    std::string body = "{\"success\":";
    body += (err == ESP_OK ? "true" : "false");
    body += ",";
    json_prop(body, "message", resp.empty() ? std::string(esp_err_to_name(err)) : resp);
    body += "}";
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, body.c_str(), body.size());
}

static esp_err_t handle_flight(httpd_req_t* req)
{
    if (!check_auth(req)) return ESP_OK;
    std::string action;
    get_query_param(req, "action", action);
    if (action.empty()) action = "query";
    if (modem_busy_reply(req)) return ESP_OK;

    std::string resp;
    bool success = false;
    std::string message;
    int mode = -1;
    if (idf_modem_send_at("AT+CFUN?", 3000, resp) == ESP_OK) {
        std::string line = first_line_containing(resp, "+CFUN:");
        const char* p = strstr(line.c_str(), "+CFUN:");
        if (p) {
            mode = atoi(p + strlen("+CFUN:"));
            success = true;
        }
    }

    if (action == "toggle" || action == "on" || action == "off") {
        // 查询失败(mode<0)时禁止切换：盲发 CFUN=4 会静默关掉射频、停掉短信接收
        if (mode < 0) {
            httpd_resp_set_type(req, "application/json");
            return httpd_resp_sendstr(req,
                "{\"success\":false,\"message\":\"无法获取当前飞行模式状态，已取消切换\"}");
        }
        int target;
        if (action == "on") target = 4;          // 进入飞行模式
        else if (action == "off") target = 1;    // 退出飞行模式
        else target = (mode == 1) ? 4 : 1;       // toggle：非正常态一律切回全功能
        char cmd[20];
        snprintf(cmd, sizeof(cmd), "AT+CFUN=%d", target);
        resp.clear();
        esp_err_t err = idf_modem_send_at(cmd, 8000, resp);
        success = (err == ESP_OK);
        if (success) mode = target;
        else message = resp.empty() ? esp_err_to_name(err) : resp;
    }

    if (success) {
        if (mode == 4) message = "飞行模式（射频关闭）";
        else if (mode == 1) message = "全功能模式（正常）";
        else if (mode == 0) message = "最小功能模式";
        else {
            char buf[48];
            snprintf(buf, sizeof(buf), "未知模式 (%d)", mode);
            message = buf;
        }
    } else if (message.empty()) {
        message = resp.empty() ? "无法获取飞行模式" : resp;
    }

    std::string body = "{\"success\":";
    body += success ? "true" : "false";
    body += ",";
    json_prop(body, "message", message);
    body += "}";
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, body.c_str(), body.size());
}

static esp_err_t handle_modem_control(httpd_req_t* req)
{
    if (!check_auth(req)) return ESP_OK;
    std::string action;
    get_query_param(req, "action", action);
    bool success = false;
    std::string message;

    if (action == "restart" || action == "hardreset") {
        bool hard = action == "hardreset";
        esp_err_t err = idf_modem_request_reset(hard);
        success = (err == ESP_OK);
        message = success
            ? (hard ? "正在硬重启模组，请等待约 15 秒后刷新页面" : "正在软重启模组，请等待约 15 秒后刷新页面")
            : esp_err_to_name(err);
    } else if (modem_busy_reply(req)) {
        return ESP_OK;
    } else if (action == "signal") {
        std::string resp;
        esp_err_t err = idf_modem_send_at("AT+CSQ", 3000, resp);
        std::string line = first_line_containing(resp, "+CSQ:");
        int rssi = 99;
        int ber = 99;
        if (err == ESP_OK && sscanf(line.c_str(), "+CSQ: %d,%d", &rssi, &ber) == 2) {
            int dbm = (rssi == 99) ? -999 : (-113 + rssi * 2);
            char buf[96];
            snprintf(buf, sizeof(buf), "信号强度(RSSI): %d dBm, CSQ原始值: %d, BER: %d", dbm, rssi, ber);
            message = buf;
            success = true;
        } else {
            message = resp.empty() ? esp_err_to_name(err) : resp;
        }
    } else if (action == "operator") {
        std::string resp;
        esp_err_t err = idf_modem_send_at("AT+COPS?", 5000, resp);
        std::string line = first_line_containing(resp, "+COPS:");
        if (err == ESP_OK && !line.empty()) {
            size_t q1 = line.find('"');
            size_t q2 = q1 == std::string::npos ? std::string::npos : line.find('"', q1 + 1);
            message = (q1 != std::string::npos && q2 != std::string::npos) ? line.substr(q1 + 1, q2 - q1 - 1) : line;
            success = true;
        } else {
            message = resp.empty() ? esp_err_to_name(err) : resp;
        }
    } else if (action == "imei") {
        std::string resp;
        esp_err_t err = idf_modem_send_at("AT+CGSN", 3000, resp);
        message = first_digits(resp);
        success = (err == ESP_OK && !message.empty());
        if (!success) message = resp.empty() ? esp_err_to_name(err) : resp;
    } else {
        message = "未知操作: " + action;
    }

    std::string body = "{\"success\":";
    body += success ? "true" : "false";
    body += ",";
    json_prop(body, "message", message);
    body += "}";
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, body.c_str(), body.size());
}

static esp_err_t handle_ussd(httpd_req_t* req)
{
    if (!check_auth(req)) return ESP_OK;
    std::string code;
    get_query_param(req, "code", code);
    bool valid = !code.empty() && code.size() <= 24;
    for (char ch : code) {
        if (!(isdigit(static_cast<unsigned char>(ch)) || ch == '*' || ch == '#')) {
            valid = false;
            break;
        }
    }
    if (!valid) {
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req, "{\"success\":false,\"message\":\"USSD 码为空或包含非法字符\"}");
    }
    if (modem_busy_reply(req)) return ESP_OK;

    std::string cmd = "AT+CUSD=1,\"" + code + "\",15";
    std::string resp;
    esp_err_t err = idf_modem_send_at_until(cmd, "+CUSD:", 20000, resp);
    bool success = (err == ESP_OK && resp.find("+CUSD:") != std::string::npos);
    std::string message = resp.empty() ? std::string(esp_err_to_name(err)) : resp;
    std::string body = "{\"success\":";
    body += success ? "true" : "false";
    body += ",";
    json_prop(body, "message", message);
    body += "}";
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, body.c_str(), body.size());
}

static bool plmn_valid(const std::string& plmn)
{
    if (plmn.empty()) return true;
    if (plmn.size() < 5 || plmn.size() > 6) return false;
    for (char ch : plmn) {
        if (!isdigit(static_cast<unsigned char>(ch))) return false;
    }
    return true;
}

static bool apn_valid_for_at(const std::string& apn)
{
    return apn.size() <= 96 && apn.find('"') == std::string::npos &&
           apn.find('\r') == std::string::npos && apn.find('\n') == std::string::npos;
}

struct ModemApplyTaskArg {
    bool dataChanged = false;
    bool operatorChanged = false;
    IdfConfig config;
};

static void modem_apply_task(void* raw)
{
    ModemApplyTaskArg* arg = static_cast<ModemApplyTaskArg*>(raw);
    IdfConfig cfg = arg->config;
    bool data_changed = arg->dataChanged;
    bool operator_changed = arg->operatorChanged;
    delete arg;

    IdfModemStatus modem = idf_modem_get_status();
    if (!modem.modemReady) {
        idf_log_line("SIM 设置已保存，模组未注册，暂不下发 COPS/CGACT");
        vTaskDelete(nullptr);
        return;
    }

    std::string resp;
    if (operator_changed) {
        if (!plmn_valid(cfg.operatorPlmn)) {
            idf_log_line("运营商 PLMN 非法，未下发 COPS");
        } else if (cfg.operatorPlmn.empty()) {
            idf_modem_send_at("AT+COPS=0", 30000, resp);
            idf_log_line("运营商: 自动注册(COPS=0)");
        } else {
            std::string cmd = "AT+COPS=1,2,\"" + cfg.operatorPlmn + "\"";
            esp_err_t err = idf_modem_send_at(cmd, 30000, resp);
            idf_logf("运营商: 锁定 PLMN %s %s", cfg.operatorPlmn.c_str(),
                     err == ESP_OK ? "成功" : "失败(可能不可达)");
        }
    }

    if (data_changed) {
        if (cfg.dataEnabled) {
            if (!cfg.apn.empty() && apn_valid_for_at(cfg.apn)) {
                std::string cmd = "AT+CGDCONT=1,\"IP\",\"" + cfg.apn + "\"";
                idf_modem_send_at(cmd, 3000, resp);
            } else if (!cfg.apn.empty()) {
                idf_log_line("APN 包含非法字符，未下发 CGDCONT");
            }
            idf_modem_send_at("AT+CGACT=1,1", 10000, resp);
            std::string ip_resp;
            idf_modem_send_at("AT+CGPADDR=1", 3000, ip_resp);
            idf_logf("蜂窝数据已启用(APN=%s)", cfg.apn.empty() ? "自动" : cfg.apn.c_str());
        } else {
            idf_modem_send_at("AT+CGACT=0,1", 5000, resp);
            idf_log_line("蜂窝数据已禁用(零流量)");
        }
    }
    vTaskDelete(nullptr);
}

static esp_err_t handle_save(httpd_req_t* req)
{
    if (!check_auth(req)) return ESP_OK;
    std::string body;
    // 16KB：5 个自定义推送模板 + 转发规则 URL 编码后可能超过 8KB
    if (read_body(req, body, 16384) != ESP_OK) return ESP_OK;
    IdfFormFields fields = parse_urlencoded(body);
    IdfConfig before = idf_config_get();
    esp_err_t err = idf_config_update_from_form(fields);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(err));
        return ESP_OK;
    }
    IdfConfig after = idf_config_get();
    bool sim_form = find_field(fields, "simForm") != nullptr;
    bool data_changed = sim_form && (before.dataEnabled != after.dataEnabled || before.apn != after.apn);
    bool operator_changed = sim_form && before.operatorPlmn != after.operatorPlmn;
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, "OK");
    if (data_changed || operator_changed) {
        ModemApplyTaskArg* arg = new (std::nothrow) ModemApplyTaskArg();
        if (arg) {
            arg->dataChanged = data_changed;
            arg->operatorChanged = operator_changed;
            arg->config = after;
            if (xTaskCreate(modem_apply_task, "idf_sim_apply", 4096, arg, 3, nullptr) != pdPASS) {
                delete arg;
                idf_log_line("SIM 设置已保存，但后台 AT 应用任务创建失败");
            }
        } else {
            idf_log_line("SIM 设置已保存，但后台 AT 应用任务内存不足");
        }
    }
    return ESP_OK;
}

static esp_err_t handle_export_config(httpd_req_t* req)
{
    if (!check_auth(req)) return ESP_OK;
    std::string body = idf_config_export_text();
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store, max-age=0");
    httpd_resp_set_hdr(req, "Content-Disposition", "attachment; filename=sms_config.txt");
    return httpd_resp_send(req, body.c_str(), body.size());
}

static esp_err_t handle_import_config(httpd_req_t* req)
{
    if (!check_auth(req)) return ESP_OK;
    std::string body;
    if (read_body(req, body, 16384) != ESP_OK) return ESP_OK;
    int applied = 0;
    esp_err_t err = idf_config_import_text(body, &applied);
    httpd_resp_set_type(req, "application/json");
    if (err != ESP_OK) {
        std::string resp = "{\"success\":false,";
        json_prop(resp, "message", std::string("导入失败: ") + esp_err_to_name(err));
        resp += "}";
        return httpd_resp_send(req, resp.c_str(), resp.size());
    }
    char msg[96];
    snprintf(msg, sizeof(msg), "已导入 %d 项，建议重启使全部生效", applied);
    std::string resp = "{\"success\":true,";
    json_prop(resp, "message", msg);
    resp += "}";
    return httpd_resp_send(req, resp.c_str(), resp.size());
}

static void restart_task(void*)
{
    vTaskDelay(pdMS_TO_TICKS(1200));
    esp_restart();
}

static esp_err_t handle_factory_reset(httpd_req_t* req)
{
    if (!check_auth(req)) return ESP_OK;
    esp_err_t err = idf_config_factory_reset();
    httpd_resp_set_type(req, "application/json");
    if (err != ESP_OK) {
        std::string body = "{\"success\":false,";
        json_prop(body, "message", std::string("恢复出厂失败: ") + esp_err_to_name(err));
        body += "}";
        return httpd_resp_send(req, body.c_str(), body.size());
    }
    httpd_resp_sendstr(req, "{\"success\":true,\"message\":\"已清除配置，设备即将重启为默认设置\"}");
    xTaskCreate(restart_task, "factory_restart", 2048, nullptr, 1, nullptr);
    return ESP_OK;
}

static esp_err_t handle_wifi_config(httpd_req_t* req)
{
    if (!check_auth(req)) return ESP_OK;
    std::string body;
    if (read_body(req, body, 1024) != ESP_OK) return ESP_OK;
    IdfFormFields fields = parse_urlencoded(body);
    const std::string* ssid = find_field(fields, "ssid");
    const std::string* pass = find_field(fields, "pass");
    esp_err_t err = idf_config_save_wifi(ssid ? *ssid : std::string(), pass ? *pass : std::string());
    httpd_resp_set_type(req, "application/json");
    if (err != ESP_OK) {
        std::string msg = "{\"success\":false,\"message\":\"WiFi 配置无效\"}";
        return httpd_resp_send(req, msg.c_str(), msg.size());
    }
    xTaskCreate(restart_task, "wifi_restart", 2048, nullptr, 1, nullptr);
    std::string msg = "{\"success\":true,\"message\":\"WiFi 已保存，设备即将重启\"}";
    return httpd_resp_send(req, msg.c_str(), msg.size());
}

static esp_err_t handle_wifi(httpd_req_t* req)
{
    if (!check_auth(req)) return ESP_OK;
    char query[64] = {};
    char action[24] = {};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        httpd_query_key_value(query, "action", action, sizeof(action));
    }
    httpd_resp_set_type(req, "application/json");
    if (strcmp(action, "restart") == 0) {
        esp_err_t err = idf_wifi_reconnect();
        std::string msg = err == ESP_OK
            ? "{\"success\":true,\"message\":\"已触发 WiFi 重连\"}"
            : "{\"success\":false,\"message\":\"WiFi 尚未启动\"}";
        return httpd_resp_send(req, msg.c_str(), msg.size());
    }
    IdfWifiStatus wifi = idf_wifi_get_status();
    std::string body = "{\"success\":true,\"connected\":";
    body += wifi.staConnected ? "true" : "false";
    body += ",\"apMode\":";
    body += wifi.apMode ? "true" : "false";
    body += "}";
    return httpd_resp_send(req, body.c_str(), body.size());
}

static esp_err_t handle_wifi_scan(httpd_req_t* req)
{
    if (!check_auth(req)) return ESP_OK;
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store, max-age=0");
    std::string body;
    esp_err_t err = idf_wifi_scan_json(body);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(err));
        return ESP_OK;
    }
    return httpd_resp_send(req, body.c_str(), body.size());
}

static bool parse_multipart_boundary(const char* content_type, std::string& marker)
{
    const char* p = strstr(content_type, "boundary=");
    if (!p) return false;
    p += strlen("boundary=");
    std::string boundary = p;
    size_t semi = boundary.find(';');
    if (semi != std::string::npos) boundary.resize(semi);
    if (!boundary.empty() && boundary.front() == '"') boundary.erase(0, 1);
    if (!boundary.empty() && boundary.back() == '"') boundary.pop_back();
    if (boundary.empty() || boundary.size() > 80) return false;
    marker = "\r\n--";
    marker += boundary;
    return true;
}

static esp_err_t handle_ota_update(httpd_req_t* req)
{
    if (!check_auth(req)) return ESP_OK;

    char ctype[160] = {};
    std::string boundary_marker;
    if (httpd_req_get_hdr_value_str(req, "Content-Type", ctype, sizeof(ctype)) != ESP_OK ||
        !parse_multipart_boundary(ctype, boundary_marker)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing multipart boundary");
        return ESP_OK;
    }

    const esp_partition_t* part = esp_ota_get_next_update_partition(nullptr);
    if (!part) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No OTA partition");
        return ESP_OK;
    }

    esp_ota_handle_t ota = 0;
    // 顺序擦除写入：整分区(1.9MB)预擦除会卡住 httpd 任务好几秒，浏览器端表现为长时间无响应
    esp_err_t err = esp_ota_begin(part, OTA_WITH_SEQUENTIAL_WRITES, &ota);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(err));
        return ESP_OK;
    }

    std::string pending;
    pending.reserve(boundary_marker.size() + 1024);
    bool in_file = false;
    bool saw_boundary = false;
    size_t received = 0;
    size_t written = 0;
    char buf[1024];
    const size_t keep_tail = boundary_marker.size() + 8;

    int timeouts = 0;
    while (received < req->content_len && err == ESP_OK) {
        int got = httpd_req_recv(req, buf, std::min(sizeof(buf), static_cast<size_t>(req->content_len - received)));
        if (got <= 0) {
            // 上传中断的客户端不能无限重试，否则 OTA 句柄一直占着、Web 全站失去响应
            if (got == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts <= 3) continue;
            err = ESP_ERR_INVALID_RESPONSE;
            break;
        }
        timeouts = 0;
        received += static_cast<size_t>(got);
        pending.append(buf, got);

        if (!in_file) {
            size_t header_end = pending.find("\r\n\r\n");
            if (header_end == std::string::npos) {
                if (pending.size() > 2048) err = ESP_ERR_INVALID_RESPONSE;
                continue;
            }
            pending.erase(0, header_end + 4);
            in_file = true;
        }

        while (pending.size() > keep_tail && err == ESP_OK) {
            size_t writable = pending.size() - keep_tail;
            err = esp_ota_write(ota, pending.data(), writable);
            if (err == ESP_OK) {
                written += writable;
                pending.erase(0, writable);
            }
        }
    }

    if (err == ESP_OK) {
        size_t boundary = pending.find(boundary_marker);
        if (boundary == std::string::npos) {
            err = ESP_ERR_INVALID_RESPONSE;
        } else {
            if (boundary > 0) {
                err = esp_ota_write(ota, pending.data(), boundary);
                if (err == ESP_OK) written += boundary;
            }
            saw_boundary = true;
        }
    }

    if (err == ESP_OK && (!in_file || !saw_boundary || written == 0)) err = ESP_ERR_INVALID_SIZE;
    if (err == ESP_OK) err = esp_ota_end(ota);
    else esp_ota_abort(ota);
    if (err == ESP_OK) err = esp_ota_set_boot_partition(part);

    httpd_resp_set_type(req, "application/json");
    if (err != ESP_OK) {
        std::string body = "{\"success\":false,";
        json_prop(body, "message", std::string("升级失败: ") + esp_err_to_name(err));
        body += "}";
        idf_logf("OTA 失败: %s", esp_err_to_name(err));
        return httpd_resp_send(req, body.c_str(), body.size());
    }

    idf_logf("OTA 完成: %u 字节，准备重启", static_cast<unsigned>(written));
    httpd_resp_sendstr(req, "{\"success\":true,\"message\":\"升级成功，设备重启中\"}");
    xTaskCreate(restart_task, "ota_restart", 2048, nullptr, 1, nullptr);
    return ESP_OK;
}

static esp_err_t handle_send_sms(httpd_req_t* req)
{
    if (!check_auth(req)) return ESP_OK;
    std::string raw;
    if (read_body(req, raw, 2048) != ESP_OK) return ESP_OK;
    IdfFormFields fields = parse_urlencoded(raw);
    const std::string* phone = find_field(fields, "phone");
    const std::string* content = find_field(fields, "content");
    std::string msg;
    esp_err_t err = idf_sms_enqueue_outgoing(phone ? *phone : std::string(),
                                             content ? *content : std::string(),
                                             msg);
    std::string body = "{\"success\":";
    body += (err == ESP_OK ? "true" : "false");
    body += ",\"queued\":";
    body += (err == ESP_OK ? "true" : "false");
    body += ",";
    json_prop(body, "message", msg);
    body += "}";
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, body.c_str(), body.size());
}

static esp_err_t handle_messages(httpd_req_t* req)
{
    if (!check_auth(req)) return ESP_OK;
    char query[96] = {};
    char box_raw[16] = {};
    char limit_raw[16] = {};
    bool sent_box = false;
    int limit = 0;
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        if (httpd_query_key_value(query, "box", box_raw, sizeof(box_raw)) == ESP_OK &&
            strcmp(box_raw, "sent") == 0) {
            sent_box = true;
        }
        if (httpd_query_key_value(query, "limit", limit_raw, sizeof(limit_raw)) == ESP_OK) {
            limit = atoi(limit_raw);
        }
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store, max-age=0");
    std::string body = idf_inbox_json(sent_box, limit);
    return httpd_resp_send(req, body.c_str(), body.size());
}

static esp_err_t handle_empty_log(httpd_req_t* req)
{
    if (!check_auth(req)) return ESP_OK;
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store, max-age=0");
    char query[64] = {};
    char since_raw[24] = {};
    uint32_t since = 0;
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK &&
        httpd_query_key_value(query, "since", since_raw, sizeof(since_raw)) == ESP_OK) {
        since = static_cast<uint32_t>(strtoul(since_raw, nullptr, 10));
    }
    std::string body = idf_log_json_since(since);
    return httpd_resp_send(req, body.c_str(), body.size());
}

static esp_err_t handle_log_download(httpd_req_t* req)
{
    if (!check_auth(req)) return ESP_OK;
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_set_hdr(req, "Content-Disposition", "attachment; filename=sms_idf_log.txt");
    std::string body = idf_log_text_dump();
    return httpd_resp_send(req, body.c_str(), body.size());
}

static bool query_u32(httpd_req_t* req, const char* key, uint32_t& value)
{
    char query[96] = {};
    char raw[24] = {};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, key, raw, sizeof(raw)) != ESP_OK) {
        return false;
    }
    value = static_cast<uint32_t>(strtoul(raw, nullptr, 10));
    return value != 0;
}

static esp_err_t handle_delete_message(httpd_req_t* req)
{
    if (!check_auth(req)) return ESP_OK;
    uint32_t id = 0;
    bool ok = query_u32(req, "id", id) && idf_inbox_delete(id);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, ok
        ? "{\"success\":true,\"message\":\"已删除\"}"
        : "{\"success\":false,\"message\":\"未找到\"}");
}

static esp_err_t handle_resend_message(httpd_req_t* req)
{
    if (!check_auth(req)) return ESP_OK;
    uint32_t id = 0;
    IdfInboxEntry entry;
    bool found = query_u32(req, "id", id) && idf_inbox_get_by_id(id, entry);
    httpd_resp_set_type(req, "application/json");
    if (!found) {
        return httpd_resp_sendstr(req, "{\"success\":false,\"message\":\"未找到该短信\"}");
    }
    bool ok = idf_push_enqueue_forward(entry.sender.c_str(), entry.text.c_str(), entry.ts.c_str(), entry.id);
    idf_logf("网页手动重发短信 id=%u: %s", static_cast<unsigned>(id), ok ? "已入队" : "入队失败");
    return httpd_resp_sendstr(req, ok
        ? "{\"success\":true,\"message\":\"已重新入队转发\"}"
        : "{\"success\":false,\"message\":\"转发队列繁忙，请稍后重试\"}");
}

static esp_err_t handle_test_push(httpd_req_t* req)
{
    if (!check_auth(req)) return ESP_OK;
    std::string action;
    std::string ch_raw;
    get_query_param(req, "action", action);
    get_query_param(req, "ch", ch_raw);
    uint8_t ch = static_cast<uint8_t>(strtoul(ch_raw.c_str(), nullptr, 10));
    httpd_resp_set_type(req, "application/json");
    if (action == "status") {
        std::string body = idf_push_test_status_json(ch);
        return httpd_resp_send(req, body.c_str(), body.size());
    }

    std::string msg;
    bool ok = idf_push_enqueue_test(ch, msg);
    std::string body = "{\"success\":";
    body += ok ? "true" : "false";
    body += ",\"queued\":";
    body += ok ? "true" : "false";
    body += ",";
    json_prop(body, "message", msg);
    body += "}";
    return httpd_resp_send(req, body.c_str(), body.size());
}

static std::string trim_copy(std::string value)
{
    size_t start = 0;
    while (start < value.size() && isspace(static_cast<unsigned char>(value[start]))) ++start;
    size_t end = value.size();
    while (end > start && isspace(static_cast<unsigned char>(value[end - 1]))) --end;
    return value.substr(start, end - start);
}

static bool keepalive_url_valid(const std::string& raw_url, std::string& err)
{
    std::string url = trim_copy(raw_url);
    if (url.size() > 240) {
        err = "URL过长";
        return false;
    }
    if (!(url.rfind("http://", 0) == 0 || url.rfind("https://", 0) == 0)) {
        err = "URL需要以 http:// 或 https:// 开头";
        return false;
    }
    if (url.find('"') != std::string::npos || url.find(' ') != std::string::npos) {
        err = "URL包含非法字符";
        return false;
    }
    return true;
}

static bool cell_job_lock(TickType_t ticks = pdMS_TO_TICKS(300))
{
    return s_cell_job_mutex && xSemaphoreTake(s_cell_job_mutex, ticks) == pdTRUE;
}

static void cell_job_unlock(void)
{
    xSemaphoreGive(s_cell_job_mutex);
}

static bool cellular_job_active_locked(void)
{
    return s_ping_job.running || s_ping_job.queued ||
           s_keepalive_job.running || s_keepalive_job.queued;
}

static std::string cellular_http_message(const IdfCellularHttpResult& result)
{
    char buf[192];
    if (result.ok) {
        snprintf(buf, sizeof(buf), "HTTP %d，已通过蜂窝下载约 %uKB payload",
                 result.httpStatus, static_cast<unsigned>(result.bytesRead / 1024UL));
    } else if (result.httpStatus >= 0) {
        snprintf(buf, sizeof(buf), "%s(HTTP %d，%uKB)",
                 result.message.empty() ? "蜂窝HTTP payload 下载失败" : result.message.c_str(),
                 result.httpStatus, static_cast<unsigned>(result.bytesRead / 1024UL));
    } else {
        snprintf(buf, sizeof(buf), "%s",
                 result.message.empty() ? "蜂窝HTTP payload 下载失败" : result.message.c_str());
    }
    return std::string(buf);
}

struct PingTaskArg {
    std::string url;
    IdfConfig config;
};

static void ping_task(void* arg_raw)
{
    PingTaskArg* arg = static_cast<PingTaskArg*>(arg_raw);
    if (cell_job_lock()) {
        s_ping_job.queued = false;
        s_ping_job.running = true;
        s_ping_job.message = "后台HTTP payload 下载中";
        cell_job_unlock();
    }

    IdfCellularHttpResult result;
    esp_err_t err = idf_modem_cellular_http_get(arg->url, arg->config, result);
    bool ok = (err == ESP_OK && result.ok);
    std::string message = cellular_http_message(result);

    // 终态写入用无限等锁，理由同 keepalive_task：状态卡在 running 会永久拒绝后续任务
    if (cell_job_lock(portMAX_DELAY)) {
        s_ping_job.running = false;
        s_ping_job.queued = false;
        s_ping_job.done = true;
        s_ping_job.success = ok;
        s_ping_job.message = message;
        cell_job_unlock();
    }
    delete arg;
    vTaskDelete(nullptr);
}

static bool valid_ussd_code(const std::string& code)
{
    if (code.empty() || code.size() > 24) return false;
    for (char ch : code) {
        if (!(isdigit(static_cast<unsigned char>(ch)) || ch == '*' || ch == '#')) return false;
    }
    return true;
}

struct KeepAliveTaskArg {
    IdfConfig config;
};

static void enqueue_maintenance_notice(const IdfConfig& cfg, const char* title,
                                       const std::string& body, uint32_t now)
{
    std::string ts = format_epoch_local(now, cfg.tzOffsetMin);
    int pushed = idf_push_enqueue_notify(title, body.c_str(), ts.c_str());
    if (pushed > 0) idf_logf("%s推送已入队: %d 个通道", title, pushed);
    else idf_logf("%s无有效推送通道", title);

    if (!cfg.emailEnabled) return;
    idf_push_enqueue_email(title, body.c_str());
}

static void keepalive_task(void* arg_raw)
{
    KeepAliveTaskArg* arg = static_cast<KeepAliveTaskArg*>(arg_raw);
    IdfConfig cfg = arg->config;
    delete arg;

    if (cell_job_lock()) {
        s_keepalive_job.queued = false;
        s_keepalive_job.running = true;
        s_keepalive_job.message = "保号动作执行中";
        cell_job_unlock();
    }

    bool ok = false;
    std::string message;
    if (cfg.kaAction == 2) {
        if (cfg.kaTarget.empty()) {
            message = "保号短信目标号码为空";
        } else {
            esp_err_t err = idf_sms_send_text(cfg.kaTarget, "keepalive", message);
            ok = (err == ESP_OK);
        }
    } else if (cfg.kaAction == 3) {
        if (!valid_ussd_code(cfg.kaTarget)) {
            message = "USSD 码为空或包含非法字符";
        } else {
            std::string cmd = "AT+CUSD=1,\"" + cfg.kaTarget + "\",15";
            std::string resp;
            esp_err_t err = idf_modem_send_at_until(cmd, "+CUSD:", 20000, resp);
            ok = (err == ESP_OK && resp.find("+CUSD:") != std::string::npos);
            message = resp.empty() ? std::string(esp_err_to_name(err)) : resp;
        }
    } else {
        IdfCellularHttpResult result;
        std::string url = cfg.kaUrl.empty() ? std::string(IDF_KEEPALIVE_DEFAULT_URL) : cfg.kaUrl;
        esp_err_t err = idf_modem_cellular_http_get(url, cfg, result);
        ok = (err == ESP_OK && result.ok);
        message = cellular_http_message(result);
    }

    if (ok) {
        uint32_t now = static_cast<uint32_t>(time(nullptr));
        if (now >= 1700000000u && idf_config_set_keepalive_last(now) == ESP_OK) {
            message += "，已更新保号基准日";
        }
        idf_log_line("保号动作成功");
        std::string notice = "保号动作已成功执行。\n方式: ";
        notice += (cfg.kaAction == 2 ? "发送短信" : (cfg.kaAction == 3 ? "USSD 查询" : "蜂窝数据流量"));
        notice += "\n结果: " + message;
        enqueue_maintenance_notice(cfg, "保号动作已执行", notice, now);
    } else {
        idf_logf("保号动作失败: %s", message.c_str());
    }

    // 终态必须写进去：这里拿不到锁就放弃的话，任务状态永远停在 running，
    // 后续所有保号/诊断请求都会被"已有任务在执行"拒绝，直到重启
    if (cell_job_lock(portMAX_DELAY)) {
        s_keepalive_job.running = false;
        s_keepalive_job.queued = false;
        s_keepalive_job.done = true;
        s_keepalive_job.success = ok;
        s_keepalive_job.message = ok ? (message.empty() ? "保号动作已完成" : message)
                                     : (message.empty() ? "保号动作失败，请查看日志" : message);
        cell_job_unlock();
    }
    vTaskDelete(nullptr);
}

static bool start_keepalive_job(const IdfConfig& cfg, const char* queued_message,
                                std::string& message, bool& already_running)
{
    already_running = false;
    if (!cell_job_lock()) {
        message = "保号任务状态锁繁忙";
        return false;
    }
    if (cellular_job_active_locked()) {
        already_running = true;
        message = "已有蜂窝/保号任务正在后台执行";
        cell_job_unlock();
        return false;
    }
    s_keepalive_job = WebAsyncJob();
    s_keepalive_job.queued = true;
    s_keepalive_job.done = false;
    s_keepalive_job.success = false;
    s_keepalive_job.message = queued_message;
    cell_job_unlock();

    KeepAliveTaskArg* arg = new (std::nothrow) KeepAliveTaskArg();
    if (!arg) {
        if (cell_job_lock()) {
            s_keepalive_job.queued = false;
            s_keepalive_job.done = true;
            s_keepalive_job.success = false;
            s_keepalive_job.message = "创建保号任务失败：内存不足";
            cell_job_unlock();
        }
        message = "创建保号任务失败：内存不足";
        return false;
    }
    arg->config = cfg;
    if (xTaskCreate(keepalive_task, "idf_keepalive", 6144, arg, 3, nullptr) != pdPASS) {
        delete arg;
        if (cell_job_lock()) {
            s_keepalive_job.queued = false;
            s_keepalive_job.done = true;
            s_keepalive_job.success = false;
            s_keepalive_job.message = "创建保号任务失败";
            cell_job_unlock();
        }
        message = "创建保号任务失败";
        return false;
    }
    message = queued_message;
    idf_log_line("保号动作已入队");
    return true;
}

static bool keepalive_due(uint32_t last_ts, uint32_t now, uint32_t interval_days)
{
    if (!epoch_valid(now) || interval_days == 0) return false;
    if (!epoch_valid(last_ts)) return true;
    return (now - last_ts) >= interval_days * 86400u;
}

static bool cellular_job_active();

static bool system_idle_for_maintenance()
{
    return !idf_push_busy() &&
           idf_push_forward_queue_depth() == 0 &&
           idf_push_retry_queue_depth() == 0 &&
           idf_sms_outgoing_queue_depth() == 0 &&
           idf_push_email_queue_depth() == 0 &&
           !cellular_job_active();
}

static bool cellular_job_active()
{
    bool active = false;
    if (cell_job_lock()) {
        active = cellular_job_active_locked();
        cell_job_unlock();
    }
    return active;
}

static void scheduler_task(void*)
{
    uint32_t last_ka_check_ms = 0;
    int64_t hb_last_day = -1;
    int64_t rb_last_day = -1;

    while (true) {
        // —— 低堆守护：无条件运行(5s 周期)。放在 NTP 门控里的话，配网模式/断网
        // 期间恰恰是最容易内存紧张的场景，却完全失去自愈能力 ——
        if (heap_caps_get_free_size(MALLOC_CAP_8BIT) < 20000U && system_idle_for_maintenance()) {
            idf_logf("空闲堆低于阈值(%u<20000)，准备有序重启",
                     static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_8BIT)));
            vTaskDelay(pdMS_TO_TICKS(300));
            esp_restart();
        }

        uint32_t now = static_cast<uint32_t>(time(nullptr));
        if (epoch_valid(now)) {
            IdfConfig cfg = idf_config_get();
            uint32_t now_ms = static_cast<uint32_t>(esp_timer_get_time() / 1000ULL);

            if (cfg.kaEnabled && (last_ka_check_ms == 0 || now_ms - last_ka_check_ms >= 3600000UL)) {
                last_ka_check_ms = now_ms;
                if (keepalive_due(cfg.kaLastTime, now, static_cast<uint32_t>(cfg.kaIntervalDays))) {
                    std::string msg;
                    bool already = false;
                    if (start_keepalive_job(cfg, "定时保号动作已排队", msg, already)) {
                        idf_log_line("保号到期，触发动作");
                    }
                }
            }

            int64_t local = static_cast<int64_t>(now) + static_cast<int64_t>(cfg.tzOffsetMin) * 60LL;
            int hour = static_cast<int>((local / 3600LL) % 24LL);
            if (hour < 0) hour += 24;
            int64_t day = local / 86400LL;
            if (local < 0 && (local % 86400LL) != 0) --day;

            if (cfg.hbEnabled && hour == cfg.hbHour && day != hb_last_day) {
                hb_last_day = day;
                IdfSmsStatus sms = idf_sms_get_status();
                char body[192];
                snprintf(body, sizeof(body), "设备运行正常。\n累计转发: %u 条\n空闲堆: %u KB",
                         static_cast<unsigned>(sms.total),
                         static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_8BIT) / 1024U));
                enqueue_maintenance_notice(cfg, "设备每日心跳", body, now);
            }

            uint64_t uptime_ms = static_cast<uint64_t>(esp_timer_get_time() / 1000ULL);
            if (cfg.rebootEnabled && hour == cfg.rebootHour && day != rb_last_day &&
                uptime_ms >= 7200000ULL && system_idle_for_maintenance()) {
                rb_last_day = day;
                idf_log_line("每日定时重启...");
                vTaskDelay(pdMS_TO_TICKS(300));
                esp_restart();
            }
        }
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

static esp_err_t handle_ping(httpd_req_t* req)
{
    if (!check_auth(req)) return ESP_OK;
    std::string action;
    get_query_param(req, "action", action);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store, max-age=0");

    if (action == "status") {
        WebAsyncJob job;
        if (cell_job_lock()) {
            job = s_ping_job;
            cell_job_unlock();
        }
        std::string body = "{\"running\":";
        body += job.running ? "true" : "false";
        body += ",\"done\":";
        body += job.done ? "true" : "false";
        body += ",\"success\":";
        body += job.success ? "true" : "false";
        body += ",";
        json_prop(body, "url", job.url);
        body += ",";
        json_prop(body, "message", job.message);
        body += "}";
        return httpd_resp_send(req, body.c_str(), body.size());
    }

    std::string raw;
    if (read_body(req, raw, 512) != ESP_OK) return ESP_OK;
    IdfFormFields fields = parse_urlencoded(raw);
    const std::string* url_field = find_field(fields, "url");
    std::string url = trim_copy(url_field ? *url_field : std::string());
    const IdfConfig& current = idf_config_get();
    if (url.empty()) url = current.kaUrl.empty() ? std::string(IDF_KEEPALIVE_DEFAULT_URL) : current.kaUrl;

    std::string err_msg;
    if (!keepalive_url_valid(url, err_msg)) {
        std::string body = "{\"success\":false,";
        json_prop(body, "message", err_msg);
        body += "}";
        return httpd_resp_send(req, body.c_str(), body.size());
    }

    if (!cell_job_lock()) {
        return httpd_resp_sendstr(req, "{\"success\":false,\"message\":\"蜂窝任务状态锁繁忙\"}");
    }
    if (cellular_job_active_locked()) {
        cell_job_unlock();
        return httpd_resp_sendstr(req, "{\"success\":false,\"running\":true,\"message\":\"已有蜂窝任务正在执行，请稍候\"}");
    }

    s_ping_job = WebAsyncJob();
    s_ping_job.running = true;
    s_ping_job.queued = true;
    s_ping_job.url = url;
    s_ping_job.message = "后台HTTP payload 下载中";
    cell_job_unlock();

    PingTaskArg* arg = new (std::nothrow) PingTaskArg();
    if (!arg) {
        if (cell_job_lock()) {
            s_ping_job.running = false;
            s_ping_job.queued = false;
            s_ping_job.done = true;
            s_ping_job.success = false;
            s_ping_job.message = "创建蜂窝HTTP任务失败：内存不足";
            cell_job_unlock();
        }
        return httpd_resp_sendstr(req, "{\"success\":false,\"message\":\"创建任务失败：内存不足\"}");
    }
    arg->url = url;
    arg->config = current;
    if (xTaskCreate(ping_task, "idf_ping_http", 6144, arg, 3, nullptr) != pdPASS) {
        delete arg;
        if (cell_job_lock()) {
            s_ping_job.running = false;
            s_ping_job.queued = false;
            s_ping_job.done = true;
            s_ping_job.success = false;
            s_ping_job.message = "创建蜂窝HTTP任务失败";
            cell_job_unlock();
        }
        return httpd_resp_sendstr(req, "{\"success\":false,\"message\":\"创建任务失败\"}");
    }

    idf_logf("网页端发起后台HTTP payload 请求: %s", url.c_str());
    return httpd_resp_sendstr(req, "{\"success\":true,\"running\":true,\"message\":\"已开始后台HTTP payload下载，可继续刷新网页\"}");
}

static esp_err_t handle_keepalive(httpd_req_t* req)
{
    if (!check_auth(req)) return ESP_OK;
    std::string action;
    get_query_param(req, "action", action);
    if (action == "reset") {
        uint32_t now = static_cast<uint32_t>(time(nullptr));
        httpd_resp_set_type(req, "application/json");
        if (now < 1700000000u) {
            // 时间未同步时写 0 会让 keepalive_due 立即判定"到期"，触发一次
            // 计划外的蜂窝流量/短信保号动作——拒绝而不是照单全收
            return httpd_resp_sendstr(req,
                "{\"success\":false,\"message\":\"设备时间未同步，暂不能重置基准日，请等待 NTP 同步后重试\"}");
        }
        esp_err_t err = idf_config_set_keepalive_last(now);
        if (err == ESP_OK) {
            const IdfConfig& cfg = idf_config_get();
            std::string local = format_epoch_local(now, cfg.tzOffsetMin);
            std::string body = "{\"success\":true,";
            json_prop(body, "message", local.empty() ? "基准日已重置" : std::string("基准日已重置为 ") + local);
            char buf[64];
            snprintf(buf, sizeof(buf), ",\"lastTime\":%u,", static_cast<unsigned>(now));
            body += buf;
            json_prop(body, "lastTimeLocal", local);
            body += "}";
            return httpd_resp_send(req, body.c_str(), body.size());
        }
        std::string body = "{\"success\":false,";
        json_prop(body, "message", std::string("基准日重置失败: ") + esp_err_to_name(err));
        body += "}";
        return httpd_resp_send(req, body.c_str(), body.size());
    }
    if (action == "run") {
        httpd_resp_set_type(req, "application/json");
        std::string message;
        bool already_running = false;
        bool ok = start_keepalive_job(idf_config_get(), "保号动作已排队，可继续刷新网页",
                                      message, already_running);
        std::string body = "{\"success\":";
        body += (ok || already_running) ? "true" : "false";
        body += ",\"queued\":";
        body += (ok || already_running) ? "true" : "false";
        body += ",";
        json_prop(body, "message", message);
        body += "}";
        return httpd_resp_send(req, body.c_str(), body.size());
    }

    const IdfConfig& cfg = idf_config_get();
    uint32_t now = static_cast<uint32_t>(time(nullptr));
    bool time_valid = now >= 1700000000u;
    int days_left = 0;
    uint32_t next_time = 0;
    if (time_valid && cfg.kaLastTime >= 1700000000u && cfg.kaIntervalDays > 0) {
        uint64_t next64 = static_cast<uint64_t>(cfg.kaLastTime) + static_cast<uint64_t>(cfg.kaIntervalDays) * 86400ULL;
        next_time = next64 > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(next64);
        uint32_t elapsed_days = (now - cfg.kaLastTime) / 86400u;
        days_left = cfg.kaIntervalDays > static_cast<int>(elapsed_days)
            ? cfg.kaIntervalDays - static_cast<int>(elapsed_days)
            : 0;
    }
    WebAsyncJob job;
    if (cell_job_lock()) {
        job = s_keepalive_job;
        cell_job_unlock();
    }
    std::string body;
    body.reserve(760);
    char buf[160];
    snprintf(buf, sizeof(buf),
             "{\"enabled\":%s,\"intervalDays\":%d,\"action\":%u,",
             cfg.kaEnabled ? "true" : "false", cfg.kaIntervalDays,
             static_cast<unsigned>(cfg.kaAction));
    body += buf;
    json_prop(body, "target", cfg.kaTarget); body += ",";
    json_prop(body, "url", cfg.kaUrl); body += ",";
    snprintf(buf, sizeof(buf),
             "\"timeValid\":%s,\"tz\":%d,\"nowEpoch\":%u,",
             time_valid ? "true" : "false",
             cfg.tzOffsetMin,
             static_cast<unsigned>(time_valid ? now : 0));
    body += buf;
    json_prop(body, "tzName", format_tz_offset(cfg.tzOffsetMin)); body += ",";
    json_prop(body, "nowLocal", format_epoch_local(time_valid ? now : 0, cfg.tzOffsetMin)); body += ",";
    snprintf(buf, sizeof(buf),
             "\"lastTime\":%u,\"nextTime\":%u,\"daysLeft\":%d,",
             static_cast<unsigned>(cfg.kaLastTime),
             static_cast<unsigned>(next_time),
             days_left);
    body += buf;
    json_prop(body, "lastTimeLocal", format_epoch_local(cfg.kaLastTime, cfg.tzOffsetMin)); body += ",";
    json_prop(body, "nextTimeLocal", format_epoch_local(next_time, cfg.tzOffsetMin)); body += ",";
    snprintf(buf, sizeof(buf),
             "\"jobQueued\":%s,\"jobRunning\":%s,\"jobDone\":%s,"
             "\"jobSuccess\":%s,\"success\":%s,\"queued\":%s,",
             job.queued ? "true" : "false",
             job.running ? "true" : "false",
             job.done ? "true" : "false",
             job.success ? "true" : "false",
             job.success ? "true" : "false",
             (job.queued || job.running) ? "true" : "false");
    body += buf;
    json_prop(body, "jobMessage", job.message); body += ",";
    json_prop(body, "message", job.message);
    body += "}";
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, body.c_str(), body.size());
}

static esp_err_t handle_reboot(httpd_req_t* req)
{
    if (!check_auth(req)) return ESP_OK;
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"success\":true,\"message\":\"设备即将重启\"}");
    xTaskCreate(restart_task, "web_restart", 2048, nullptr, 1, nullptr);
    return ESP_OK;
}

static esp_err_t handle_not_found(httpd_req_t* req)
{
    if (idf_wifi_get_status().apMode) {
        httpd_resp_set_status(req, "302 Found");
        httpd_resp_set_hdr(req, "Location", "http://192.168.1.1/");
        return httpd_resp_send(req, nullptr, 0);
    }
    httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Not Found");
    return ESP_OK;
}

static esp_err_t register_handler(httpd_handle_t server, const char* uri, httpd_method_t method,
                                  esp_err_t (*handler)(httpd_req_t*))
{
    httpd_uri_t item = {};
    item.uri = uri;
    item.method = method;
    item.handler = handler;
    return httpd_register_uri_handler(server, &item);
}

esp_err_t idf_web_start(void)
{
    if (s_server) return ESP_OK;
    if (!s_cell_job_mutex) {
        s_cell_job_mutex = xSemaphoreCreateMutex();
        if (!s_cell_job_mutex) return ESP_ERR_NO_MEM;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.uri_match_fn = httpd_uri_match_wildcard;
    config.lru_purge_enable = true;
    config.max_uri_handlers = 32;
    config.stack_size = 8192;

    esp_err_t err = httpd_start(&s_server, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed: %s", esp_err_to_name(err));
        return err;
    }

    httpd_uri_t root = {};
    root.uri = "/";
    root.method = HTTP_GET;
    root.handler = handle_root;

    httpd_uri_t assets = {};
    assets.uri = "/assets/*";
    assets.method = HTTP_GET;
    assets.handler = handle_asset;

    httpd_uri_t ui = {};
    ui.uri = "/ui";
    ui.method = HTTP_GET;
    ui.handler = handle_ui_panel;

    httpd_uri_t status = {};
    status.uri = "/status";
    status.method = HTTP_GET;
    status.handler = handle_status;

    httpd_uri_t config_json = {};
    config_json.uri = "/config.json";
    config_json.method = HTTP_GET;
    config_json.handler = send_config_json;

    httpd_uri_t save = {};
    save.uri = "/save";
    save.method = HTTP_POST;
    save.handler = handle_save;

    httpd_uri_t wifi = {};
    wifi.uri = "/wifi";
    wifi.method = HTTP_GET;
    wifi.handler = handle_wifi;

    httpd_uri_t wifi_scan = {};
    wifi_scan.uri = "/wifiscan";
    wifi_scan.method = HTTP_GET;
    wifi_scan.handler = handle_wifi_scan;

    httpd_uri_t wifi_config = {};
    wifi_config.uri = "/wificonfig";
    wifi_config.method = HTTP_POST;
    wifi_config.handler = handle_wifi_config;

    httpd_uri_t messages = {};
    messages.uri = "/messages";
    messages.method = HTTP_GET;
    messages.handler = handle_messages;

    httpd_uri_t log = {};
    log.uri = "/log";
    log.method = HTTP_GET;
    log.handler = handle_empty_log;

    httpd_uri_t keepalive = {};
    keepalive.uri = "/keepalive";
    keepalive.method = HTTP_GET;
    keepalive.handler = handle_keepalive;

    httpd_uri_t reboot = {};
    reboot.uri = "/reboot";
    reboot.method = HTTP_POST;
    reboot.handler = handle_reboot;

    httpd_uri_t pending_get = {};
    pending_get.uri = "/at";
    pending_get.method = HTTP_GET;
    pending_get.handler = handle_at;

    httpd_uri_t pending_post = {};
    pending_post.uri = "/ping";
    pending_post.method = HTTP_POST;
    pending_post.handler = handle_ping;

    httpd_uri_t not_found = {};
    not_found.uri = "/*";
    not_found.method = HTTP_GET;
    not_found.handler = handle_not_found;

    auto register_checked = [&](const char* name, esp_err_t reg_err) -> esp_err_t {
        if (reg_err == ESP_OK) return ESP_OK;
        ESP_LOGE(TAG, "注册 HTTP 路由 %s 失败: %s", name, esp_err_to_name(reg_err));
        idf_logf("注册 HTTP 路由 %s 失败: %s", name, esp_err_to_name(reg_err));
        httpd_stop(s_server);
        s_server = nullptr;
        return reg_err;
    };

#define IDF_WEB_TRY_REGISTER(name, expr) do { \
        esp_err_t _reg_err = register_checked((name), (expr)); \
        if (_reg_err != ESP_OK) return _reg_err; \
    } while (0)

    IDF_WEB_TRY_REGISTER("/", httpd_register_uri_handler(s_server, &root));
    IDF_WEB_TRY_REGISTER("/tools", register_handler(s_server, "/tools", HTTP_GET, handle_root));
    IDF_WEB_TRY_REGISTER("/sms", register_handler(s_server, "/sms", HTTP_GET, handle_root));
    IDF_WEB_TRY_REGISTER("/assets/*", httpd_register_uri_handler(s_server, &assets));
    IDF_WEB_TRY_REGISTER("/ui", httpd_register_uri_handler(s_server, &ui));
    IDF_WEB_TRY_REGISTER("/status", httpd_register_uri_handler(s_server, &status));
    IDF_WEB_TRY_REGISTER("/config.json", httpd_register_uri_handler(s_server, &config_json));
    IDF_WEB_TRY_REGISTER("/save", httpd_register_uri_handler(s_server, &save));
    IDF_WEB_TRY_REGISTER("/wifi", httpd_register_uri_handler(s_server, &wifi));
    IDF_WEB_TRY_REGISTER("/wifiscan", httpd_register_uri_handler(s_server, &wifi_scan));
    IDF_WEB_TRY_REGISTER("/wificonfig", httpd_register_uri_handler(s_server, &wifi_config));
    IDF_WEB_TRY_REGISTER("/messages", httpd_register_uri_handler(s_server, &messages));
    IDF_WEB_TRY_REGISTER("/log", httpd_register_uri_handler(s_server, &log));
    IDF_WEB_TRY_REGISTER("/keepalive", httpd_register_uri_handler(s_server, &keepalive));
    IDF_WEB_TRY_REGISTER("/reboot", httpd_register_uri_handler(s_server, &reboot));
    IDF_WEB_TRY_REGISTER("/at", httpd_register_uri_handler(s_server, &pending_get));
    IDF_WEB_TRY_REGISTER("/ping", httpd_register_uri_handler(s_server, &pending_post));
    IDF_WEB_TRY_REGISTER("/testpush GET", register_handler(s_server, "/testpush", HTTP_GET, handle_test_push));
    IDF_WEB_TRY_REGISTER("/testpush POST", register_handler(s_server, "/testpush", HTTP_POST, handle_test_push));
    IDF_WEB_TRY_REGISTER("/ussd", register_handler(s_server, "/ussd", HTTP_GET, handle_ussd));
    IDF_WEB_TRY_REGISTER("/flight", register_handler(s_server, "/flight", HTTP_GET, handle_flight));
    IDF_WEB_TRY_REGISTER("/modem", register_handler(s_server, "/modem", HTTP_GET, handle_modem_control));
    IDF_WEB_TRY_REGISTER("/sendsms", register_handler(s_server, "/sendsms", HTTP_POST, handle_send_sms));
    IDF_WEB_TRY_REGISTER("/resend", register_handler(s_server, "/resend", HTTP_POST, handle_resend_message));
    IDF_WEB_TRY_REGISTER("/delete", register_handler(s_server, "/delete", HTTP_POST, handle_delete_message));
    IDF_WEB_TRY_REGISTER("/factory", register_handler(s_server, "/factory", HTTP_POST, handle_factory_reset));
    IDF_WEB_TRY_REGISTER("/import", register_handler(s_server, "/import", HTTP_POST, handle_import_config));
    IDF_WEB_TRY_REGISTER("/update", register_handler(s_server, "/update", HTTP_POST, handle_ota_update));
    IDF_WEB_TRY_REGISTER("/export", register_handler(s_server, "/export", HTTP_GET, handle_export_config));
    IDF_WEB_TRY_REGISTER("/logdownload", register_handler(s_server, "/logdownload", HTTP_GET, handle_log_download));
    IDF_WEB_TRY_REGISTER("/*", httpd_register_uri_handler(s_server, &not_found));

#undef IDF_WEB_TRY_REGISTER
    if (!s_scheduler_started) {
        BaseType_t ok = xTaskCreate(scheduler_task, "idf_sched", 4096, nullptr, 2, nullptr);
        if (ok == pdPASS) {
            s_scheduler_started = true;
            idf_log_line("定时任务 scheduler 已启动");
        } else {
            ESP_LOGW(TAG, "scheduler task start failed");
            idf_log_line("定时任务 scheduler 启动失败");
        }
    }
    ESP_LOGI(TAG, "ESP-IDF web server registered UI and bootstrap dynamic routes");
    idf_log_line("HTTP 服务器已启动");
    return ESP_OK;
}

void idf_web_stop(void)
{
    if (!s_server) return;
    httpd_stop(s_server);
    s_server = nullptr;
}
