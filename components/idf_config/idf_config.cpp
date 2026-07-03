#include "idf_config.h"

#include <algorithm>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "idf_log.h"
#include "nvs.h"
#include "nvs_flash.h"

#if IDF_HAS_WIFI_CONFIG_H
#include "wifi_config.h"
#else
#define WIFI_SSID ""
#define WIFI_PASS ""
#endif

static const char* TAG = "idf_config";
static IdfConfig s_config;
static SemaphoreHandle_t s_config_mutex = nullptr;

static esp_err_t save_config_to_nvs(const IdfConfig& c);

static esp_err_t ensure_config_mutex()
{
    if (s_config_mutex) return ESP_OK;
    s_config_mutex = xSemaphoreCreateMutex();
    return s_config_mutex ? ESP_OK : ESP_ERR_NO_MEM;
}

static IdfConfig config_snapshot()
{
    if (ensure_config_mutex() != ESP_OK) return IdfConfig();
    xSemaphoreTake(s_config_mutex, portMAX_DELAY);
    IdfConfig copy = s_config;
    xSemaphoreGive(s_config_mutex);
    return copy;
}

static esp_err_t replace_config(const IdfConfig& next)
{
    esp_err_t err = ensure_config_mutex();
    if (err != ESP_OK) return err;
    xSemaphoreTake(s_config_mutex, portMAX_DELAY);
    s_config = next;
    xSemaphoreGive(s_config_mutex);
    return ESP_OK;
}

static std::string channel_default_name(int idx)
{
    char buf[20];
    snprintf(buf, sizeof(buf), "通道%d", idx + 1);
    return std::string(buf);
}

static bool is_blank(const std::string& value)
{
    return value.find_first_not_of(" \t\r\n") == std::string::npos;
}

static std::string read_str(nvs_handle_t nvs, const char* key, const char* fallback, size_t max_len = 1024)
{
    size_t len = 0;
    esp_err_t err = nvs_get_str(nvs, key, nullptr, &len);
    if (err == ESP_ERR_NVS_NOT_FOUND) return fallback ? std::string(fallback) : std::string();
    if (err != ESP_OK || len == 0) {
        if (err != ESP_OK) ESP_LOGW(TAG, "读取 NVS 字符串 %s 失败: %s", key, esp_err_to_name(err));
        return fallback ? std::string(fallback) : std::string();
    }
    std::string value(len, '\0');
    err = nvs_get_str(nvs, key, value.data(), &len);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "读取 NVS 字符串 %s 内容失败: %s", key, esp_err_to_name(err));
        return fallback ? std::string(fallback) : std::string();
    }
    if (!value.empty() && value.back() == '\0') value.pop_back();
    if (value.size() > max_len) {
        // 截断保留而不是丢弃：Arduino 版对字符串没有长度上限，OTA 迁移过来的
        // 超长规则/黑名单若被静默清空，下次保存还会把 NVS 里完好的原值覆盖掉
        size_t end = max_len;
        while (end > 0 && (static_cast<unsigned char>(value[end]) & 0xC0) == 0x80) --end;
        value.resize(end);
        ESP_LOGE(TAG, "NVS 字符串 %s 超长，已截断保留前 %u 字节", key, static_cast<unsigned>(end));
        idf_logf("配置项 %s 超长，已截断保留(建议在网页里检查该项)", key);
    }
    return value;
}

static int32_t read_i32(nvs_handle_t nvs, const char* key, int32_t fallback)
{
    int32_t value = fallback;
    esp_err_t err = nvs_get_i32(nvs, key, &value);
    return err == ESP_OK ? value : fallback;
}

static uint32_t read_u32(nvs_handle_t nvs, const char* key, uint32_t fallback)
{
    uint32_t value = fallback;
    esp_err_t err = nvs_get_u32(nvs, key, &value);
    return err == ESP_OK ? value : fallback;
}

static uint8_t read_u8(nvs_handle_t nvs, const char* key, uint8_t fallback)
{
    uint8_t value = fallback;
    esp_err_t err = nvs_get_u8(nvs, key, &value);
    return err == ESP_OK ? value : fallback;
}

static bool read_bool(nvs_handle_t nvs, const char* key, bool fallback)
{
    return read_u8(nvs, key, fallback ? 1 : 0) != 0;
}

static esp_err_t write_str(nvs_handle_t nvs, const char* key, const std::string& value)
{
    return nvs_set_str(nvs, key, value.c_str());
}

