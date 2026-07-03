#pragma once

#include <stdint.h>

#include <string>

#include "esp_err.h"
#include "idf_config.h"

struct IdfModemStatus {
    bool started = false;
    bool atReady = false;
    bool modemReady = false;
    bool signalFresh = false;
    bool identityFresh = false;
    std::string phase = "off";
    int ceregStat = -1;
    int csq = -1;
    int ber = 99;
    int rsrp = 999;
    int rsrq = 999;
    int sinr = 999;
    std::string mfr;
    std::string model;
    std::string fwver;
    std::string imei;
    std::string iccid;
    std::string imsi;
    std::string operatorName;
    std::string apnSim;
    std::string cellIp;
    std::string phone;
};

struct IdfCellularHttpResult {
    bool ok = false;
    int httpStatus = -1;
    uint32_t bytesRead = 0;
    uint32_t expectedBytes = 0;
    int mhttpError = 0;
    std::string cellIp;
    std::string message;
};

esp_err_t idf_modem_start(const IdfConfig& config);
esp_err_t idf_modem_send_at(const std::string& cmd, uint32_t timeout_ms, std::string& response);
esp_err_t idf_modem_send_at_until(const std::string& cmd, const char* token, uint32_t timeout_ms, std::string& response);
esp_err_t idf_modem_send_pdu(const std::string& cmgs_cmd, const char* pdu, uint32_t timeout_ms, std::string& response);
esp_err_t idf_modem_cellular_http_get(const std::string& url, const IdfConfig& config, IdfCellularHttpResult& result);
esp_err_t idf_modem_request_reset(bool hard_reset);
bool idf_modem_take_urc(std::string& out);
IdfModemStatus idf_modem_get_status(void);
// AT 通道当前是否空闲（Web 路由用于"模组正忙"快速返回，避免长时间阻塞 httpd 任务）
bool idf_modem_at_idle(void);
