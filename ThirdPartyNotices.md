# 第三方依赖

| 依赖 | 版本／用途 | 许可及来源 |
| --- | --- | --- |
| Qt Base／Translations | 6.11.1，动态链接 GUI、Core、Widgets、Test 及部署依赖 | LGPLv3／GPLv3 等上游条款，实际模块及许可由上游提供；本分发使用动态 DLL，许可全文在 `Licenses`，源码见 https://download.qt.io/archive/qt/6.11/6.11.1/single/ |
| Qt/MSBuild | 3.4.1，仅构建工具，不进入便携版运行时 | Qt VS Tools 官方包，https://download.qt.io/official_releases/vsaddin/3.4.1/ |
| zlib | 1.3.2，Core 内静态编译 | [zlib 许可](ThirdParty/Zlib/LICENSE)；https://zlib.net/ |
| nlohmann/json | 3.12.0，JSON 清单和报告 | [MIT](ThirdParty/Json/LICENSE.MIT)；https://github.com/nlohmann/json/releases/tag/v3.12.0 |
| Microsoft Visual C++ Runtime | v143 x64，运行库 DLL | 随 VS 2022 的可再分发组件；Microsoft 许可，https://learn.microsoft.com/cpp/windows/redistributing-visual-cpp-files |

下载与源码指纹分别记录于 `Artifacts/Dependencies/Downloads/DependencyReceipt.json` 和 `ThirdParty/DependencySources.json`。维护工具的 Python 依赖只用于本地开发。

项目自有源码采用根目录 `LICENSE` 中的 Apache License 2.0。上述第三方组件遵循各自许可证；zlib 和 nlohmann/json 的上游许可文件统一保存在 `ThirdParty` 下，便携分发沿用相同路径。