static const std::string* find_field(const IdfFormFields& fields, const char* key)
{
    for (const auto& field : fields) {
        if (field.first == key) return &field.second;
    }
    return nullptr;
}

static bool has_field(const IdfFormFields& fields, const char* key)
{
    return find_field(fields, key) != nullptr;
}

static int to_int(const std::string* value, int fallback)
{
    if (!value) return fallback;
    char* end = nullptr;
    long parsed = strtol(value->c_str(), &end, 10);
    return end != value->c_str() ? static_cast<int>(parsed) : fallback;
}

static uint8_t to_u8(const std::string* value, uint8_t fallback)
{
    int parsed = to_int(value, fallback);
    if (parsed < 0) return fallback;
    if (parsed > 255) return fallback;
    return static_cast<uint8_t>(parsed);
}

static int clamp_int(int value, int lo, int hi)
{
    return value < lo ? lo : (value > hi ? hi : value);
}

static std::string trim_copy(const std::string& value)
{
    size_t start = value.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return {};
    size_t end = value.find_last_not_of(" \t\r\n");
    return value.substr(start, end - start + 1);
}

static bool looks_like_url(const std::string& value)
{
    return value.rfind("http://", 0) == 0 || value.rfind("https://", 0) == 0;
}

static std::string cfg_escape(const std::string& value)
{
    std::string out;
    out.reserve(value.size() + 8);
    for (char ch : value) {
        if (ch == '\\') out += "\\\\";
        else if (ch == '\n') out += "\\n";
        else if (ch == '\r') out += "\\r";
        else out += ch;
    }
    return out;
}

static std::string cfg_unescape(const std::string& value)
{
    std::string out;
    out.reserve(value.size());
    for (size_t i = 0; i < value.size(); ++i) {
        char ch = value[i];
        if (ch == '\\' && i + 1 < value.size()) {
            char next = value[i + 1];
            if (next == 'n') { out += '\n'; ++i; }
            else if (next == 'r') { out += '\r'; ++i; }
            else if (next == '\\') { out += '\\'; ++i; }
            else out += ch;
        } else {
            out += ch;
        }
    }
    return out;
}

static bool bool_from_text(const std::string& value)
{
    return value == "1" || value == "true" || value == "on" || value == "yes";
}

static void append_kv(std::string& out, const char* key, const std::string& value)
{
    out += key;
    out += "=";
    out += cfg_escape(value);
    out += "\n";
}

static void append_kv_i(std::string& out, const char* key, int value)
{
    char buf[32];
    snprintf(buf, sizeof(buf), "%d", value);
    append_kv(out, key, buf);
}

