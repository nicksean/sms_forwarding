#pragma once

#include <stdint.h>

#include <string>

void idf_log_init(void);
void idf_log_line(const char* line);
void idf_logf(const char* fmt, ...);
std::string idf_log_json_since(uint32_t since);
std::string idf_log_text_dump(void);
