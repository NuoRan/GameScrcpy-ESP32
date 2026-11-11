# ESP32 触控盒 · GameScrcpy 社区扩展

[English](README.en.md)

把一块 ESP32 开发板变成手机的“外接触摸屏 + 键盘 + 鼠标 + 手柄”：板子经 USB 或蓝牙插进手机，电脑上的 [GameScrcpy](https://github.com/NuoRan/GameScrcpy) 经串口或 Wi-Fi 把触控发给板子，手机不需要打开 USB 调试。这个仓库是 GameScrcpy 的社区扩展 `community.esp32`，作者“GameScrcpy 社区”，以 Apache-2.0 许可发布。它原是 GameScrcpy 2.0 内置的 ESP32 触控盒，现在只用 GameScrcpy 公开的扩展接口。

## 需要什么

- Windows 10 / 11（x64）上的 GameScrcpy 2.1 或更新的版本（扩展接口 ABI 2：长任务、设备提供者、事件、设置、硬件占用、设备与扩展设置里的面板、扩展的帮助）。
- 一块 ESP32-S3、S2、P4（USB 接手机）或经典 ESP32、C3、C6（蓝牙接手机）开发板，至少 4 MB 闪存。

## 安装与使用

1. 下载 `community.esp32-<版本>.gsext`，在 GameScrcpy 的“扩展”页选“从文件安装”，同意它要的权限：使用你选的设备（串口与刷写）、在设备页加设备、使用网络（只用于局域网里经 Wi-Fi 找板子）。
2. 把板子的串口接到电脑。扩展找到它后，它作为“触控盒”出现在“设备”页：“控制”不看画面直接操作手机，“设置”打开这块板子的窗口（试一下、固件、Wi-Fi、名称、诊断），“更多”里有刷写固件、诊断和断开。
3. 想让投屏中的手机经板子触控：在那台手机的设置里把触控方式选成 ESP32，并在这个扩展的手机设置里选串口（auto 或 COM7 这样的串口）或 Wi-Fi 与地址。
4. 新板子或旧固件的板子：在扩展卡片的“设置”里选它的串口点“刷写固件”。

详细说明（接线、什么时候按 BOOT、各种提示的意思）在程序“帮助”页的“扩展”一组里，原文是 [docs/help/esp32.zh_CN.md](docs/help/esp32.zh_CN.md)。

### 与 GameScrcpy 内置 ESP32 的关系

- 这个扩展不读内置扩展 `gamescrcpy.esp32` 的东西：内置的触控方式与 `esp32.*` 设置不迁移，板子的 Wi-Fi 要在这个扩展里重新设置一次。
- 两者同时打开时会抢同一个串口；用这个扩展时请在“扩展”页关掉内置的 ESP32 触控盒。
- GameScrcpy 安装扩展时检查 DLL 用到的系统功能：这个 DLL 用 `CreateFile` 打开串口、读自带的固件镜像，安装框可能提示“它的代码会读写文件”。它不读写你的其它文件。

## 仓库里有什么

| 目录 | 内容 |
|---|---|
| `extension/` | 扩展清单 `manifest.json`、画面板的脚本 `scripts/panel.ts`、图标 |
| `native/support` | 从 GameScrcpy 的 `gs::base` / `gs::platform` 留下的精简部分：`Expected`、日志、串口与 UDP 用到的 Win32 小工具 |
| `native/esp32` | 板子的协议链路：串口、UDP、mDNS 查找、触点分槽、按键、诊断（原 GameScrcpy `src/esp32`） |
| `native/espflash` | 按乐鑫公开的串口下载协议刷写固件（原 GameScrcpy `src/espflash`） |
| `native/touchbox` | 扩展本身：找板子、触控方式、设备提供者、长任务、节点、事件，以及导出 `gs_ext_main` 的 `esp32.dll` |
| `firmware/` | ESP-IDF v6.1 固件工程与编好的镜像 `release/`（原 GameScrcpy `firmware/esp32`） |
| `protocol/` | 电脑 ↔ 板子协议：`SPEC.md`、共用的 C 头文件、向量生成器与金标准向量 |
| `sdk/include/gamescrcpy_ext.h` | GameScrcpy 原生扩展接口的 C 头文件，取自 GameScrcpy 提交 `6a26cb0e`（2.1.0 开发中，ABI 2）；对着哪个版本的 GameScrcpy 编译，就用那个版本 `sdk/include/` 里的同名文件替换 |
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
& "$env:GS_HOME\gs.exe" ext test build\msvc-release\package
```

`tests/extension/` 里的场景不需要真板子；`touch-fake-board-over-wifi.json` 要先在另一个窗口运行 `python tools\fake_esp32.py --port 45455 --phone usb --phone-delay 0`。场景的格式（`gamescrcpy.ext-test` 第 1 版）跟着 GameScrcpy 的 `gs ext test` 走，以 GameScrcpy 文档为准。

## 给逻辑与面板用的接口

扩展的脚本、面板与用户自己的逻辑都可以用（`ext.community.esp32`）：

| 名字 | 种类 | 说明 |
|---|---|---|
| `boards()` | 节点 → json | 找到的板子：编号、状态、芯片、固件、手机、Wi-Fi、往返延迟、能否升级 |
| `ports()` | 节点 → json | 这台电脑的串口：名字、标题、种类、像不像板子 |
| `firmware()` | 节点 → json | 自带的固件：版本与有镜像的芯片 |
| `problem()` | 节点 → string | 上一次查找为什么没找到板子 |
| `language()` | 节点 → string | 扩展文字用的语言（`zh_CN` 或 `en`，跟随 Windows 的显示语言） |
| `tap(device)`、`disconnect(device)`、`rescan()` | 节点 → boolean | 试一下、放开一块板子、重新查找 |
| `flash({ device })` | 长任务 | 刷写固件；`device` 是板子编号或串口（如 `COM7`） |
| `wifi({ device, ssid, password })` | 长任务 | 经串口设置 Wi-Fi；`ssid` 为空则忘记 Wi-Fi |
| `rename({ device, name })` | 长任务 | 给板子改名（1～32 字节） |
| `diagnose({ device })` | 长任务 | 询问板子或串口，说明哪里不对 |

事件（`events.on(主题, ({ value }) => …)`）：`board.added`、`board.changed`、`board.removed`、`ports.changed`、`flash.progress`、`flash.done`、`board.diagnosed`。

## 固件与协议

- 固件：装好 ESP-IDF v6.1 后运行 `tools\build_firmware.ps1`，它编译六个型号、合并镜像并更新 `firmware/release/firmware.json`。说明见 [firmware/README.md](firmware/README.md)。
- 协议：[protocol/SPEC.md](protocol/SPEC.md)。改协议的顺序是 SPEC → `protocol/gen_vectors.py` → `gs_esp32_protocol.h` → 测试，向量不手改。

## 还没做的

- 电脑经蓝牙连板子（BLE 串口服务）：固件与扩展都要加，链路层已经为第三种连接方式留好了位置。

发布地址由维护者发布时补上。
