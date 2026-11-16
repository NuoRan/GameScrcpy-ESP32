# ESP32 触控盒 · GameScrcpy 社区扩展

[English](README.en.md)

把一块 ESP32 开发板变成手机的“外接触摸屏 + 键盘 + 鼠标 + 手柄”：板子经 USB 或蓝牙插进手机，电脑上的 [GameScrcpy](https://github.com/NuoRan/GameScrcpy) 经串口、Wi-Fi 或蓝牙把触控发给板子，手机不需要打开 USB 调试。这个仓库是 GameScrcpy 的社区扩展 `community.esp32`，作者“GameScrcpy 社区”，以 Apache-2.0 许可发布。它原是 GameScrcpy 2.0 内置的 ESP32 触控盒，现在只用 GameScrcpy 公开的扩展接口。

## 需要什么

- Windows 10 / 11（x64）上的 GameScrcpy 2.1 或更新的版本（扩展接口 ABI 2：长任务、设备提供者、事件、设置、硬件占用、设备与扩展设置里的面板、扩展的帮助）。
- 一块 ESP32-S3、S2、P4（USB 接手机）或经典 ESP32、C3、C6（蓝牙接手机）开发板，至少 4 MB 闪存。
- 电脑经蓝牙连板子时：这台电脑要有低功耗蓝牙（自带或 USB 适配器），板子是经典 ESP32、C3、C6 或 S3，固件 1.1 或更新。

## 安装与使用

1. 下载 `community.esp32-<版本>.gsext`，在 GameScrcpy 的“扩展”页选“从文件安装”，同意它要的权限：使用你选的设备（串口、蓝牙与刷写）、在设备页加设备、使用网络（只用于局域网里经 Wi-Fi 找板子）。
2. 把板子的串口接到电脑。扩展找到它后，它作为“触控盒”出现在“设备”页：“控制”不看画面直接操作手机，“设置”打开这块板子的窗口（试一下、固件、Wi-Fi、蓝牙、名称、诊断），“更多”里有刷写固件、诊断和断开。
3. 想让投屏中的手机经板子触控：在那台手机的设置里把触控方式选成 ESP32，并在这个扩展的手机设置里选串口（auto 或 COM7 这样的串口）、Wi-Fi 或蓝牙，以及地址。
4. 新板子或旧固件的板子：在扩展卡片的“设置”里选它的串口点“刷写固件”。
5. 想让电脑经蓝牙连板子、不接数据线：板子接在串口上时，在它的窗口里点“经蓝牙配对”，再把连接方式选成蓝牙。经蓝牙连着的板子在设备页显示蓝牙图标和信号；每一百次往返里最慢那次超过 30 毫秒时，它的窗口提示“蓝牙延迟较高”。

详细说明（接线、什么时候按 BOOT、各种提示的意思）在程序“帮助”页的“扩展”一组里，原文是 [docs/help/esp32.zh_CN.md](docs/help/esp32.zh_CN.md)。

### 从 GameScrcpy 2.0 的内置 ESP32 换过来

- GameScrcpy 2.1 起不再自带 ESP32 触控盒，ESP32 只经这个扩展使用。
- 2.0 的设置不迁移：手机设置里的触控方式“ESP32”与 `esp32.*` 设置在 2.1 里都不再认（手机回到普通触控，采集卡的触控回到“无”）。装好这个扩展后，重新为手机选触控方式，并在这个扩展里重新设置一次板子的 Wi-Fi。
- 板子上 1.0.x 的固件照常能用；电脑经蓝牙连板子要固件 1.1，板子窗口的“固件”里可以升级。
- GameScrcpy 安装扩展时检查 DLL 用到的系统功能：这个 DLL 用 `CreateFile` 打开串口、读自带的固件镜像，安装框可能提示“它的代码会读写文件”。它不读写你的其它文件。

## 仓库里有什么

| 目录 | 内容 |
|---|---|
| `extension/` | 扩展清单 `manifest.json`、画面板的脚本 `scripts/panel.ts`、图标 |
| `native/support` | 从 GameScrcpy 的 `gs::base` / `gs::platform` 留下的精简部分：`Expected`、日志、串口与 UDP 用到的 Win32 小工具 |
| `native/esp32` | 板子的协议链路：串口、UDP、低功耗蓝牙（WinRT GATT）、mDNS 查找、触点分槽、按键、诊断（原 GameScrcpy `src/esp32`） |
| `native/espflash` | 按乐鑫公开的串口下载协议刷写固件（原 GameScrcpy `src/espflash`） |
| `native/touchbox` | 扩展本身：找板子、触控方式、设备提供者、长任务、节点、事件，以及导出 `gs_ext_main` 的 `esp32.dll` |
| `firmware/` | ESP-IDF v6.1 固件工程与编好的镜像 `release/`（原 GameScrcpy `firmware/esp32`） |
| `protocol/` | 电脑 ↔ 板子协议：`SPEC.md`、共用的 C 头文件、向量生成器与金标准向量 |
| `sdk/include/gamescrcpy_ext.h` | GameScrcpy 原生扩展接口的 C 头文件，取自 GameScrcpy 2.1.0（ABI 2）；对着哪个版本的 GameScrcpy 编译，就用那个版本 `sdk/include/` 里的同名文件替换 |
| `docs/help/` | 扩展自带的帮助文章（英文、简体中文） |
| `tools/` | `pack.ps1`（出扩展包）、`build_firmware.ps1`（编固件）、`fake_esp32.py`（Wi-Fi 上的假板子） |
| `tests/unit` | doctest 单元测试：协议向量、链路与握手、刷写流程、找板子、扩展的 C 接口 |
| `tests/extension` | 给 `gs ext test` 的场景 |
| `third_party/` | doctest、tl::expected、nlohmann/json、miniz（版本与校验和见 [third_party/README.md](third_party/README.md)） |

## 构建与测试

需要 Visual Studio 2022（MSVC x64）、CMake 3.28+ 与 Ninja，在“Developer PowerShell for VS 2022”里：

```powershell
cmake --workflow --preset msvc-debug
ctest --preset msvc-debug
```

预设有 `msvc-debug`、`msvc-release`（LTO）、`msvc-asan`、`ci`（警告即错误）。产物在 `build/<预设>/`：`package/` 是扩展包的内容（DLL、清单、脚本、图标、帮助、固件、许可文本），`bin/` 是测试程序。代码规则与 GameScrcpy 相同：`/W4 /WX /permissive-`，C++20，不写注释，不用固定延时做同步。

## 打包

```powershell
$env:GS_HOME = 'C:\Program Files\GameScrcpy'
.\tools\pack.ps1
```

`pack.ps1` 用 `msvc-release` 预设编出 `esp32.dll`，核对扩展包的文件与固件镜像的 SHA-256，再用 `GS_HOME` 里的 `gs.exe pack` 写出 `dist/community.esp32-<版本>.gsext`。版本只写在 `extension/manifest.json` 里。

## 用 `gs ext test` 跑场景

```powershell
$tests = Get-ChildItem tests\extension\*.json | ForEach-Object { '--test'; $_.FullName }
& "$env:GS_HOME\gs.exe" ext test build\msvc-release\package @tests
```

扩展包里不带测试，所以每个场景用一个 `--test` 交给 `gs ext test`。场景不需要真板子，也不会让电脑的蓝牙去搜索；串口设成不存在的 `COM250` 或连接方式设成蓝牙，结果不受电脑上插着什么影响。格式是 GameScrcpy 的 `gamescrcpy.ext-test` 第 1 版：每个场景一个 JSON，可以写 `settings`、`deviceSettings`（手机叫 `mock-phone`）、`holders` 与 `timeout`；`steps` 里每一步用 `do` 写动作（`call`、`job`、`touch.open`、`devices`、`device.act`、`settings`、`holder`、`claim.want`、`claims` 等），用 `expect` 写要核对的结果。每个场景从空的存储开始，放不进板子的密钥，所以原来经 Wi-Fi 连假板子（`tools/fake_esp32.py`）的场景去掉了，链路由 `tests/unit` 里的假板子测试。

## 给逻辑与面板用的接口

扩展的脚本、面板与用户自己的逻辑都可以用（`ext.community.esp32`）：

| 名字 | 种类 | 说明 |
|---|---|---|
| `boards()` | 节点 → json | 找到的板子：编号、状态、连接方式、芯片、固件、手机、Wi-Fi、往返延迟、蓝牙信号与最慢往返、能否升级 |
| `bluetooth()` | 节点 → json | 这台电脑的蓝牙状态、最近在附近看到的板子（地址、芯片、信号、有没有密钥、是否配对、是否连着）、经蓝牙配对过的板子 |
| `ports()` | 节点 → json | 这台电脑的串口：名字、标题、种类、像不像板子 |
| `firmware()` | 节点 → json | 自带的固件：版本与有镜像的芯片 |
| `problem()` | 节点 → string | 上一次查找为什么没找到板子 |
| `language()` | 节点 → string | 扩展文字用的语言（`zh_CN` 或 `en`，跟随 Windows 的显示语言） |
| `tap(device)`、`disconnect(device)`、`rescan()` | 节点 → boolean | 试一下、放开一块板子、重新查找 |
| `flash({ device })` | 长任务 | 刷写固件；`device` 是板子编号或串口（如 `COM7`） |
| `wifi({ device, ssid, password })` | 长任务 | 经串口设置 Wi-Fi；`ssid` 为空则忘记 Wi-Fi |
| `rename({ device, name })` | 长任务 | 给板子改名（1～32 字节） |
| `diagnose({ device })` | 长任务 | 询问板子或串口，说明哪里不对 |
| `scan({})` | 长任务 | 经蓝牙查找附近的板子六秒，结果与 `bluetooth()` 相同 |
| `pair({ device })` | 长任务 | 经蓝牙配对：`device` 是接在 USB 上的板子（先交给它密钥）、这台电脑有密钥的板子编号，或附近板子的蓝牙地址 |
| `unpair({ device })` | 长任务 | 从 Windows 和扩展里删除蓝牙配对，密钥留给 Wi-Fi；`device` 是板子编号或蓝牙地址 |

事件（`events.on(主题, ({ value }) => …)`）：`board.added`、`board.changed`、`board.removed`、`ports.changed`、`flash.progress`、`flash.done`、`board.diagnosed`、`bluetooth.changed`。

## 固件与协议

- 固件：装好 ESP-IDF v6.1 后运行 `tools\build_firmware.ps1`，它编译六个型号、合并镜像并更新 `firmware/release/firmware.json`。说明见 [firmware/README.md](firmware/README.md)。
- 协议：[protocol/SPEC.md](protocol/SPEC.md)。改协议的顺序是 SPEC → `protocol/gen_vectors.py` → `gs_esp32_protocol.h` → 测试，向量不手改。

## 更新记录

- 1.1.1：
  - USB：鼠标移动很密时触控不再被丢掉（视角放开时 WASD 失灵）。
  - 蓝牙：1.1.0 及更早的固件在经典 ESP32、C3、C6 上蓝牙根本没起来——给手机的 HID 报告描述符有 743 字节，超过 NimBLE HID 服务的 512 字节上限，`esp_hidd_dev_init` 直接失败，板子既不向手机广播，也不开给电脑的串口服务。现在蓝牙单独用一份 5 指的报告描述符（503 字节，USB 仍是 10 指），报告表按描述符里的 7 个报告配置；HID 起不来时给电脑的串口服务照常开；“最多触点数”写入真实值；蓝牙上的鼠标移动累加后再发、不再丢。经蓝牙连手机时最多同时 5 指。
  - 刷写：板子跑着本固件、分区表又与新固件相同时，升级保留它的设置（Wi-Fi、名称、密钥和蓝牙配对），不再每次升级都清空；板子没有应答时照旧整片写入。
  - 配对：Windows 还留着旧配对、板子却已经忘了这台电脑（比如刷写清掉了设置）时，“经蓝牙配对”删掉旧配对再配一次，不再一直报“没有配对”。
  - 电脑经蓝牙连板子：以前在真板子上一次都连不上（Windows 按 128 位 UUID 不经缓存查找服务时，对板子总是报“设备不识别此命令”）。现在先不经缓存列出全部服务、再从中挑出串口服务。已在 ESP32-S3 上核对：配对、经蓝牙连接、从扩展升级固件后不用重新配对、设置被整片擦掉后重新配对。
  - 连接方式从串口改成蓝牙时，第一次查找不再报“正被别人占用”、等十几秒才连上：先关掉旧的串口连接，再开始新的查找（关串口要约 2 秒，以前新查找的占用请求会在这段时间里超时）。

## 还没做的

- 经典 ESP32、C3、C6 上的电脑蓝牙与手机蓝牙 HID 还没有在真板子上核对过。

发布地址由维护者发布时补上。