esp_err_t idf_config_load(void)
{
    esp_err_t mutex_err = ensure_config_mutex();
    if (mutex_err != ESP_OK) return mutex_err;

    IdfConfig next;

    nvs_handle_t nvs = 0;
    esp_err_t err = nvs_open("sms_config", NVS_READONLY, &nvs);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "NVS 配置不存在，使用默认值");
        idf_log_line("NVS 配置不存在，使用默认值");
    } else if (err != ESP_OK) {
        ESP_LOGE(TAG, "打开 NVS 配置失败: %s", esp_err_to_name(err));
        idf_logf("打开 NVS 配置失败: %s", esp_err_to_name(err));
        return err;
    }

    if (err == ESP_OK) {
        next.wifiSsid = read_str(nvs, "wifiSsid", "", 64);
        next.wifiPass = read_str(nvs, "wifiPass", "", 128);
        next.smtpServer = read_str(nvs, "smtpServer", "", 128);
        next.smtpPort = read_i32(nvs, "smtpPort", 465);
        next.smtpUser = read_str(nvs, "smtpUser", "", 128);
        next.smtpPass = read_str(nvs, "smtpPass", "", 256);
        next.smtpSendTo = read_str(nvs, "smtpSendTo", "", 256);
        next.adminPhone = read_str(nvs, "adminPhone", "", 64);
        next.webUser = read_str(nvs, "webUser", IDF_DEFAULT_WEB_USER, 64);
        next.webPass = read_str(nvs, "webPass", IDF_DEFAULT_WEB_PASS, 128);
        next.numberBlackList = read_str(nvs, "numBlkList", "", 1024);
        next.forwardRules = read_str(nvs, "fwdRules", "", 2048);
        next.emailEnabled = read_bool(nvs, "emailEn", true);
        next.pushEnabled = read_bool(nvs, "pushEn", true);

        next.kaEnabled = read_bool(nvs, "kaEn", false);
        next.kaIntervalDays = read_i32(nvs, "kaDays", 175);
        next.kaAction = read_u8(nvs, "kaAct", 1);
        next.kaTarget = read_str(nvs, "kaTarget", "", 64);
        next.kaUrl = read_str(nvs, "kaUrl", IDF_KEEPALIVE_DEFAULT_URL, 256);
        next.kaLastTime = read_u32(nvs, "kaLast", 0);

        next.tzOffsetMin = read_i32(nvs, "tzMin", 480);
        next.ntpServer = read_str(nvs, "ntpSrv", "ntp.aliyun.com", 128);
        next.rebootEnabled = read_bool(nvs, "rbEn", false);
        next.rebootHour = read_i32(nvs, "rbHour", 4);
        next.hbEnabled = read_bool(nvs, "hbEn", false);
        next.hbHour = read_i32(nvs, "hbHour", 9);

        next.dataEnabled = read_bool(nvs, "dataEn", false);
        next.apn = read_str(nvs, "apn", "", 96);
        next.operatorPlmn = read_str(nvs, "opPlmn", "", 16);
        next.phoneNumber = read_str(nvs, "phoneNum", "", 64);

        for (int i = 0; i < IDF_MAX_PUSH_CHANNELS; ++i) {
            char prefix[12];
            snprintf(prefix, sizeof(prefix), "push%d", i);
            auto key = [&](const char* suffix) {
                char buf[24];
                snprintf(buf, sizeof(buf), "%s%s", prefix, suffix);
                return std::string(buf);
            };
            IdfPushChannel& ch = next.pushChannels[i];
            ch.enabled = read_bool(nvs, key("en").c_str(), false);
            ch.type = read_u8(nvs, key("type").c_str(), 1);
            ch.url = read_str(nvs, key("url").c_str(), "", 512);
            ch.name = read_str(nvs, key("name").c_str(), channel_default_name(i).c_str(), 64);
            ch.key1 = read_str(nvs, key("k1").c_str(), "", 256);
            ch.key2 = read_str(nvs, key("k2").c_str(), "", 512);
            ch.customBody = read_str(nvs, key("body").c_str(), "", 1024);
        }

        // 旧版(多通道之前)单通道配置一次性迁移：httpUrl/barkMode → 通道1
        if (!next.pushChannels[0].enabled && next.pushChannels[0].url.empty()) {
            std::string legacy_url = read_str(nvs, "httpUrl", "", 512);
            if (!legacy_url.empty()) {
                next.pushChannels[0].enabled = true;
                next.pushChannels[0].type = read_bool(nvs, "barkMode", false) ? 2 : 1;  // 2=Bark 1=POST JSON
                next.pushChannels[0].url = legacy_url;
                idf_log_line("已迁移旧版单通道推送配置到通道1");
            }
        }
        nvs_close(nvs);
    }

    if (is_blank(next.webUser)) next.webUser = IDF_DEFAULT_WEB_USER;
    if (is_blank(next.webPass)) next.webPass = IDF_DEFAULT_WEB_PASS;
    if (next.kaUrl.empty()) next.kaUrl = IDF_KEEPALIVE_DEFAULT_URL;

    if (next.wifiSsid.empty() && WIFI_SSID[0]) {
        next.wifiSsid = WIFI_SSID;
        next.wifiPass = WIFI_PASS;
        next.wifiFromFallback = true;
    }

    std::string log_wifi = next.wifiSsid;
    bool log_fallback = next.wifiFromFallback;
    std::string log_web_user = next.webUser;
    esp_err_t set_err = replace_config(next);
    if (set_err != ESP_OK) return set_err;
    ESP_LOGI(TAG, "配置已加载: wifi=%s%s, webUser=%s",
             log_wifi.empty() ? "(none)" : log_wifi.c_str(),
             log_fallback ? " (fallback)" : "",
             log_web_user.c_str());
    idf_logf("配置已加载: wifi=%s%s, webUser=%s",
             log_wifi.empty() ? "(none)" : log_wifi.c_str(),
             log_fallback ? " (fallback)" : "",
             log_web_user.c_str());
    return ESP_OK;
}

