#include "idf_inbox.h"

#include <stdio.h>
#include <time.h>

#include <array>
#include <algorithm>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

// 收件箱只是网页速览缓存，完整内容已转发到推送/邮件。
// 50×1024B 的旧配额最坏 ~73KB 堆，会挤掉 TLS 握手所需的 ~40KB 连续内存。
static constexpr size_t INBOX_MAX = 50;
static constexpr size_t SENT_MAX = 10;
static constexpr size_t BODY_MAX = 320;

struct InboxSlot : IdfInboxEntry {
    bool deleted = false;
};

static SemaphoreHandle_t s_mutex = nullptr;
static std::array<InboxSlot, INBOX_MAX> s_inbox;
static std::array<IdfSentEntry, SENT_MAX> s_sent;
static size_t s_inbox_head = 0;
static size_t s_inbox_filled = 0;
static uint32_t s_inbox_seq = 0;
static size_t s_sent_head = 0;
static size_t s_sent_filled = 0;
static uint32_t s_sent_seq = 0;

static void ensure_init()
{
    if (!s_mutex) s_mutex = xSemaphoreCreateMutex();
}

static std::string truncate_body(const char* text)
{
    std::string out = text ? text : "";
    if (out.size() > BODY_MAX) {
        size_t end = BODY_MAX;
        // 退到 UTF-8 字符边界，避免截出半个汉字变成乱码
        while (end > 0 && (static_cast<unsigned char>(out[end]) & 0xC0) == 0x80) --end;
        out.resize(end);
        out += "...";
    }
    return out;
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

static void json_prop(std::string& out, const char* key, const std::string& value)
{
    out += "\"";
    out += key;
    out += "\":\"";
    json_escape_append(out, value);
    out += "\"";
}

void idf_inbox_init(void)
{
    ensure_init();
}

uint32_t idf_inbox_add(const char* sender, const char* text, const char* ts)
{
    ensure_init();
    if (!s_mutex || xSemaphoreTake(s_mutex, pdMS_TO_TICKS(500)) != pdTRUE) return 0;

    InboxSlot& e = s_inbox[s_inbox_head];
    e.id = ++s_inbox_seq;
    e.recvEpoch = static_cast<uint32_t>(time(nullptr));
    e.sender = sender ? sender : "";
    e.ts = ts ? ts : "";
    e.text = truncate_body(text);
    e.forwarded = false;
    e.deleted = false;

    s_inbox_head = (s_inbox_head + 1) % INBOX_MAX;
    if (s_inbox_filled < INBOX_MAX) ++s_inbox_filled;
    uint32_t id = e.id;
    xSemaphoreGive(s_mutex);
    return id;
}

void idf_inbox_mark_forwarded(uint32_t id)
{
    if (id == 0) return;
    ensure_init();
    if (!s_mutex || xSemaphoreTake(s_mutex, pdMS_TO_TICKS(500)) != pdTRUE) return;
    for (size_t i = 0; i < s_inbox_filled; ++i) {
        if (s_inbox[i].id == id && !s_inbox[i].deleted) {
            s_inbox[i].forwarded = true;
            break;
        }
    }
    xSemaphoreGive(s_mutex);
}

size_t idf_inbox_count(void)
{
    ensure_init();
    if (!s_mutex || xSemaphoreTake(s_mutex, pdMS_TO_TICKS(500)) != pdTRUE) return 0;
    size_t count = 0;
    for (size_t i = 0; i < s_inbox_filled; ++i) {
        if (!s_inbox[i].deleted) ++count;
    }
    xSemaphoreGive(s_mutex);
    return count;
}

bool idf_inbox_get_newest(size_t index, IdfInboxEntry& out)
{
    ensure_init();
    if (!s_mutex || xSemaphoreTake(s_mutex, pdMS_TO_TICKS(500)) != pdTRUE) return false;
    size_t seen = 0;
    bool ok = false;
    for (size_t k = 0; k < s_inbox_filled; ++k) {
        size_t idx = (s_inbox_head + INBOX_MAX - 1 - k) % INBOX_MAX;
        if (s_inbox[idx].deleted) continue;
        if (seen == index) {
            out.id = s_inbox[idx].id;
            out.recvEpoch = s_inbox[idx].recvEpoch;
            out.sender = s_inbox[idx].sender;
            out.ts = s_inbox[idx].ts;
            out.text = s_inbox[idx].text;
            out.forwarded = s_inbox[idx].forwarded;
            ok = true;
            break;
        }
        ++seen;
    }
    xSemaphoreGive(s_mutex);
    return ok;
}

bool idf_inbox_get_by_id(uint32_t id, IdfInboxEntry& out)
{
    if (id == 0) return false;
    ensure_init();
    if (!s_mutex || xSemaphoreTake(s_mutex, pdMS_TO_TICKS(500)) != pdTRUE) return false;
    bool ok = false;
    for (size_t i = 0; i < s_inbox_filled; ++i) {
        if (s_inbox[i].id == id && !s_inbox[i].deleted) {
            out.id = s_inbox[i].id;
            out.recvEpoch = s_inbox[i].recvEpoch;
            out.sender = s_inbox[i].sender;
            out.ts = s_inbox[i].ts;
            out.text = s_inbox[i].text;
            out.forwarded = s_inbox[i].forwarded;
            ok = true;
            break;
        }
    }
    xSemaphoreGive(s_mutex);
    return ok;
}

bool idf_inbox_delete(uint32_t id)
{
    if (id == 0) return false;
    ensure_init();
    if (!s_mutex || xSemaphoreTake(s_mutex, pdMS_TO_TICKS(500)) != pdTRUE) return false;
    bool ok = false;
    for (size_t i = 0; i < s_inbox_filled; ++i) {
        if (s_inbox[i].id == id && !s_inbox[i].deleted) {
            s_inbox[i].deleted = true;
            s_inbox[i].sender.clear();
            s_inbox[i].ts.clear();
            s_inbox[i].text.clear();
            ok = true;
            break;
        }
    }
    xSemaphoreGive(s_mutex);
    return ok;
}

void idf_sent_add(const char* target, const char* text, bool ok)
{
    ensure_init();
    if (!s_mutex || xSemaphoreTake(s_mutex, pdMS_TO_TICKS(500)) != pdTRUE) return;

    IdfSentEntry& e = s_sent[s_sent_head];
    e.id = ++s_sent_seq;
    e.sentEpoch = static_cast<uint32_t>(time(nullptr));
    e.target = target ? target : "";
    e.text = truncate_body(text);
    e.ok = ok;

    s_sent_head = (s_sent_head + 1) % SENT_MAX;
    if (s_sent_filled < SENT_MAX) ++s_sent_filled;
    xSemaphoreGive(s_mutex);
}

size_t idf_sent_count(void)
{
    ensure_init();
    if (!s_mutex || xSemaphoreTake(s_mutex, pdMS_TO_TICKS(500)) != pdTRUE) return 0;
    size_t count = s_sent_filled;
    xSemaphoreGive(s_mutex);
    return count;
}

bool idf_sent_get_newest(size_t index, IdfSentEntry& out)
{
    ensure_init();
    if (!s_mutex || xSemaphoreTake(s_mutex, pdMS_TO_TICKS(500)) != pdTRUE) return false;
    if (index >= s_sent_filled) {
        xSemaphoreGive(s_mutex);
        return false;
    }
    size_t idx = (s_sent_head + SENT_MAX - 1 - index) % SENT_MAX;
    out = s_sent[idx];
    xSemaphoreGive(s_mutex);
    return true;
}

std::string idf_inbox_json(bool sent_box, int limit)
{
    // 单次持锁直接遍历环形缓冲：避免"取数量、再逐条取"期间新短信入库导致的
    // 条目错位/重复，也把 51 次锁往返 + O(n²) 扫描降为一次 O(n)
    ensure_init();
    if (limit < 0) limit = 0;

    std::string out;
    out += "[";
    if (!s_mutex || xSemaphoreTake(s_mutex, pdMS_TO_TICKS(500)) != pdTRUE) {
        out += "]";
        return out;
    }

    size_t emitted = 0;
    char buf[80];
    if (sent_box) {
        out.reserve(64 + std::min<size_t>(s_sent_filled, limit ? limit : s_sent_filled) * 160);
        for (size_t k = 0; k < s_sent_filled; ++k) {
            if (limit > 0 && emitted >= static_cast<size_t>(limit)) break;
            size_t idx = (s_sent_head + SENT_MAX - 1 - k) % SENT_MAX;
            const IdfSentEntry& e = s_sent[idx];
            if (emitted) out += ",";
            snprintf(buf, sizeof(buf), "{\"id\":%u,\"sent\":%u,",
                     static_cast<unsigned>(e.id), static_cast<unsigned>(e.sentEpoch));
            out += buf;
            json_prop(out, "target", e.target); out += ",";
            json_prop(out, "text", e.text); out += ",";
            out += "\"ok\":";
            out += e.ok ? "true" : "false";
            out += "}";
            ++emitted;
        }
    } else {
        out.reserve(64 + std::min<size_t>(s_inbox_filled, limit ? limit : s_inbox_filled) * 200);
        for (size_t k = 0; k < s_inbox_filled; ++k) {
            if (limit > 0 && emitted >= static_cast<size_t>(limit)) break;
            size_t idx = (s_inbox_head + INBOX_MAX - 1 - k) % INBOX_MAX;
            const InboxSlot& e = s_inbox[idx];
            if (e.deleted) continue;
            if (emitted) out += ",";
            snprintf(buf, sizeof(buf), "{\"id\":%u,\"recv\":%u,",
                     static_cast<unsigned>(e.id), static_cast<unsigned>(e.recvEpoch));
            out += buf;
            json_prop(out, "sender", e.sender); out += ",";
            json_prop(out, "ts", e.ts); out += ",";
            json_prop(out, "text", e.text); out += ",";
            out += "\"fwd\":";
            out += e.forwarded ? "true" : "false";
            out += "}";
            ++emitted;
        }
    }
    xSemaphoreGive(s_mutex);
    out += "]";
    return out;
}
