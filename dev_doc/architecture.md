# ESP-IDF 架构

```text
main/app_main.cpp
  ├─ idf_config      NVS 配置、兼容旧 key、表单导入导出
  ├─ idf_wifi        STA/SoftAP、DNS 门户、mDNS、SNTP、重连
  ├─ idf_modem       UART1 单 owner、AT 串行化、蜂窝 HTTP/USSD/SMS
  ├─ idf_sms         PDU 收发、长短信、去重、补收、转发入队
  ├─ idf_push        推送/邮件 worker、重试、规则、测试推送
  ├─ idf_web         HTTP 路由、Web UI、OTA、状态、调度器
  ├─ idf_inbox       收件箱/已发送环形缓存
  ├─ idf_logbuf      120 行 Web 可见日志环
  ├─ idf_pdu         pdulib 原生组件
  └─ web_assets      gzip Web 静态资源
```

## 任务模型

- 模组 UART 只由 `idf_modem` owner task 访问。
- Web handler 不直接执行慢操作，长耗时工作通过后台任务/队列处理。
- 推送和邮件由 `idf_push` worker 串行发送，避免多个 TLS 会话同时吃堆。
- Web 侧 scheduler 负责定时保号、每日心跳、每日重启和低堆保护。

## 短信接收流

```text
UART URC / CMGL poll
  -> idf_sms PDU decode
  -> 长短信合并 / 去重 / 黑名单 / 管理员命令
  -> idf_inbox 入库
  -> idf_push forward queue
  -> 规则判定
  -> 推送队列 / 邮件队列
```

接收同时使用 `+CMT`/`+CMTI` 与周期性 `AT+CMGL` 兜底；去重保证双路径安全。

## Web UI

`code/web_src/` 是可编辑源，`tools/build_web_assets.py` 生成 `code/web_assets.*`。运行时由 `components/web_assets` 链接，`idf_web` 通过 gzip + ETag/304 服务页面。

## 配置

配置持久化在 NVS namespace `sms_config`。新增 key 应保持 additive default，不做破坏性迁移。WiFi 可通过 Web 配网，也可用本地未提交的 `code/wifi_config.h` 作为出厂 seed。