std::string idf_config_export_text(void)
{
    IdfConfig c = idf_config_get();
    std::string out;
    out.reserve(4096);

    append_kv(out, "wifiSsid", c.wifiSsid);
    append_kv(out, "wifiPass", c.wifiPass);
    append_kv(out, "smtpServer", c.smtpServer);
    append_kv_i(out, "smtpPort", c.smtpPort);
    append_kv(out, "smtpUser", c.smtpUser);
    append_kv(out, "smtpPass", c.smtpPass);
    append_kv(out, "smtpSendTo", c.smtpSendTo);
    append_kv(out, "adminPhone", c.adminPhone);
    append_kv(out, "webUser", c.webUser);
    append_kv(out, "webPass", c.webPass);
    append_kv(out, "numBlkList", c.numberBlackList);
    append_kv(out, "fwdRules", c.forwardRules);
    append_kv_i(out, "emailEnabled", c.emailEnabled ? 1 : 0);
    append_kv_i(out, "pushEnabled", c.pushEnabled ? 1 : 0);

    append_kv_i(out, "tzOffsetMin", c.tzOffsetMin);
    append_kv(out, "ntpServer", c.ntpServer);
    append_kv_i(out, "rebootEnabled", c.rebootEnabled ? 1 : 0);
    append_kv_i(out, "rebootHour", c.rebootHour);
    append_kv_i(out, "hbEnabled", c.hbEnabled ? 1 : 0);
    append_kv_i(out, "hbHour", c.hbHour);

    append_kv_i(out, "kaEnabled", c.kaEnabled ? 1 : 0);
    append_kv_i(out, "kaIntervalDays", c.kaIntervalDays);
    append_kv_i(out, "kaAction", c.kaAction);
    append_kv(out, "kaTarget", c.kaTarget);
    append_kv(out, "kaUrl", c.kaUrl);
    append_kv_i(out, "kaLastTime", static_cast<int>(c.kaLastTime));

    append_kv_i(out, "dataEnabled", c.dataEnabled ? 1 : 0);
    append_kv(out, "apn", c.apn);
    append_kv(out, "operatorPlmn", c.operatorPlmn);
    append_kv(out, "phoneNumber", c.phoneNumber);

    for (int i = 0; i < IDF_MAX_PUSH_CHANNELS; ++i) {
        char key[24];
        const IdfPushChannel& ch = c.pushChannels[i];
        snprintf(key, sizeof(key), "push%den", i);
        append_kv_i(out, key, ch.enabled ? 1 : 0);
        snprintf(key, sizeof(key), "push%dtype", i);
        append_kv_i(out, key, ch.type);
        snprintf(key, sizeof(key), "push%durl", i);
        append_kv(out, key, ch.url);
        snprintf(key, sizeof(key), "push%dname", i);
        append_kv(out, key, ch.name);
        snprintf(key, sizeof(key), "push%dk1", i);
        append_kv(out, key, ch.key1);
        snprintf(key, sizeof(key), "push%dk2", i);
        append_kv(out, key, ch.key2);
        snprintf(key, sizeof(key), "push%dbody", i);
        append_kv(out, key, ch.customBody);
    }
    return out;
}

