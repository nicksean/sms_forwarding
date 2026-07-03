#include "idf_sms.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <algorithm>
#include <array>
#include <new>
#include <string>

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
#include "pdulib.h"

static constexpr size_t MAX_PDU_HEX_CHARS = 600;
static constexpr size_t INDEX_QUEUE_MAX = 8;
static constexpr size_t OUT_SMS_QUEUE_MAX = 3;
static constexpr size_t SEEN_RING_MAX = 32;
static constexpr size_t CONCAT_SLOTS = 5;
static constexpr size_t CONCAT_PARTS = 10;
static constexpr int64_t CONCAT_TIMEOUT_US = 30LL * 1000LL * 1000LL;
static constexpr uint32_t SMS_POLL_INTERVAL_MS = 60000;
static constexpr uint32_t SMS_STARTUP_POLL_INTERVAL_MS = 8000;
static constexpr uint32_t SMS_STARTUP_FAST_WINDOW_MS = 120000;
static constexpr uint8_t SMS_CNMI_REASSERT_EVERY = 5;

struct ConcatPart {
    bool valid = false;
    std::string text;
};

struct ConcatSlot {
    bool active = false;
    int ref = 0;
    int total = 0;
    int received = 0;
    std::string sender;
    std::string timestamp;
    int64_t lastUs = 0;
    std::array<ConcatPart, CONCAT_PARTS> parts;
};

struct OutgoingSmsJob {
    bool used = false;
    std::string phone;
    std::string text;
    int64_t queuedUs = 0;
};

static SemaphoreHandle_t s_status_mutex = nullptr;
static SemaphoreHandle_t s_pdu_mutex = nullptr;
static SemaphoreHandle_t s_out_mutex = nullptr;
static IdfSmsStatus s_status;
static bool s_started = false;
static PDU s_pdu(4096);
static std::array<int, INDEX_QUEUE_MAX> s_index_queue = {};
static size_t s_index_count = 0;
static std::array<OutgoingSmsJob, OUT_SMS_QUEUE_MAX> s_out_queue = {};
static size_t s_out_head = 0;
static size_t s_out_count = 0;
static std::array<uint32_t, SEEN_RING_MAX> s_seen = {};
static size_t s_seen_next = 0;
static size_t s_seen_filled = 0;
static std::array<ConcatSlot, CONCAT_SLOTS> s_concat = {};
static std::string s_urc_carry;
static bool s_wait_pdu = false;
static int64_t s_wait_pdu_until_us = 0;   // +CMT 后等 PDU 行的窗口截止(3s，对齐 Arduino)
static bool s_backfill_pending = false;   // 索引队列溢出/CMGR 失败时，请求一次近期 CMGL 兜底

static std::string trim(std::string value)
{
    size_t start = 0;
    while (start < value.size() && isspace(static_cast<unsigned char>(value[start]))) ++start;
    size_t end = value.size();
    while (end > start && isspace(static_cast<unsigned char>(value[end - 1]))) --end;
    return value.substr(start, end - start);
}

static bool starts_with(const std::string& text, const char* prefix)
{
    return text.rfind(prefix, 0) == 0;
}

static bool is_hex_string(const std::string& line)
{
    if (line.empty() || line.size() > MAX_PDU_HEX_CHARS || (line.size() & 1U)) return false;
    for (char ch : line) {
        if (!isxdigit(static_cast<unsigned char>(ch))) return false;
    }
    return true;
}

static uint32_t fnv1a32(const std::string& text)
{
    uint32_t h = 2166136261u;
    for (unsigned char ch : text) {
        h ^= ch;
        h *= 16777619u;
    }
    return h;
}

static bool seen_recently(uint32_t hash)
{
    // 用 filled 计数而不是排除 0 值：hash 恰为 0 的短信也要参与去重(对齐 Arduino)
    for (size_t i = 0; i < s_seen_filled; ++i) {
        if (s_seen[i] == hash) return true;
    }
    s_seen[s_seen_next] = hash;
    s_seen_next = (s_seen_next + 1) % SEEN_RING_MAX;
    if (s_seen_filled < SEEN_RING_MAX) ++s_seen_filled;
    return false;
}

