# Murphy M4 / Fengyan 阅读器固件

ESP32-S3 双 OTA 墨水屏阅读器固件（APP0 原厂系统 / APP1 本固件），支持 M4x 插件体系（Lua 沙箱）、原生阅读器、宿主列表场景与 provider 内容管线。

## 项目结构

- `src/` — 固件主源码（AppRuntime、LuaHost、阅读器、网络、安装器等）
- `lib/` — 自研组件库（字体/渲染、TXT 解析、文件系统封装等）
- `scripts/` — 构建/刷写/调试工具（含仅写 APP1 的安全刷写脚本）
- `platformio.ini` — PlatformIO 构建配置（`pio run -e murphy_m4`）

第三方依赖由 PlatformIO `lib_deps` 拉取（ArduinoJson 等），vendor 的第三方库（Lua/EPUB/expat/miniz 等）与内置字体数据未包含在本仓库。

## 构建

```bash
pio run -e murphy_m4
# 产物: .pio/build/murphy_m4/firmware.bin
```

## 刷写（仅 APP1）

```bash
python3 scripts/murphy_m4_app1_flash.py --port /dev/cu.usbmodemXXX \
  --firmware .pio/build/murphy_m4/firmware.bin --i-understand-app1-only
```

安全边界：默认只写 APP1（0x6e0000）；不触碰 APP0/bootloader/分区表/NVS。

## 插件

配套插件仓库：
- m4-weread-plugin（微信读书：扫码登录、分片章节）
- m4-fanqie-plugin（番茄小说：远程分页书单、FileRows 目录）
