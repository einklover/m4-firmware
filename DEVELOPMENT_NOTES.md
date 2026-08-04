# Murphy M4 开发经验与避坑手册

面向接手开发者。本固件的开发过程中踩过的坑、验证过的模式、以及必须遵守的纪律，按主题整理如下。

---

## 0. 总体架构速览

- **双 OTA**：APP0 原厂系统 / APP1 本固件。日常开发只动 APP1。
- **插件体系**：`.m4x` 包（zip）→ Lua 沙箱（512KB 堆预算）→ 宿主（C++）提供 `ui.* / dl.* / net.* / provider.* / sys.* / fs.*` API。
- **内容管线**：宿主投影（jsonGet/jsonToFile 在宿主侧解析，Lua 只拿小结果）+ FileRows 虚拟目录（大 TOC 驻 SD）+ 原生阅读器（章节文件 + prefetch）。

---

## 1. 环境与工具链（macOS）

| 事项 | 说明 |
|---|---|
| 一切串口脚本用 `~/.platformio/penv/bin/python` | 系统 python 缺 `pyserial`/`esptool`，直接跑必挂 |
| `otatool` 切槽必须 penv python 跑 | 它内部用 `sys.executable` 拉起 esptool；anaconda python 会报 `No module named esptool` |
| 刷写后用 penv 跑 `otatool.py read_otadata` 复核槽位 | 确认已切到 slot 1 再继续 |

## 2. 串口纪律（血泪教训，最重要）

- **同一时间只有一个串口 owner**。不要 `screen`/串口监视器/m4adb/esptool 并行。
- **反复 open/close 会让 USB JTAG 彻底静默**——连 esptool 都收不到数据，**只能物理断电重启**恢复。失败后先等 10-20s 再考虑重开，禁止 `for i in range(N): open→ping→close`。
- 设备 boot 到桥就绪约 4-6s，`wait_ready(timeout>=60)`。
- 设备静默/无响应时：断电重启，不要盲目重刷。

## 3. 刷写与升级

- **只刷 APP1**：`scripts/murphy_m4_app1_flash.py --port ... --firmware firmware.bin --i-understand-app1-only`，写 `0x6e0000`，校验 hash。
- **禁止**：`pio run -t upload`（不保证只写 APP1）、整片擦除、写 APP0/分区表/NVS（除用户明确要求）。
- 切回原厂：`--select-slot 0`（只切槽不擦数据）。
- 刷完等设备重启并重新枚举（5-15s）再连 m4adb。

## 4. m4adb 使用

- **daemon 竞态**：命令连不上 socket 会自动起新 daemon → 双 daemon 抢端口 → 设备复位。遇到异常先 `pkill -f m4adb.py; rm -f /tmp/m4adb-*.sock` 再重来。
- **`--no-daemon` 与常驻 daemon 混用 = 双端口冲突**，选一种用。
- **最稳模式**：写单连接脚本 `Client(SerialTransport(port, 115200))` 一条连接跑完 launch→tap→screenshot→status 全流程，日志用 `log_sink` 收集；设备重启时连接会断，脚本要能容错。
- 设备重启会导致 daemon 死、日志流丢失——抓日志时留意。

## 5. 真机验证

- **截图是 PBM**，用 PIL 转 PNG 后 `tesseract -l chi_sim` OCR（放工作目录跑，tesseract 读 /tmp 有时失败）。
- 翻页节奏：一次书单 fetch 约 3-4s，间隔 <2.5s 会踩到 loading 屏（快速连点会触发取消逻辑）。
- **长时间持续翻页后宿主/串口偶发冻结**（未完全定位，疑似 e-ink 连续刷新或宿主 UI 循环）——断电重启即可恢复，不影响已修复的功能。
- **launch 新 app 时旧 Reader sub-activity 可能残留占屏**（tap 会落错地方）——先 `key("back")` 退出再 launch。
- 验证"章节末尾翻页"要翻 50+ 页：用单连接脚本 + 每 1.5s tap，章节较短的书几页就翻完。

## 6. Lua 插件开发（512KB 堆约束下）

### 核心模式
- **宿主投影**：大 JSON（书单 87KB/页、TOC 数百 KB）必须走 `dl.jsonGet`/`dl.jsonToFile`，宿主侧解析，Lua 只拿投影小结果或文件路径。**禁止** `json.decode` 大响应。
- **FileRows**：大目录用 `dl.jsonToFile` 写 `toc_rows.txt` + `Catalog.virtual_rows`，行按需解析（`provider.resolveCatalogWork`）。
- **延迟网络任务**：`begin_network_job` 先渲染一帧 loading，下一帧 draw 里 `advance_network_job` 再执行同步 fetch——保证用户每次操作先看到反馈。
- **宿主场景**（`ui.listOpen`）：`page_count/initial_page/remote_page` 让书单远程分页。宿主 `uiCallPage` 在边界 clamp 后**不回调**（末页再翻 = 静默 no-op）；远程行回调 `idx0 = (page-1)*pageSize + local`。
- **会话缓存**：已访问书单页缓存（8 页窗口，~1KB/页），回翻秒开、零网络；换分类清空。