// "YYYY-MM-DD HH:MM:SS" 本地时间；未同步返回空串(调用方退回 PDU 原始时间戳)
static std::string format_epoch_local(uint32_t epoch, int tz_offset_min)
{
    if (epoch < 1700000000u) return {};
    time_t shifted = static_cast<time_t>(epoch) + static_cast<time_t>(tz_offset_min) * 60;
    struct tm tmv = {};
    gmtime_r(&shifted, &tmv);
    char buf[40];
    snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d",
             tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
             tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
    return std::string(buf);
}

static std::string strip_country_code(const std::string& num)
{
    return num.rfind("+86", 0) == 0 ? num.substr(3) : num;
}

static bool number_blacklisted(const std::string& list, const std::string& sender)
{
    if (list.empty()) return false;
    std::string s1 = sender;
    std::string s2 = strip_country_code(sender);
    size_t pos = 0;
    while (pos <= list.size()) {
        size_t end = list.find('\n', pos);
        if (end == std::string::npos) end = list.size();
        std::string line = trim(list.substr(pos, end - pos));
        if (!line.empty() && (line == s1 || line == s2 || strip_country_code(line) == s2)) {
            return true;
        }
        if (end == list.size()) break;
        pos = end + 1;
    }
    return false;
}

static std::string mask_phone(const std::string& phone)
{
    size_t n = phone.size();
    if (n <= 4) return phone;
    size_t head = n >= 8 ? 3 : 1;
    size_t tail = n >= 8 ? 4 : 2;
    if (head + tail >= n) {
        head = n / 3;
        tail = n / 3;
    }
    return phone.substr(0, head) + "****" + phone.substr(n - tail);
}

static bool is_valid_phone_number(const std::string& phone)
{
    if (phone.size() < 3 || phone.size() > 20) return false;
    for (size_t i = 0; i < phone.size(); ++i) {
        char ch = phone[i];
        if (i == 0 && ch == '+') continue;
        if (!isdigit(static_cast<unsigned char>(ch))) return false;
    }
    return true;
}

static bool is_admin_sender(const std::string& sender, const IdfConfig& cfg)
{
    if (cfg.adminPhone.empty()) return false;
    return strip_country_code(sender) == strip_country_code(cfg.adminPhone);
}

struct AdminSmsTaskArg {
    std::string target;
    std::string content;
    std::string command;
};

static void admin_sms_task(void* raw)
{
    AdminSmsTaskArg* arg = static_cast<AdminSmsTaskArg*>(raw);
    vTaskDelay(pdMS_TO_TICKS(250));
    std::string send_message;
    esp_err_t err = idf_sms_send_text(arg->target, arg->content, send_message);
    bool ok = (err == ESP_OK);
    std::string subject = ok ? "短信发送成功" : "短信发送失败";
    std::string body = "管理员命令执行结果:\n命令: " + arg->command +
                       "\n目标号码: " + arg->target +
                       "\n短信内容: " + arg->content +
                       "\n执行结果: " + (ok ? "成功" : "失败") +
                       "\n详情: " + send_message;
    idf_push_enqueue_email(subject.c_str(), body.c_str());
    delete arg;
    vTaskDelete(nullptr);
}

static void admin_reset_task(void*)
{
    int64_t deadline = esp_timer_get_time() + 5LL * 1000LL * 1000LL;
    while ((idf_push_email_queue_depth() > 0 || idf_push_busy()) && esp_timer_get_time() < deadline) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    idf_modem_request_reset(true);
    vTaskDelay(pdMS_TO_TICKS(1500));
    idf_log_line("正在重启ESP32...");
    esp_restart();
}

