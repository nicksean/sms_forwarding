#pragma once

#include <string>

#include "esp_err.h"
#include "idf_config.h"

struct IdfWifiStatus {
    bool staConnected = false;
    bool apMode = false;
    int rssi = 0;
    int channel = 0;
    std::string ssid;
    std::string ip;
    std::string gw;
    std::string mask;
    std::string dns;
    std::string mac;
    std::string bssid;
    std::string apSsid;
    std::string apIp;
};

esp_err_t idf_wifi_start(const IdfConfig& config);
esp_err_t idf_wifi_reconnect(void);
esp_err_t idf_wifi_scan_json(std::string& out_json);
IdfWifiStatus idf_wifi_get_status(void);
