#pragma once

#include <stdint.h>

#include <string>
#include <utility>
#include <vector>

#include "esp_err.h"

static constexpr int IDF_MAX_PUSH_CHANNELS = 5;
static constexpr const char* IDF_FW_VERSION = "1.0.3";
static constexpr const char* IDF_DEFAULT_WEB_USER = "admin";
static constexpr const char* IDF_DEFAULT_WEB_PASS = "admin123";
static constexpr const char* IDF_KEEPALIVE_DEFAULT_URL = "http://gg.incrafttime.top/api/payload?size=64342";

struct IdfPushChannel {
    bool enabled = false;
    uint8_t type = 1;
    std::string name;
    std::string url;
    std::string key1;
    std::string key2;
    std::string customBody;
};

struct IdfConfig {
    std::string wifiSsid;
    std::string wifiPass;
    bool wifiFromFallback = false;

    std::string smtpServer;
    int smtpPort = 465;
    std::string smtpUser;
    std::string smtpPass;
    std::string smtpSendTo;
    std::string adminPhone;
    bool emailEnabled = true;
    bool pushEnabled = true;

    std::string webUser = IDF_DEFAULT_WEB_USER;
    std::string webPass = IDF_DEFAULT_WEB_PASS;
    std::string numberBlackList;
    std::string forwardRules;

    bool kaEnabled = false;
    int kaIntervalDays = 175;
    uint8_t kaAction = 1;
    std::string kaTarget;
    std::string kaUrl = IDF_KEEPALIVE_DEFAULT_URL;
    uint32_t kaLastTime = 0;

    int tzOffsetMin = 480;
    std::string ntpServer = "ntp.aliyun.com";
    bool rebootEnabled = false;
    int rebootHour = 4;
    bool hbEnabled = false;
    int hbHour = 9;

    bool dataEnabled = false;
    std::string apn;
    std::string operatorPlmn;
    std::string phoneNumber;

    IdfPushChannel pushChannels[IDF_MAX_PUSH_CHANNELS];
};

using IdfFormFields = std::vector<std::pair<std::string, std::string>>;

esp_err_t idf_config_load(void);
esp_err_t idf_config_save(void);
esp_err_t idf_config_save_wifi(const std::string& ssid, const std::string& pass);
esp_err_t idf_config_update_from_form(const IdfFormFields& fields);
std::string idf_config_export_text(void);
esp_err_t idf_config_import_text(const std::string& text, int* applied_count);
esp_err_t idf_config_factory_reset(void);
esp_err_t idf_config_set_keepalive_last(uint32_t epoch);

IdfConfig idf_config_get(void);
bool idf_config_has_sta_credentials(void);
int idf_config_enabled_push_count(void);
bool idf_config_email_configured(void);
// 锁内直接比对 Web 凭据，避免每个 HTTP 请求做一次全量配置深拷贝
bool idf_config_check_web_auth(const char* user, const char* pass);