static bool process_admin_command(const std::string& sender, const std::string& text)
{
    std::string cmd = trim(text);
    if (!(starts_with(cmd, "SMS:") || cmd == "RESET")) return false;

    idf_logf("处理管理员命令 from=%s", mask_phone(sender).c_str());
    if (starts_with(cmd, "SMS:")) {
        size_t first = cmd.find(':');
        size_t second = first == std::string::npos ? std::string::npos : cmd.find(':', first + 1);
        if (second == std::string::npos || second <= first + 1) {
            idf_log_line("SMS命令格式错误");
            idf_push_enqueue_email("命令执行失败", "SMS命令格式错误，正确格式: SMS:号码:内容");
            return true;
        }
        std::string target = trim(cmd.substr(first + 1, second - first - 1));
        std::string content = trim(cmd.substr(second + 1));
        idf_logf("管理员命令目标号码: %s", mask_phone(target).c_str());
        if (!is_valid_phone_number(target)) {
            idf_log_line("目标号码非法，拒绝执行");
            idf_push_enqueue_email("命令执行失败", "SMS命令目标号码非法（应为 3-20 位数字，可带 + 前缀）");
            return true;
        }
        if (content.empty() || content.size() > 300) {
            idf_log_line("短信内容为空或超长，拒绝执行");
            idf_push_enqueue_email("命令执行失败", "SMS命令内容为空或超过 300 字符");
            return true;
        }

        AdminSmsTaskArg* arg = new (std::nothrow) AdminSmsTaskArg();
        if (!arg) {
            idf_log_line("管理员短信任务创建失败：内存不足");
            idf_push_enqueue_email("命令执行失败", "管理员短信任务创建失败：内存不足");
            return true;
        }
        arg->target = target;
        arg->content = content;
        arg->command = cmd;
        if (xTaskCreate(admin_sms_task, "admin_sms", 4096, arg, 3, nullptr) != pdPASS) {
            delete arg;
            idf_log_line("管理员短信任务创建失败");
            idf_push_enqueue_email("命令执行失败", "管理员短信任务创建失败");
        }
        return true;
    }

    if (esp_timer_get_time() < 60LL * 1000LL * 1000LL) {
        idf_log_line("设备刚启动，忽略RESET命令（防重启风暴）");
        idf_push_enqueue_email("RESET已忽略", "设备启动不足60秒，已忽略RESET命令以防重启风暴。请稍后重试。");
        return true;
    }
    idf_log_line("执行RESET命令");
    idf_push_enqueue_email("重启命令已执行", "收到RESET命令，即将重启模组和ESP32...");
    if (xTaskCreate(admin_reset_task, "admin_reset", 3072, nullptr, 3, nullptr) != pdPASS) {
        idf_log_line("RESET任务创建失败，直接重启ESP32");
        esp_restart();
    }
    return true;
}

static void update_status(bool receive_ready, bool got_sms)
{
    if (!s_status_mutex) return;
    if (xSemaphoreTake(s_status_mutex, pdMS_TO_TICKS(100)) != pdTRUE) return;
    s_status.receiveReady = receive_ready || s_status.receiveReady;
    if (got_sms) {
        ++s_status.total;
        s_status.lastSmsEpoch = static_cast<uint32_t>(time(nullptr));
    }
    xSemaphoreGive(s_status_mutex);
}

static void enqueue_index(int idx)
{
    if (idx < 0) {
        s_backfill_pending = true;  // 非法索引：改由近期 CMGL 兜底(对齐 Arduino storedSmsPending)
        return;
    }
    for (size_t i = 0; i < s_index_count; ++i) {
        if (s_index_queue[i] == idx) return;
    }
    if (s_index_count < INDEX_QUEUE_MAX) {
        s_index_queue[s_index_count++] = idx;
    } else {
        s_backfill_pending = true;  // 队列满：丢索引但请求 CMGL 兜底，避免最长等 60s
    }
}

static bool pop_index(int& idx)
{
    if (s_index_count == 0) return false;
    idx = s_index_queue[0];
    for (size_t i = 1; i < s_index_count; ++i) s_index_queue[i - 1] = s_index_queue[i];
    --s_index_count;
    return true;
}

static int outgoing_depth_locked()
{
    return static_cast<int>(s_out_count);
}

static bool pop_outgoing_sms(OutgoingSmsJob& job)
{
    if (!s_out_mutex || xSemaphoreTake(s_out_mutex, pdMS_TO_TICKS(100)) != pdTRUE) return false;
    if (s_out_count == 0) {
        xSemaphoreGive(s_out_mutex);
        return false;
    }
    job = std::move(s_out_queue[s_out_head]);
    s_out_queue[s_out_head] = OutgoingSmsJob();
    s_out_head = (s_out_head + 1) % OUT_SMS_QUEUE_MAX;
    --s_out_count;
    xSemaphoreGive(s_out_mutex);
    return true;
}

