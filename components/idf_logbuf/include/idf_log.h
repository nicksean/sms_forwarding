#pragma once

#include <stdint.h>

#include <string>

void idf_log_init(void);
void idf_log_line(const char* line);
void idf_logf(const char* fmt, ...);
std::string idf_log_json_since(uint32_t since);
std::string idf_log_text_dump(void);

// 上次运行日志：开机时只捕获 noinit RAM 镜像，异常复位后延迟保存到
// smsdata NVS 分区；Flash 里的兜底日志只在用户请求 prev dump 时读取。
bool idf_log_has_prev(void);
std::string idf_log_prev_dump(void);
