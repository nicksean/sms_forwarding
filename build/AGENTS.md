# Build Directory Rules

本目录是本项目唯一允许使用的固件编译输出目录。

- Arduino 编译必须使用 `--build-path E:\GitHub-Repo\sms_forwarding\build\arduino`。
- 当前固件保留 OTA 时使用 `esp32:esp32:esp32c3:CDCOnBoot=cdc,PartitionScheme=min_spiffs`；`makergo_c3_supermini` 默认 1.2MB APP 分区会放不下。
- ESP-IDF 编译必须使用 `idf.py -B "E:\GitHub-Repo\sms_forwarding\build\idf" -D "SDKCONFIG=E:\GitHub-Repo\sms_forwarding\build\sdkconfig" ...`，不要把 `sdkconfig` 或 CMake 产物写到仓库根目录。PowerShell 下 `-D` 和 `SDKCONFIG=...` 必须分开。
- 不要再使用 `build-codex`、`build-codex2`、临时外部目录或源码目录作为 build path。
- 本目录除本文件外均为编译产物，不应提交到版本库。