static void apply_import_key(IdfConfig& c, const std::string& key, const std::string& value)
{
    if (key == "wifiSsid") c.wifiSsid = value;
    else if (key == "wifiPass") c.wifiPass = value;
    else if (key == "smtpServer") c.smtpServer = value;
    else if (key == "smtpPort") c.smtpPort = std::max(1, atoi(value.c_str()));
    else if (key == "smtpUser") c.smtpUser = value;
    else if (key == "smtpPass") c.smtpPass = value;
    else if (key == "smtpSendTo") c.smtpSendTo = value;
    else if (key == "adminPhone") c.adminPhone = value;
    else if (key == "webUser" && !is_blank(value)) c.webUser = value;
    else if (key == "webPass" && !is_blank(value)) c.webPass = value;
    else if (key == "numBlkList" || key == "numberBlackList") c.numberBlackList = value;
    else if (key == "fwdRules" || key == "forwardRules") c.forwardRules = value;
    else if (key == "emailEnabled") c.emailEnabled = bool_from_text(value);
    else if (key == "pushEnabled") c.pushEnabled = bool_from_text(value);
    else if (key == "tzOffsetMin") c.tzOffsetMin = atoi(value.c_str());
    else if (key == "ntpServer") c.ntpServer = value;
    else if (key == "rebootEnabled") c.rebootEnabled = bool_from_text(value);
    else if (key == "rebootHour") c.rebootHour = atoi(value.c_str());
    else if (key == "hbEnabled") c.hbEnabled = bool_from_text(value);
    else if (key == "hbHour") c.hbHour = atoi(value.c_str());
    else if (key == "kaEnabled") c.kaEnabled = bool_from_text(value);
    else if (key == "kaIntervalDays") c.kaIntervalDays = atoi(value.c_str());
    else if (key == "kaAction") c.kaAction = static_cast<uint8_t>(atoi(value.c_str()));
    else if (key == "kaTarget") c.kaTarget = value;
    else if (key == "kaUrl") c.kaUrl = value.empty() ? IDF_KEEPALIVE_DEFAULT_URL : value;
    else if (key == "kaLastTime") c.kaLastTime = static_cast<uint32_t>(strtoul(value.c_str(), nullptr, 10));
    else if (key == "dataEnabled") c.dataEnabled = bool_from_text(value);
    else if (key == "apn") c.apn = value;
    else if (key == "operatorPlmn") c.operatorPlmn = value;
    else if (key == "phoneNumber") c.phoneNumber = value;
    else if (key.rfind("push", 0) == 0) {
        size_t pos = 4;
        int idx = 0;
        bool has_digit = false;
        while (pos < key.size() && isdigit(static_cast<unsigned char>(key[pos]))) {
            has_digit = true;
            idx = idx * 10 + (key[pos] - '0');
            ++pos;
        }
        if (!has_digit || idx < 0 || idx >= IDF_MAX_PUSH_CHANNELS) return;
        std::string suffix = key.substr(pos);
        IdfPushChannel& ch = c.pushChannels[idx];
        if (suffix == "en") ch.enabled = bool_from_text(value);
        else if (suffix == "type") ch.type = static_cast<uint8_t>(atoi(value.c_str()));
        else if (suffix == "url") ch.url = value;
        else if (suffix == "name") ch.name = value.empty() ? channel_default_name(idx) : value;
        else if (suffix == "k1") ch.key1 = value;
        else if (suffix == "k2") ch.key2 = value;
        else if (suffix == "body") ch.customBody = value;
    }
}

esp_err_t idf_config_import_text(const std::string& text, int* applied_count)
{
    if (text.empty()) return ESP_ERR_INVALID_ARG;
    IdfConfig next = idf_config_get();
    int applied = 0;
    size_t pos = 0;
    while (pos <= text.size()) {
        size_t end = text.find('\n', pos);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(pos, end - pos);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        size_t eq = line.find('=');
        if (eq != std::string::npos && eq > 0) {
            std::string key = line.substr(0, eq);
            std::string value = cfg_unescape(line.substr(eq + 1));
            key.erase(0, key.find_first_not_of(" \t\r\n"));
            size_t keep = key.find_last_not_of(" \t\r\n");
            if (keep != std::string::npos) key.erase(keep + 1);
            if (!key.empty()) {
                apply_import_key(next, key, value);
                ++applied;
            }
        }
        if (end == text.size()) break;
        pos = end + 1;
    }

    if (is_blank(next.webUser)) next.webUser = IDF_DEFAULT_WEB_USER;
    if (is_blank(next.webPass)) next.webPass = IDF_DEFAULT_WEB_PASS;
    if (next.kaUrl.empty()) next.kaUrl = IDF_KEEPALIVE_DEFAULT_URL;
    next.wifiFromFallback = false;

    esp_err_t err = replace_config(next);
    if (err == ESP_OK) err = save_config_to_nvs(next);
    if (err == ESP_OK && applied_count) *applied_count = applied;
    return err;
}

esp_err_t idf_config_factory_reset(void)
{
    nvs_handle_t nvs = 0;
    esp_err_t err = nvs_open("sms_config", NVS_READWRITE, &nvs);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return replace_config(IdfConfig());
    }
    if (err != ESP_OK) return err;
    err = nvs_erase_all(nvs);
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);
    if (err == ESP_OK) {
        esp_err_t set_err = replace_config(IdfConfig());
        if (set_err != ESP_OK) return set_err;
        idf_log_line("配置已恢复出厂设置");
    }
    return err;
}

esp_err_t idf_config_set_keepalive_last(uint32_t epoch)
{
    // 只改这一个字段并只写这一个 NVS key：
    // 1) 快照-改-回写会和并发的 /save 互相覆盖(丢失更新)；2) 全量 45 键回写浪费 NVS 寿命
    esp_err_t err = ensure_config_mutex();
    if (err != ESP_OK) return err;
    xSemaphoreTake(s_config_mutex, portMAX_DELAY);
    s_config.kaLastTime = epoch;
    xSemaphoreGive(s_config_mutex);

    nvs_handle_t nvs = 0;
    err = nvs_open("sms_config", NVS_READWRITE, &nvs);
    if (err != ESP_OK) return err;
    err = nvs_set_u32(nvs, "kaLast", epoch);
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);
    return err;
}