static int parse_cmti_index(const std::string& line)
{
    size_t comma = line.rfind(',');
    if (comma == std::string::npos || comma + 1 >= line.size()) return -1;
    // 必须全为数字：atoi 对乱码返回 0，而 0 是合法 SIM 索引，会误读/误删真实短信
    std::string num = trim(line.substr(comma + 1));
    if (num.empty() || num.size() > 5) return -1;
    for (char ch : num) {
        if (!isdigit(static_cast<unsigned char>(ch))) return -1;
    }
    return atoi(num.c_str());
}

static int parse_cmgl_index(const std::string& line)
{
    const char* p = strchr(line.c_str(), ':');
    if (!p) return -1;
    while (*++p && isspace(static_cast<unsigned char>(*p))) {}
    return atoi(p);
}

static void clear_concat_slot(ConcatSlot& slot)
{
    slot.active = false;
    slot.ref = 0;
    slot.total = 0;
    slot.received = 0;
    slot.sender.clear();
    slot.timestamp.clear();
    slot.lastUs = 0;
    for (auto& part : slot.parts) {
        part.valid = false;
        part.text.clear();
    }
}

static std::string assemble_concat(const ConcatSlot& slot)
{
    std::string text;
    for (int i = 0; i < slot.total && i < static_cast<int>(CONCAT_PARTS); ++i) {
        if (slot.parts[i].valid) {
            text += slot.parts[i].text;
        } else {
            // 超时强制合并时标记缺口，收件人能看出内容不完整(对齐 Arduino)
            text += "[缺失分段";
            text += std::to_string(i + 1);
            text += "]";
        }
    }
    return text;
}

static ConcatSlot& find_concat_slot(int ref, const std::string& sender, int total)
{
    int64_t now = esp_timer_get_time();
    for (auto& slot : s_concat) {
        if (slot.active && slot.ref == ref && slot.sender == sender) return slot;
    }
    for (auto& slot : s_concat) {
        if (!slot.active || now - slot.lastUs > CONCAT_TIMEOUT_US) {
            clear_concat_slot(slot);
            slot.active = true;
            slot.ref = ref;
            slot.total = total;
            slot.sender = sender;
            slot.lastUs = now;
            return slot;
        }
    }
    auto oldest = std::min_element(s_concat.begin(), s_concat.end(),
        [](const ConcatSlot& a, const ConcatSlot& b) { return a.lastUs < b.lastUs; });
    clear_concat_slot(*oldest);
    oldest->active = true;
    oldest->ref = ref;
    oldest->total = total;
    oldest->sender = sender;
    oldest->lastUs = now;
    return *oldest;
}

static void process_sms_content(const char* sender_raw, const char* text_raw, const char* timestamp_raw)
{
    std::string sender = sender_raw ? sender_raw : "";
    std::string text = text_raw ? text_raw : "";
    std::string timestamp = timestamp_raw ? timestamp_raw : "";
    const IdfConfig cfg = idf_config_get();

    if (number_blacklisted(cfg.numberBlackList, sender)) {
        idf_logf("短信发件人 %s 在黑名单中，已忽略", mask_phone(sender).c_str());
        return;
    }

    // 去重键用 PDU 原始时间戳(双通道 URC+CMGL 收到的是同一原始值)；
    // 展示/转发用本地可读时间，未同步时才退回原始数字串
    uint32_t hash = fnv1a32(sender + "|" + timestamp + "|" + text);
    if (seen_recently(hash)) {
        idf_logf("重复短信 %s 已忽略", mask_phone(sender).c_str());
        return;
    }

    if (is_admin_sender(sender, cfg)) {
        idf_log_line("收到管理员短信，检查命令...");
        if (process_admin_command(sender, text)) {
            update_status(true, true);
            return;
        }
    }

    std::string display_ts = format_epoch_local(static_cast<uint32_t>(time(nullptr)), cfg.tzOffsetMin);
    if (display_ts.empty()) display_ts = timestamp;

    uint32_t id = idf_inbox_add(sender.c_str(), text.c_str(), display_ts.c_str());
    update_status(true, true);
    idf_logf("收到短信 id=%u from=%s len=%u，已入本地收件箱",
             static_cast<unsigned>(id), mask_phone(sender).c_str(), static_cast<unsigned>(text.size()));
    idf_push_enqueue_forward(sender.c_str(), text.c_str(), display_ts.c_str(), id);
}

