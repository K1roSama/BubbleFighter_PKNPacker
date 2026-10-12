# 开发与贡献指南

本文面向构建、测试和维护 PKNPacker 的开发者。首次运行及环境要求见[构建指南](Docs/Build.md)，命令参数见[命令行参考](Docs/CommandLine.md)。

## 源码结构

```text
PKNPacker.sln
Source/
  PKNPacker.Core/          # 格式、读取、工作区、写入器、哈希和任务
  PKNPacker/               # 中文 Qt Widgets 图形管理器
  PKNPacker.Cli/           # 命令行入口
Tests/PKNPacker.Tests/     # 核心行为测试与固定密码流向量
Build/                    # 共享 MSBuild 属性与本地配置示例
Tools/                    # 构建、部署、依赖下载和工程生成
ThirdParty/               # zlib、nlohmann/json 源码、上游许可及来源指纹
Docs/                     # 使用参考、架构、格式与验证说明
Licenses/                 # Qt 许可文本
Artifacts/                # 本地生成目录，不提交到 Git
```

GUI 与 CLI 共用 `PKNPacker.Core`。Core 不依赖 Qt、IDA 或客户端运行时。修改共享行为时应在 Core 中实现，确保两个入口的路径规则、工作区操作和输出验证一致；模块关系见[架构说明](Docs/Architecture.md)。

## 构建

工程提供 **Debug／Release × x64**。在仓库根目录运行：

```powershell
.\Tools\Build.ps1 -Configuration Debug
.\Tools\Build.ps1 -Configuration Release
```

也可以用 Visual Studio 2022 打开 `PKNPacker.sln`，选择 `x64` 后构建。`Tools/Build.ps1` 通过 `vswhere` 查找已安装的 VS 2022，构建失败时会抛出错误。

| 输出 | 位置 |
| --- | --- |
| 可执行文件和核心库 | `Artifacts/Bin/x64/<配置>` |
| 中间文件 | `Artifacts/Obj/x64/<配置>/<项目>` |
| 脚本构建日志 | `Artifacts/Logs/<配置>Build.log` |

GUI 需要 Qt DLL 和平台插件，使用部署脚本收集后运行。CLI 与核心测试可直接从编译输出目录启动，普通 C++ 构建不需要 Python。

### 只构建 Core 或 CLI

只构建核心库：

```powershell
.\Tools\Build.ps1 -Configuration Release -CoreOnly
```

CLI 和核心测试也不依赖 Qt。可以在 VS 中单独构建对应项目及其 Core 依赖，或在 **Visual Studio Developer PowerShell** 中运行：

```powershell
MSBuild.exe .\Source\PKNPacker.Cli\PKNPacker.Cli.vcxproj /m /p:Configuration=Release /p:Platform=x64
MSBuild.exe .\Tests\PKNPacker.Tests\PKNPacker.Tests.vcxproj /m /p:Configuration=Release /p:Platform=x64
```

### 维护工程文件

`Tools/GenerateProjects.py` 用于生成原生 `.vcxproj` 和筛选器文件；普通构建无需运行。添加源文件或修改工程生成规则时，同步检查生成脚本及工程文件，避免重新生成后丢失配置。

## 测试

构建后运行不依赖客户端的合成测试：

```powershell
.\Artifacts\Bin\x64\Debug\PKNPacker.Tests.exe
.\Artifacts\Bin\x64\Release\PKNPacker.Tests.exe
```

每次运行的逐项结果保存到 `Artifacts/TestRuns/<进程 ID>/TestReport.json`。权限不足导致的符号链接测试跳过会在报告中明确记录；跳过不能视为通过。

有匹配版本的完整 `Packs` 目录时，可以追加原包读取和样本往返检查：

```powershell
.\Artifacts\Bin\x64\Release\PKNPacker.Tests.exe --samples 'D:\Game\Packs' --all
```

`--all` 检查指定目录中的全部 PKN；样本回归另外需要 `Shader_00000.pkn` 进行往返、改名与缺失文件检查。单个资源包的读取检查可使用 CLI `verify`。测试范围、固定向量来源和历史验证记录见[验证说明](Docs/Validation.md)。

## 生成便携分发

默认构建 Release 后部署：

```powershell
.\Tools\Deploy.ps1
```

已有 Release 构建时，可以跳过编译：

```powershell
.\Tools\Deploy.ps1 -SkipBuild
```

脚本使用 `windeployqt` 收集 Qt DLL 和平台插件，并附带中文翻译、v143 运行库、便携版使用说明、许可证和 `DistributionManifest.json` 文件摘要清单。插件与第三方许可分别集中到 `plugins` 和 `Licenses`；测试程序、测试向量和源码文档留在仓库中。ZIP 从全新暂存目录生成，更新本地分发目录时保留用户工作区与新增文件。分发目录和 ZIP 的位置见[构建指南](Docs/Build.md#构建并运行)。

使用已有 Qt 安装时，部署前将 `PKNPACKER_QT_ROOT` 设置为构建时使用的 Qt 路径，具体配置见[构建指南](Docs/Build.md#使用已有安装)。创建 ZIP 需要 Python；可用 `PKNPACKER_PYTHON` 指定解释器路径：

```powershell
$env:PKNPACKER_PYTHON = 'C:\Python\python.exe'
.\Tools\Deploy.ps1 -SkipBuild
```

发布前检查文件摘要清单、许可和文档是否完整，并记录实际完成的验证范围。工具测试、原始加载指令对照和真实游戏验收的结论应分别说明。

## 提交问题与改进

问题反馈请包含客户端版本、实际包名、最小复现步骤、期望与实际结果，以及相关错误信息或 JSON 报告。反馈构建问题时，附上构建配置、Qt 路径和对应日志中的错误段落。

提交代码改动时，说明具体触发条件、修改后的行为及验证结果。格式、工作区或写入行为变更应运行相关核心测试；新增能力同时更新 README 或对应参考文档。文档改动应核对命令、相对链接及分发目录中的文档副本。
