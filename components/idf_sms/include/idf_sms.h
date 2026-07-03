#pragma once

#include <stdint.h>

#include <string>

#include "esp_err.h"

struct IdfSmsStatus {
    uint32_t total = 0;
    uint32_t lastSmsEpoch = 0;
    bool receiveReady = false;
};

esp_err_t idf_sms_start(void);
esp_err_t idf_sms_send_text(const std::string& phone, const std::string& text, std::string& message);
esp_err_t idf_sms_enqueue_outgoing(const std::string& phone, const std::string& text, std::string& message);
int idf_sms_outgoing_queue_depth(void);
IdfSmsStatus idf_sms_get_status(void);