static void handle_decoded_pdu()
{
    const char* sender = s_pdu.getSender();
    const char* text = s_pdu.getText();
    const char* ts = s_pdu.getTimeStamp();
    int* concat = s_pdu.getConcatInfo();
    int ref = concat ? concat[0] : 0;
    int part = concat ? concat[1] : 0;
    int total = concat ? concat[2] : 0;

    if (total > 1 && part > 0) {
        if (total > static_cast<int>(CONCAT_PARTS) || part > total) {
            idf_logf("长短信分段参数超限 part=%d total=%d，按单条处理", part, total);
            process_sms_content(sender, text, ts);
            return;
        }
        ConcatSlot& slot = find_concat_slot(ref, sender ? sender : "", total);
        int idx = part - 1;
        if (!slot.parts[idx].valid) {
            slot.parts[idx].valid = true;
            slot.parts[idx].text = text ? text : "";
            slot.received++;
            slot.lastUs = esp_timer_get_time();
            if (slot.timestamp.empty()) slot.timestamp = ts ? ts : "";
            idf_logf("收到长短信分段 ref=%d %d/%d", ref, part, total);
        }
        if (slot.received >= slot.total) {
            std::string full = assemble_concat(slot);
            process_sms_content(slot.sender.c_str(), full.c_str(), slot.timestamp.c_str());
            clear_concat_slot(slot);
        }
        return;
    }

    process_sms_content(sender, text, ts);
}

static bool decode_pdu_line(const std::string& line)
{
    if (!is_hex_string(line)) return false;
    if (!s_pdu_mutex || xSemaphoreTake(s_pdu_mutex, pdMS_TO_TICKS(2000)) != pdTRUE) {
        idf_log_line("PDU 解码器忙，短信暂未处理");
        return false;
    }
    if (!s_pdu.decodePDU(line.c_str())) {
        xSemaphoreGive(s_pdu_mutex);
        idf_log_line("PDU 解析失败");
        return false;
    }
    handle_decoded_pdu();
    xSemaphoreGive(s_pdu_mutex);
    return true;
}

static void expire_concat_slots()
{
    int64_t now = esp_timer_get_time();
    for (auto& slot : s_concat) {
        if (!slot.active || now - slot.lastUs <= CONCAT_TIMEOUT_US) continue;
        std::string full = assemble_concat(slot);
        if (!full.empty()) {
            idf_logf("长短信等待超时，已合并现有 %d/%d 段", slot.received, slot.total);
            process_sms_content(slot.sender.c_str(), full.c_str(), slot.timestamp.c_str());
        }
        clear_concat_slot(slot);
    }
}

static void process_urc_line(const std::string& raw)
{
    std::string line = trim(raw);
    if (line.empty()) return;

    if (s_wait_pdu) {
        if (starts_with(line, "+CMT:")) return;
        if (starts_with(line, "+CMTI:")) {
            enqueue_index(parse_cmti_index(line));
            return;
        }
        if (decode_pdu_line(line)) {
            s_wait_pdu = false;
            return;
        }
        if (line != "OK" && line != "ERROR") {
            idf_log_line("等待直推 PDU 时收到非 PDU 行，已关闭接收窗口");
            s_wait_pdu = false;
        }
        return;
    }

    if (starts_with(line, "+CMT:")) {
        s_wait_pdu = true;
        s_wait_pdu_until_us = esp_timer_get_time() + 3LL * 1000LL * 1000LL;  // 3s 窗口
    } else if (starts_with(line, "+CMTI:")) {
        enqueue_index(parse_cmti_index(line));
    }
}

// +CMT 后迟迟等不到 PDU 行时关闭窗口，避免后续无关的十六进制样 URC 被误当短信解码
static void expire_wait_pdu_window()
{
    if (s_wait_pdu && esp_timer_get_time() > s_wait_pdu_until_us) {
        s_wait_pdu = false;
        s_backfill_pending = true;  // 直推丢失的短信大概率还在存储里，请求兜底补收
    }
}

static void process_urc_text(const std::string& text)
{
    s_urc_carry += text;
    size_t pos = 0;
    while (true) {
        size_t nl = s_urc_carry.find('\n', pos);
        if (nl == std::string::npos) break;
        std::string line = s_urc_carry.substr(pos, nl - pos);
        process_urc_line(line);
        pos = nl + 1;
    }
    if (pos > 0) s_urc_carry.erase(0, pos);
    if (s_urc_carry.size() > MAX_PDU_HEX_CHARS + 128) {
        s_urc_carry.clear();
        s_wait_pdu = false;
    }
}