static esp_err_t save_config_to_nvs(const IdfConfig& c)
{
    nvs_handle_t nvs = 0;
    esp_err_t err = nvs_open("sms_config", NVS_READWRITE, &nvs);
    if (err != ESP_OK) return err;

    err = ESP_OK;
    if (err == ESP_OK) err = write_str(nvs, "wifiSsid", c.wifiSsid);
    if (err == ESP_OK) err = write_str(nvs, "wifiPass", c.wifiPass);
    if (err == ESP_OK) err = write_str(nvs, "smtpServer", c.smtpServer);
    if (err == ESP_OK) err = nvs_set_i32(nvs, "smtpPort", c.smtpPort);
    if (err == ESP_OK) err = write_str(nvs, "smtpUser", c.smtpUser);
    if (err == ESP_OK) err = write_str(nvs, "smtpPass", c.smtpPass);
    if (err == ESP_OK) err = write_str(nvs, "smtpSendTo", c.smtpSendTo);
    if (err == ESP_OK) err = write_str(nvs, "adminPhone", c.adminPhone);
    if (err == ESP_OK) err = write_str(nvs, "webUser", c.webUser);
    if (err == ESP_OK) err = write_str(nvs, "webPass", c.webPass);
    if (err == ESP_OK) err = write_str(nvs, "numBlkList", c.numberBlackList);
    if (err == ESP_OK) err = write_str(nvs, "fwdRules", c.forwardRules);
    if (err == ESP_OK) err = nvs_set_u8(nvs, "emailEn", c.emailEnabled ? 1 : 0);
    if (err == ESP_OK) err = nvs_set_u8(nvs, "pushEn", c.pushEnabled ? 1 : 0);

    if (err == ESP_OK) err = nvs_set_u8(nvs, "kaEn", c.kaEnabled ? 1 : 0);
    if (err == ESP_OK) err = nvs_set_i32(nvs, "kaDays", c.kaIntervalDays);
    if (err == ESP_OK) err = nvs_set_u8(nvs, "kaAct", c.kaAction);
    if (err == ESP_OK) err = write_str(nvs, "kaTarget", c.kaTarget);
    if (err == ESP_OK) err = write_str(nvs, "kaUrl", c.kaUrl.empty() ? IDF_KEEPALIVE_DEFAULT_URL : c.kaUrl);
    if (err == ESP_OK) err = nvs_set_u32(nvs, "kaLast", c.kaLastTime);

    if (err == ESP_OK) err = nvs_set_i32(nvs, "tzMin", c.tzOffsetMin);
    if (err == ESP_OK) err = write_str(nvs, "ntpSrv", c.ntpServer);
    if (err == ESP_OK) err = nvs_set_u8(nvs, "rbEn", c.rebootEnabled ? 1 : 0);
    if (err == ESP_OK) err = nvs_set_i32(nvs, "rbHour", c.rebootHour);
    if (err == ESP_OK) err = nvs_set_u8(nvs, "hbEn", c.hbEnabled ? 1 : 0);
    if (err == ESP_OK) err = nvs_set_i32(nvs, "hbHour", c.hbHour);

    if (err == ESP_OK) err = nvs_set_u8(nvs, "dataEn", c.dataEnabled ? 1 : 0);
    if (err == ESP_OK) err = write_str(nvs, "apn", c.apn);
    if (err == ESP_OK) err = write_str(nvs, "opPlmn", c.operatorPlmn);
    if (err == ESP_OK) err = write_str(nvs, "phoneNum", c.phoneNumber);

    for (int i = 0; err == ESP_OK && i < IDF_MAX_PUSH_CHANNELS; ++i) {
        char prefix[12];
        snprintf(prefix, sizeof(prefix), "push%d", i);
        auto key = [&](const char* suffix) {
            char buf[24];
            snprintf(buf, sizeof(buf), "%s%s", prefix, suffix);
            return std::string(buf);
        };
        const IdfPushChannel& ch = c.pushChannels[i];
        if (err == ESP_OK) err = nvs_set_u8(nvs, key("en").c_str(), ch.enabled ? 1 : 0);
        if (err == ESP_OK) err = nvs_set_u8(nvs, key("type").c_str(), ch.type);
        if (err == ESP_OK) err = write_str(nvs, key("url").c_str(), ch.url);
        if (err == ESP_OK) err = write_str(nvs, key("name").c_str(), ch.name);
        if (err == ESP_OK) err = write_str(nvs, key("k1").c_str(), ch.key1);
        if (err == ESP_OK) err = write_str(nvs, key("k2").c_str(), ch.key2);
        if (err == ESP_OK) err = write_str(nvs, key("body").c_str(), ch.customBody);
    }

    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "保存配置失败: %s", esp_err_to_name(err));
        idf_logf("保存配置失败: %s", esp_err_to_name(err));
    } else {
        idf_log_line("配置已保存");
    }
    return err;
}

