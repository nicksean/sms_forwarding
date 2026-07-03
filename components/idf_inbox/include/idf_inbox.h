#pragma once

#include <stddef.h>
#include <stdint.h>

#include <string>

struct IdfInboxEntry {
    uint32_t id = 0;
    uint32_t recvEpoch = 0;
    std::string sender;
    std::string ts;
    std::string text;
    bool forwarded = false;
};

struct IdfSentEntry {
    uint32_t id = 0;
    uint32_t sentEpoch = 0;
    std::string target;
    std::string text;
    bool ok = false;
};

void idf_inbox_init(void);
uint32_t idf_inbox_add(const char* sender, const char* text, const char* ts);
void idf_inbox_mark_forwarded(uint32_t id);
size_t idf_inbox_count(void);
bool idf_inbox_get_newest(size_t index, IdfInboxEntry& out);
bool idf_inbox_get_by_id(uint32_t id, IdfInboxEntry& out);
bool idf_inbox_delete(uint32_t id);

void idf_sent_add(const char* target, const char* text, bool ok);
size_t idf_sent_count(void);
bool idf_sent_get_newest(size_t index, IdfSentEntry& out);

std::string idf_inbox_json(bool sent_box, int limit);