static bool decode_first_stored_pdu(const std::string& resp, const char* header)
{
    size_t h = resp.find(header);
    if (h == std::string::npos) return false;
    size_t pos = resp.find('\n', h);
    if (pos == std::string::npos) return false;
    ++pos;
    while (pos < resp.size()) {
        size_t nl = resp.find('\n', pos);
        if (nl == std::string::npos) nl = resp.size();
        std::string line = trim(resp.substr(pos, nl - pos));
        pos = nl + 1;
        if (line.empty() || line == "OK" || line == "ERROR" || starts_with(line, "+")) continue;
        return decode_pdu_line(line);
    }
    return false;
}

static void fetch_stored_sms_by_index(int idx)
{
    if (idx < 0) return;
    char cmd[24];
    snprintf(cmd, sizeof(cmd), "AT+CMGR=%d", idx);
    std::string resp;
    esp_err_t err = idf_modem_send_at(cmd, 3000, resp);
    bool has_header = resp.find("+CMGR:") != std::string::npos;
    bool decoded = decode_first_stored_pdu(resp, "+CMGR:");
    if (has_header) {
        if (!decoded) idf_logf("PDU 无法解析(索引=%d)，删除以释放 SIM 存储", idx);
        snprintf(cmd, sizeof(cmd), "AT+CMGD=%d", idx);
        std::string ignored;
        idf_modem_send_at(cmd, 2000, ignored);
    } else if (err != ESP_OK) {
        // 不重排队：空槽位(补收已删)/+CMS ERROR 会形成无限重试环，把收发队列全部饿死。
        // 改为请求一次 CMGL 兜底——真有短信它一定还在存储列表里。
        s_backfill_pending = true;
    }
}

static void backfill_stored_sms(bool announce)
{
    std::string resp;
    esp_err_t err = idf_modem_send_at("AT+CMGL=4", 4000, resp);
    if (err != ESP_OK && resp.find("+CMGL:") == std::string::npos) {
        if (announce) idf_logf("SIM 暂存短信读取失败: %s", esp_err_to_name(err));
        return;
    }

    // 每轮最多处理 5 条就归还控制权：处理间隙主循环能继续排空 URC、检查长短信超时、
    // 发送网页短信。剩余的通过 s_backfill_pending 在 ~500ms 后接着处理。
    constexpr int BATCH_MAX = 5;
    int processed = 0;
    int handled = 0;
    bool more_left = false;
    size_t pos = 0;
    while (pos < resp.size()) {
        size_t nl = resp.find('\n', pos);
        if (nl == std::string::npos) nl = resp.size();
        std::string line = trim(resp.substr(pos, nl - pos));
        pos = nl + 1;
        if (!starts_with(line, "+CMGL:")) continue;
        if (handled >= BATCH_MAX) {
            more_left = true;
            break;
        }

        int idx = parse_cmgl_index(line);
        std::string pdu_line;
        while (pos < resp.size()) {
            nl = resp.find('\n', pos);
            if (nl == std::string::npos) nl = resp.size();
            std::string candidate = trim(resp.substr(pos, nl - pos));
            if (starts_with(candidate, "+CMGL:")) break;  // PDU 缺失：别把下一条的头当 PDU 吞掉
            pos = nl + 1;
            if (!candidate.empty()) {
                pdu_line = candidate;
                break;
            }
        }

        bool decoded = decode_pdu_line(pdu_line);
        if (decoded) ++processed;
        if (idx >= 0) {
            if (!decoded) idf_logf("PDU 无法解析(索引=%d)，删除以释放 SIM 存储", idx);
            char cmd[24];
            snprintf(cmd, sizeof(cmd), "AT+CMGD=%d", idx);
            std::string ignored;
            idf_modem_send_at(cmd, 2000, ignored);
            ++handled;
        }
    }

    if (more_left) s_backfill_pending = true;
    if (processed > 0) idf_logf("SIM 暂存短信处理并删除 %d 条", processed);
}