esp_err_t idf_config_save(void)
{
    return save_config_to_nvs(idf_config_get());
}

esp_err_t idf_config_save_wifi(const std::string& ssid, const std::string& pass)
{
    if (ssid.empty() || ssid.size() > 32 || pass.size() > 64) return ESP_ERR_INVALID_ARG;
    IdfConfig next = idf_config_get();
    next.wifiSsid = ssid;
    next.wifiPass = pass;
    next.wifiFromFallback = false;
    esp_err_t err = replace_config(next);
    if (err != ESP_OK) return err;
    return save_config_to_nvs(next);
}

esp_err_t idf_config_update_from_form(const IdfFormFields& fields)
{
    IdfConfig next = idf_config_get();

    if (const std::string* v = find_field(fields, "webUser"); v && !is_blank(*v)) next.webUser = *v;
    if (const std::string* v = find_field(fields, "webPass"); v && !is_blank(*v)) next.webPass = *v;

    if (const std::string* v = find_field(fields, "smtpServer")) next.smtpServer = trim_copy(*v);
    {
        int port = to_int(find_field(fields, "smtpPort"), next.smtpPort);
        next.smtpPort = (port > 0 && port <= 65535) ? port : 465;  // 0/非法回落默认 SSL 端口
    }
    if (const std::string* v = find_field(fields, "smtpUser")) next.smtpUser = *v;
    if (const std::string* v = find_field(fields, "smtpPass")) next.smtpPass = *v;
    if (const std::string* v = find_field(fields, "smtpSendTo")) next.smtpSendTo = *v;
    if (has_field(fields, "emailForm")) next.emailEnabled = has_field(fields, "emailEnabled");

    if (const std::string* v = find_field(fields, "adminPhone")) next.adminPhone = *v;
    if (const std::string* v = find_field(fields, "numberBlackList")) next.numberBlackList = *v;
    if (const std::string* v = find_field(fields, "forwardRules")) next.forwardRules = *v;

    if (has_field(fields, "pushForm")) {
        next.pushEnabled = has_field(fields, "pushEnabled");
        for (int i = 0; i < IDF_MAX_PUSH_CHANNELS; ++i) {
            char key[24];
            snprintf(key, sizeof(key), "push%den", i);
            next.pushChannels[i].enabled = has_field(fields, key);
            snprintf(key, sizeof(key), "push%dtype", i);
            uint8_t type = to_u8(find_field(fields, key), next.pushChannels[i].type);
            next.pushChannels[i].type = (type >= 1 && type <= 10) ? type : 1;  // 越界回落 POST JSON
            snprintf(key, sizeof(key), "push%durl", i);
            if (const std::string* v = find_field(fields, key)) next.pushChannels[i].url = trim_copy(*v);
            snprintf(key, sizeof(key), "push%dname", i);
            if (const std::string* v = find_field(fields, key)) {
                std::string name = trim_copy(*v);
                next.pushChannels[i].name = name.empty() ? channel_default_name(i) : name;
            }
            snprintf(key, sizeof(key), "push%dkey1", i);
            if (const std::string* v = find_field(fields, key)) next.pushChannels[i].key1 = trim_copy(*v);
            snprintf(key, sizeof(key), "push%dkey2", i);
            if (const std::string* v = find_field(fields, key)) next.pushChannels[i].key2 = trim_copy(*v);
            snprintf(key, sizeof(key), "push%dbody", i);
            if (const std::string* v = find_field(fields, key)) next.pushChannels[i].customBody = *v;

            // Server酱常见误填：把 SendKey 粘到了 URL 框——自动搬到 key1
            IdfPushChannel& ch = next.pushChannels[i];
            // Bark 常见误填：只把 Device Key 粘到了 URL 框——自动搬到 key1，URL 留空走官方服务
            if (ch.type == 2 && ch.key1.empty() && !ch.url.empty() && !looks_like_url(ch.url)) {
                ch.key1 = ch.url;
                ch.url.clear();
            }
            if (ch.type == 6 && ch.key1.empty() && !ch.url.empty() && !looks_like_url(ch.url)) {
                ch.key1 = ch.url;
                ch.url.clear();
            }
        }
    }

    if (has_field(fields, "kaForm")) {
        next.kaEnabled = has_field(fields, "kaEnabled");
        // 0/负数会让保号"每小时都到期"，反复发短信/USSD/跑流量——只接受 1~3650 天
        int ka_days = to_int(find_field(fields, "kaIntervalDays"), next.kaIntervalDays);
        if (ka_days >= 1 && ka_days <= 3650) next.kaIntervalDays = ka_days;
        next.kaAction = to_u8(find_field(fields, "kaAction"), next.kaAction);
        if (const std::string* v = find_field(fields, "kaTarget")) next.kaTarget = *v;
        if (const std::string* v = find_field(fields, "kaUrl")) next.kaUrl = v->empty() ? IDF_KEEPALIVE_DEFAULT_URL : *v;
    }

    if (has_field(fields, "tzForm")) {
        next.tzOffsetMin = to_int(find_field(fields, "tzOffsetMin"), next.tzOffsetMin);
        if (next.tzOffsetMin < -720) next.tzOffsetMin = -720;
        if (next.tzOffsetMin > 840) next.tzOffsetMin = 840;
        if (const std::string* v = find_field(fields, "ntpServer")) next.ntpServer = *v;
    }

    if (has_field(fields, "schedForm")) {
        next.rebootEnabled = has_field(fields, "rebootEnabled");
        next.rebootHour = clamp_int(to_int(find_field(fields, "rebootHour"), next.rebootHour), 0, 23);
        next.hbEnabled = has_field(fields, "hbEnabled");
        next.hbHour = clamp_int(to_int(find_field(fields, "hbHour"), next.hbHour), 0, 23);
    }

    if (has_field(fields, "simForm")) {
        next.dataEnabled = has_field(fields, "dataEnabled");
        if (const std::string* v = find_field(fields, "apn")) next.apn = *v;
        if (const std::string* v = find_field(fields, "operatorPlmn")) next.operatorPlmn = *v;
        if (const std::string* v = find_field(fields, "phoneNumber")) next.phoneNumber = *v;
    }

    esp_err_t err = replace_config(next);
    if (err != ESP_OK) return err;
    return save_config_to_nvs(next);
}

