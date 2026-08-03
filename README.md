# Murphy M4 / Fengyan 阅读器固件

ESP32-S3 墨水屏阅读器固件（480×800 E-Ink），采用**双 OTA** 结构：APP0 为原厂系统，APP1 为本固件。支持插件体系（Lua 沙箱）、原生阅读器、宿主列表场景与内容提供管线。

> 配套插件：[m4-weread-plugin](https://github.com/einklover/m4-weread-plugin)（微信读书）、[m4-fanqie-plugin](https://github.com/einklover/m4-fanqie-plugin)（番茄小说）。

## 一、刷写固件

### 前置

- 设备通过 USB 连接电脑，出现 `/dev/cu.usbmodemXXX` 串口
- 安装 PlatformIO：`pip install platformio`（或使用 IDE 插件）

### 安全刷写（仅 APP1）

默认只写 APP1 分区（`0x6e0000`），**不会触碰** APP0 / bootloader / 分区表 / NVS：

```bash
python3 scripts/murphy_m4_app1_flash.py \
  --port /dev/cu.usbmodemXXX \
  --firmware .pio/build/murphy_m4/firmware.bin \
  --i-understand-app1-only
```

刷写完成后设备自动切到 APP1 启动。如需返回原厂 APP0（仅切槽，不擦除）：

```bash
python3 scripts/murphy_m4_app1_flash.py --port /dev/cu.usbmodemXXX --select-slot 0
```

### 串口纪律（重要）

- 同一时间只允许一个程序占用串口（不要同时开串口监视器/多个调试工具）
- 反复开关串口会导致设备复位；优先使用持续会话
- 设备静默无响应时，先**断电重启**，不要反复重试

## 二、安装插件

插件为 `.m4x` 包（zip 格式，含 `manifest.json` 与 `main.lua` 入口）。三种安装方式：

### 1. Wi-Fi / 串口安装（推荐，需开启 USB 串口调试）

```bash
python3 scripts/m4adb.py --port /dev/cu.usbmodemXXX install fanqie.m4x
```

默认优先走局域网 Wi-Fi 传输，失败自动回退串口。也可指定 `--transport wifi` 或 `--transport usb`。返回 `noop: true` 表示设备已有同版本，无需重复安装。

### 2. SD 卡安装

把 `.m4x` 放入 SD 卡 `/apps_inbox/`，在设备「应用 / 安装」入口选择安装。

### 3. 源码目录安装（开发用）

```bash
python3 scripts/m4adb.py --port /dev/cu.usbmodemXXX install ./fanqie_src
```

## 三、日常使用

- **书架**：已打开的书会保留进度；第一行「分类浏览」进入分类书单
- **书单**：底部左右键翻页，已看过的页会缓存（回翻秒开），无需反复等待网络
- **目录**：FileRows 大目录按需读取，章节在后台预取
- **阅读器**：点击右半屏翻下一页，左半屏回上一页；章节末尾自动加载下一章
- **返回**：顶栏或返回键逐级退出

## 四、插件功能

### WeRead（微信读书）

扫码登录后自动续期会话；章节按分片协议下载并加密缓存，支持离线阅读。

### Fanqie（番茄小说）

无需账号；分类 → 书单 → 书籍 → 章节全链路，远程分页书单与 FileRows 目录。

## 五、常见问题

| 现象 | 处理 |
|---|---|
| 插件安装返回瞬态 SD 写失败 | 重试一次；`install` 返回 `noop:true` 即已装好 |
| 阅读器打开章节失败 | 确认已连接 Wi-Fi；Fanqie 内容走公开镜像，免证书校验 |
| 设备无响应/串口静默 | 断电重启（长按电源或拔电） |
| 中文缺失 | 将 NotoSansCJKsc.epdfont 放入 SD 卡 `/fonts/` 获得完整 CJK 字体（内置子集已覆盖常用 UI 字符） |

## 六、开发者

### 构建

```bash
pio run -e murphy_m4
# 产物：.pio/build/murphy_m4/firmware.bin
```

第三方依赖由 PlatformIO `lib_deps` 拉取；本仓库不包含 vendor 的第三方库源码与内置字体数据（内置 CJK 字体可用 `lib/EpdFont/scripts/fontconvert.py` 重新生成）。

### 项目结构

- `src/` — 固件主源码（AppRuntime、LuaHost、阅读器、网络、安装器）
- `lib/` — 自研组件库（字体/渲染、TXT 解析、文件系统封装等）
- `scripts/` — 刷写/调试工具（m4adb、APP1 安全刷写脚本）

## License

MIT，见 [LICENSE](LICENSE)。