static void sms_task(void*)
{
    uint32_t last_poll_ms = 0;
    uint32_t start_ms = static_cast<uint32_t>(esp_timer_get_time() / 1000ULL);
    uint32_t poll_count = 0;
    uint8_t reassert_step = 0;  // 1=CMGF, 2=CNMI；拆帧避免一次堆多条 AT
    bool backfill_after_reassert = false;
    bool configured = false;
    bool first_backfill = true;

    while (true) {
        std::string urc;
        if (idf_modem_take_urc(urc)) process_urc_text(urc);
        expire_wait_pdu_window();
        expire_concat_slots();

        IdfModemStatus modem = idf_modem_get_status();
        if (modem.atReady && !configured) {
            std::string ignored;
            idf_modem_send_at("AT+CMGF=0", 1200, ignored);
            idf_modem_send_at("AT+CNMI=2,1,0,0,0", 1200, ignored);
            configured = true;
            update_status(true, false);
            idf_log_line("短信接收(PDU/存储通知)已配置");
        }

        if (configured && modem.modemReady) {
            if (reassert_step != 0) {
                std::string ignored;
                if (reassert_step == 1) {
                    idf_modem_send_at("AT+CMGF=0", 1200, ignored);
                    reassert_step = 2;
                } else {
                    idf_modem_send_at("AT+CNMI=2,1,0,0,0", 1200, ignored);
                    reassert_step = 0;
                    backfill_after_reassert = true;
                }
                vTaskDelay(pdMS_TO_TICKS(120));
                continue;
            }

            if (backfill_after_reassert) {
                backfill_after_reassert = false;
                backfill_stored_sms(false);
                last_poll_ms = static_cast<uint32_t>(esp_timer_get_time() / 1000ULL);
                vTaskDelay(pdMS_TO_TICKS(200));
                continue;
            }

            int idx = -1;
            if (pop_index(idx)) {
                fetch_stored_sms_by_index(idx);
                vTaskDelay(pdMS_TO_TICKS(200));
                continue;
            }

            OutgoingSmsJob out;
            if (pop_outgoing_sms(out)) {
                std::string send_message;
                idf_logf("网页短信出队发送: %s len=%u",
                         mask_phone(out.phone).c_str(), static_cast<unsigned>(out.text.size()));
                idf_sms_send_text(out.phone, out.text, send_message);
                vTaskDelay(pdMS_TO_TICKS(200));
                continue;
            }

            if (s_backfill_pending) {
                s_backfill_pending = false;
                backfill_stored_sms(false);
                last_poll_ms = static_cast<uint32_t>(esp_timer_get_time() / 1000ULL);
                vTaskDelay(pdMS_TO_TICKS(200));
                continue;
            }

            uint32_t now_ms = static_cast<uint32_t>(esp_timer_get_time() / 1000ULL);
            uint32_t interval = (now_ms - start_ms < SMS_STARTUP_FAST_WINDOW_MS)
                ? SMS_STARTUP_POLL_INTERVAL_MS
                : SMS_POLL_INTERVAL_MS;
            if (first_backfill || now_ms - last_poll_ms >= interval) {
                if (!first_backfill && (poll_count % SMS_CNMI_REASSERT_EVERY) == 0) {
                    reassert_step = 1;
                    ++poll_count;
                    last_poll_ms = now_ms;
                    continue;
                }
                backfill_stored_sms(first_backfill);
                first_backfill = false;
                last_poll_ms = now_ms;
                ++poll_count;
            }
        }

        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

esp_err_t idf_sms_start(void)
{
    if (s_started) return ESP_OK;
    s_status_mutex = xSemaphoreCreateMutex();
    s_pdu_mutex = xSemaphoreCreateMutex();
    s_out_mutex = xSemaphoreCreateMutex();
    if (!s_status_mutex || !s_pdu_mutex || !s_out_mutex) return ESP_ERR_NO_MEM;
    BaseType_t ok = xTaskCreate(sms_task, "idf_sms", 8192, nullptr, 3, nullptr);
    if (ok != pdPASS) return ESP_ERR_NO_MEM;
    s_started = true;
    return ESP_OK;
}

esp_err_t idf_sms_send_text(const std::string& phone_raw, const std::string& text_raw, std::string& message)
{
    message.clear();
    std::string phone = trim(phone_raw);
    std::string text = trim(text_raw);
    if (!is_valid_phone_number(phone) || text.empty() || text.size() > 300) {
        message = "号码或内容无效";
        return ESP_ERR_INVALID_ARG;
    }
    IdfModemStatus modem = idf_modem_get_status();
    if (!modem.modemReady) {
        message = "模组尚未注册网络";
        return ESP_ERR_INVALID_STATE;
    }
    if (!s_pdu_mutex || xSemaphoreTake(s_pdu_mutex, pdMS_TO_TICKS(3000)) != pdTRUE) {
        message = "PDU 编码器忙";
        return ESP_ERR_TIMEOUT;
    }
    s_pdu.setSCAnumber();
    int pdu_len = s_pdu.encodePDU(phone.c_str(), text.c_str());
    std::string sms_pdu;
    if (pdu_len >= 0) sms_pdu.assign(s_pdu.getSMS(), strlen(s_pdu.getSMS()));
    xSemaphoreGive(s_pdu_mutex);

    if (pdu_len < 0) {
        char buf[64];
        snprintf(buf, sizeof(buf), "PDU 编码失败(%d)", pdu_len);
        message = buf;
        idf_sent_add(phone.c_str(), text.c_str(), false);
        return ESP_FAIL;
    }

    char cmd[32];
    snprintf(cmd, sizeof(cmd), "AT+CMGS=%d", pdu_len);
    std::string resp;
    esp_err_t err = idf_modem_send_pdu(cmd, sms_pdu.c_str(), 20000, resp);
    bool ok = (err == ESP_OK);
    idf_sent_add(phone.c_str(), text.c_str(), ok);
    if (ok) {
        message = "短信发送成功";
        idf_logf("网页发送短信成功: %s len=%u", mask_phone(phone).c_str(), static_cast<unsigned>(text.size()));
    } else {
        message = resp.empty() ? std::string(esp_err_to_name(err)) : trim(resp);
        idf_logf("网页发送短信失败: %s", message.c_str());
    }
    return err;
}

esp_err_t idf_sms_enqueue_outgoing(const std::string& phone_raw, const std::string& text_raw, std::string& message)
{
    std::string phone = trim(phone_raw);
    std::string text = trim(text_raw);
    message.clear();
    if (phone.empty()) {
        message = "错误：请输入目标号码";
        return ESP_ERR_INVALID_ARG;
    }
    if (!is_valid_phone_number(phone)) {
        message = "错误：目标号码非法（3-20 位数字，可带 + 前缀）";
        return ESP_ERR_INVALID_ARG;
    }
    if (text.empty()) {
        message = "错误：请输入短信内容";
        return ESP_ERR_INVALID_ARG;
    }
    if (text.size() > 300) {
        message = "错误：短信内容超过 300 字符";
        return ESP_ERR_INVALID_SIZE;
    }
    if (!s_out_mutex || xSemaphoreTake(s_out_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        message = "发送队列繁忙，请稍后再试";
        return ESP_ERR_TIMEOUT;
    }
    if (s_out_count >= OUT_SMS_QUEUE_MAX) {
        xSemaphoreGive(s_out_mutex);
        message = "发送队列已满，请稍后再试";
        return ESP_ERR_NO_MEM;
    }

    size_t tail = (s_out_head + s_out_count) % OUT_SMS_QUEUE_MAX;
    s_out_queue[tail].used = true;
    s_out_queue[tail].phone = phone;
    s_out_queue[tail].text = text;
    s_out_queue[tail].queuedUs = esp_timer_get_time();
    ++s_out_count;
    int depth = outgoing_depth_locked();
    xSemaphoreGive(s_out_mutex);

    idf_logf("网页短信已入队，当前待发=%d", depth);
    message = "已加入发送队列，请稍后在已发送列表查看结果";
    return ESP_OK;
}

int idf_sms_outgoing_queue_depth(void)
{
    if (!s_out_mutex || xSemaphoreTake(s_out_mutex, pdMS_TO_TICKS(100)) != pdTRUE) return 0;
    int n = outgoing_depth_locked();
    xSemaphoreGive(s_out_mutex);
    return n;
}

IdfSmsStatus idf_sms_get_status(void)
{
    IdfSmsStatus copy;
    if (!s_status_mutex) return copy;
    if (xSemaphoreTake(s_status_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        copy = s_status;
        xSemaphoreGive(s_status_mutex);
    }
    return copy;
}