IdfConfig idf_config_get(void)
{
    return config_snapshot();
}

// 以下布尔/小字段访问器都在锁内直接求值：全量快照要深拷贝 42 个 std::string，
// 在每个 HTTP 请求上都做一次会造成持续的堆分配抖动与碎片化
bool idf_config_has_sta_credentials(void)
{
    if (ensure_config_mutex() != ESP_OK) return false;
    xSemaphoreTake(s_config_mutex, portMAX_DELAY);
    bool ok = !s_config.wifiSsid.empty();
    xSemaphoreGive(s_config_mutex);
    return ok;
}

bool idf_config_email_configured(void)
{
    if (ensure_config_mutex() != ESP_OK) return false;
    xSemaphoreTake(s_config_mutex, portMAX_DELAY);
    bool ok = !s_config.smtpServer.empty() && !s_config.smtpUser.empty() &&
              !s_config.smtpPass.empty() && !s_config.smtpSendTo.empty();
    xSemaphoreGive(s_config_mutex);
    return ok;
}

int idf_config_enabled_push_count(void)
{
    if (ensure_config_mutex() != ESP_OK) return 0;
    xSemaphoreTake(s_config_mutex, portMAX_DELAY);
    int count = 0;
    for (const auto& ch : s_config.pushChannels) {
        if (ch.enabled) ++count;
    }
    xSemaphoreGive(s_config_mutex);
    return count;
}

bool idf_config_check_web_auth(const char* user, const char* pass)
{
    if (!user || !pass) return false;
    if (ensure_config_mutex() != ESP_OK) return false;
    xSemaphoreTake(s_config_mutex, portMAX_DELAY);
    bool ok = (s_config.webUser == user) && (s_config.webPass == pass);
    xSemaphoreGive(s_config_mutex);
    return ok;
}
