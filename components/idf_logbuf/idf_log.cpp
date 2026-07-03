#include "idf_log.h"

#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <array>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static constexpr size_t LOG_RING_SIZE = 120;
static constexpr size_t LOG_LINE_MAX = 192;

static SemaphoreHandle_t s_log_mutex = nullptr;
static std::array<std::string, LOG_RING_SIZE> s_lines;
static uint32_t s_seq = 0;
static size_t s_count = 0;
static size_t s_next = 0;

static void ensure_init()
{
    if (!s_log_mutex) s_log_mutex = xSemaphoreCreateMutex();
}

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

void idf_log_init(void)
{
    ensure_init();
}

void idf_log_line(const char* line)
{
    ensure_init();
    if (!s_log_mutex || !line) return;

    std::string item(line);
    if (item.size() > LOG_LINE_MAX) {
        size_t end = LOG_LINE_MAX - 3;
        // 退到 UTF-8 字符边界：日志多为中文(3 字节/字)，硬截会产生乱码
        while (end > 0 && (static_cast<unsigned char>(item[end]) & 0xC0) == 0x80) --end;
        item.resize(end);
        item += "...";
    }

    if (xSemaphoreTake(s_log_mutex, pdMS_TO_TICKS(500)) != pdTRUE) return;
    s_lines[s_next] = std::move(item);
    s_next = (s_next + 1) % LOG_RING_SIZE;
    if (s_count < LOG_RING_SIZE) ++s_count;
    ++s_seq;
    xSemaphoreGive(s_log_mutex);
}

void idf_logf(const char* fmt, ...)
{
    char buf[LOG_LINE_MAX + 1];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    idf_log_line(buf);
}

std::string idf_log_json_since(uint32_t since)
{
    ensure_init();
    std::string out;
    out.reserve(2048);

    if (!s_log_mutex || xSemaphoreTake(s_log_mutex, pdMS_TO_TICKS(500)) != pdTRUE) {
        return "{\"seq\":0,\"lines\":[]}";
    }

    uint32_t seq = s_seq;
    size_t count = s_count;
    size_t start = (s_next + LOG_RING_SIZE - count) % LOG_RING_SIZE;
    uint32_t oldest = seq >= count ? seq - static_cast<uint32_t>(count) + 1 : 1;
    // 客户端游标比当前序号还大 = 设备已重启、序号从头计。从 0 重放，
    // 否则网页要等新日志追上旧游标才恢复显示
    if (since > seq) since = 0;

    char head[48];
    snprintf(head, sizeof(head), "{\"seq\":%" PRIu32 ",\"lines\":[", seq);
    out += head;
    bool first = true;
    for (size_t i = 0; i < count; ++i) {
        uint32_t line_seq = oldest + static_cast<uint32_t>(i);
        if (line_seq <= since) continue;
        if (!first) out += ",";
        first = false;
        out += "\"";
        json_escape_append(out, s_lines[(start + i) % LOG_RING_SIZE]);
        out += "\"";
    }
    out += "]}";
    xSemaphoreGive(s_log_mutex);
    return out;
}

std::string idf_log_text_dump(void)
{
    ensure_init();
    std::string out;
    out.reserve(4096);
    if (!s_log_mutex || xSemaphoreTake(s_log_mutex, pdMS_TO_TICKS(500)) != pdTRUE) return out;

    size_t count = s_count;
    size_t start = (s_next + LOG_RING_SIZE - count) % LOG_RING_SIZE;
    for (size_t i = 0; i < count; ++i) {
        out += s_lines[(start + i) % LOG_RING_SIZE];
        out += "\r\n";
    }
    xSemaphoreGive(s_log_mutex);
    return out;
}