### 常见崩溃/错误（全部真实踩过）
| 症状 | 根因 | 修法 |
|---|---|---|
| `attempt to perform arithmetic` → app 退回 Home | 全局变量未初始化（如 `booklist_page` 只在旧宿主回退分支赋值），宿主回调抛错 → `setFailed` → 退出 | 所有状态变量在声明处初始化 |
| 翻页几页后状态错乱（footer 页码 ≠ 内容） | loading 屏 tap 在 fetch 已进行时把 `screen` 误设 `toc`，与后台 fetch 完成的开场景打架 | 区分 pending job / in-flight：in-flight 时忽略 tap |
| 取消翻页后翻页永久失效 | `booklist_loading` 未复位 | cancel 路径重置标志位 |
| Lua C API 段错误（t=函数指针） | `lua_rawseti(L, -2, k)` 栈不足时 -2 越界读到残留值 | 构造表格时每元素一个 `lua_newtable`，结果表留在栈底；先推栈再索引 |
| 内存 guard 误报/翻页 OOM | Lua 同时持有解码表+书单+UI rows 三份 | 每页只驻留当前页，旧页交给缓存引用，`collectgarbage` 及时回收 |

### 宿主回调错误会直接杀死 app
宿主用 pcall 调 Lua 回调，错误 → `setFailed` → `renderError` → app 退出到 Home。**任何回调（onTouch/onKey/on_row/on_page/draw）内不许抛错**——宁可在 Lua 里 pcall 包住。

## 7. 宿主（C++）注意

- **ArduinoJson 默认分配器走内部 RAM**：解析 87KB 书单响应会建 ~150-200KB 池，把 287KB 内部堆顶到 38KB 空闲 → 持续翻页后设备重启。**必须用 PSRAM allocator**（`JsonDocument doc(PsramJsonAllocatorInstance())`）。
- **TLS 证书**：设备 CA bundle 不含 ZeroSSL（fanqie 镜像 `fq-book.nat.netsite.cc:8043` 用 ZeroSSL ECC DV SSL CA 2），验证握手必失败（-30336）。按 app 分支：无凭证 app（fanqie）`setInsecure`，有 Cookie 的（weread）保持校验。
- **min_free_heap 是金标准**：翻页/下载时监控它，一旦掉到几十 KB 就要怀疑内部堆被大块占用。
- net.request 手动跟随重定向（可剥 Cookie）；jsonGet 用 HTTPClient `FORCE_FOLLOW_REDIRECTS`。

## 8. 版本与发布

- **插件改源码必须 bump `manifest.json` 的 version/versionCode**，否则设备端无法判断新旧。
- **`.m4x` 包与源码易失同步**：weread.m4x 曾停留在 0.6.2 而源码到 0.6.8。发版前 `unzip -p pkg manifest.json` 核对版本，或 diff 解包内容与源码目录。
- 测试夹具（`test/fixtures/m4x/fanqie_src` 等）与插件源码保持同步，改插件先同步 fixture 再跑模拟器。
- 固件/插件对真机验证顺序：**模拟器回归（ctest 49 项）→ 编译 → 真机**。

## 9. 开源发布

- 发布前**脱敏**：docs/task_prompts 常含个人路径（`/Volumes/z`、`/Users/...`）、设备 IP、实验文件（`*原始*`/`*玄三*` 字体）、示例 IP 改成测试网段（192.0.2.x）。
- 第三方库（Lua/EPUB/expat/miniz）与字体数据不入库，README 注明由 lib_deps/脚本生成。
- 书源插件（fanqie/weread）合规由使用者自担，LICENSE 注明。

---

## 快速排查清单

```
设备无响应        → 断电重启（不是重刷）
串口连不上        → pkill m4adb; rm /tmp/m4adb-*.sock; 单 daemon 重连
插件 app 退出     → 查 Lua 回调错误（宿主 setFailed 显示在屏幕/日志）
翻页慢            → 检查缓存是否生效（jsonGet 计数）
翻页后重启        → 查内部堆 min_free（PSRAM allocator 是否生效）
章节下载失败      → 查 TLS（bundle 缺 CA → 按 app 免校验）
```
