# third_party — 第三方依赖清单

所有依赖都以固定版本纳入仓库，配置阶段不联网。这几份文件与 GameScrcpy 仓库 `third_party/` 里的同名文件逐字相同（取自 GameScrcpy 提交 `6a26cb0e`），校验和也与那里记录的一致。升级依赖必须同时更新本表。

| 依赖 | 版本 | 许可 | 用途 | 引入方式 |
|---|---|---|---|---|
| doctest | v2.5.3 | MIT | C++ 单元测试 | 单头文件 |
| tl::expected | v1.1.0 | CC0-1.0 | C++20 下的 `Expected` 实现 | 单头文件 |
| nlohmann/json | v3.12.0 | MIT | 读写扩展的选项、设置、存储与 `firmware.json`；测试读取协议金标准向量 | 单头文件，只在 .cpp 中包含 |
| miniz | 3.1.2 | MIT | 刷写固件时压缩镜像 | 发布包里的 `miniz.c`、`miniz.h` 与 `LICENSE`，原样未改；以 `MINIZ_NO_ZLIB_COMPATIBLE_NAMES` 编译成静态库 |

扩展包里的 `licenses/` 文件夹带上 nlohmann/json、miniz 与 tl::expected 的许可文本（DLL 里用到了它们）；doctest 只用于测试，不进扩展包。

## 文件校验

| 文件 | 来源 | SHA256 |
|---|---|---|
| `doctest/doctest/doctest.h` | https://raw.githubusercontent.com/doctest/doctest/v2.5.3/doctest/doctest.h | `cfd518a3ef90f67e1f3ba514df23fb3627437de1a2feeba78cf5062a40021421` |
| `doctest/LICENSE.txt` | https://raw.githubusercontent.com/doctest/doctest/v2.5.3/LICENSE.txt | `0fe0b331fa1513dcce8604ff1fa925f32d1cea17d8aeb1c2471fad40d291adc5` |
| `tl-expected/include/tl/expected.hpp` | https://raw.githubusercontent.com/TartanLlama/expected/v1.1.0/include/tl/expected.hpp | `5a6a46da2e46ee3360bd8fbd759affefc46b8bfd6295043262ec867acac1fa14` |
| `tl-expected/COPYING` | https://raw.githubusercontent.com/TartanLlama/expected/v1.1.0/COPYING | `a2010f343487d3f7618affe54f789f5487602331c0a8d03f49e9a7c547cf0499` |
| `nlohmann-json/include/nlohmann/json.hpp` | https://raw.githubusercontent.com/nlohmann/json/v3.12.0/single_include/nlohmann/json.hpp | `aaf127c04cb31c406e5b04a63f1ae89369fccde6d8fa7cdda1ed4f32dfc5de63` |
| `nlohmann-json/LICENSE.MIT` | https://raw.githubusercontent.com/nlohmann/json/v3.12.0/LICENSE.MIT | `46a65cffd1ea955132d95a8dd921640714a8d6b537d2e4e482d31145ae95b603` |
| （miniz 发布包） | https://github.com/richgel999/miniz/releases/download/3.1.2/miniz-3.1.2.zip | `f0446d863f9c19926ad9483c523fdc42e42b8d4a6a431d27e09d49c79a140d9a` |
| `miniz/miniz.c` | 上述发布包 | `e2c1aeb66eef9191d8c3feb164db2def2335a61d039bf04ed849f6b042433b30` |
| `miniz/miniz.h` | 上述发布包 | `b53b62ed122e559b8f679e3cb787a0b0035fe87a58f909da0e44931678f4e85f` |
| `miniz/LICENSE` | 上述发布包 | `0115478d567121238cf6cc1c0c361926cf07a49d9e4c9e66da97fac6a01646b3`（发布包里是 CRLF；仓库按 `.gitattributes` 存成 LF，LF 版本的 SHA256 是 `6f20fa7672b00e2e975c291df737cf227addf3ad32e36fef3fe0f416e4664d3d`） |
