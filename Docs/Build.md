# 构建与运行

本文介绍完整工具的首次构建、Qt 配置和运行方式。仅构建 Core 或 CLI、Debug 构建、测试与分发维护见[开发指南](../CONTRIBUTING.md)。

## 环境要求

| 组件 | 要求 |
| --- | --- |
| 操作系统 | Windows x64，已验证环境为 Windows 11 |
| 编译工具 | Visual Studio 2022，安装“使用 C++ 的桌面开发”，MSVC v143 |
| Windows SDK | `10.0.26100.0`，工程指定版本 |
| GUI 依赖 | Qt `6.11.1`（`msvc2022_64`）与 Qt/MSBuild `3.4.1` |
| 脚本环境 | Python，用于自动下载依赖和生成分发 ZIP |

项目使用 C++20 和原生 Visual Studio 工程。zlib `1.3.2` 与 nlohmann/json `3.12.0` 源码已包含在仓库中，无需初始化子模块。Core、CLI 和核心测试不依赖 Qt。

## 获取源码

在 PowerShell 中执行：

```powershell
git clone https://github.com/K1roSama/BubbleFighter_PKNPacker.git
cd .\BubbleFighter_PKNPacker
```

以下步骤均在仓库根目录运行。Qt 配置可选择自动下载或使用已有安装。

## 配置 Qt

### 自动下载

```powershell
python -m pip install requests py7zr
python .\Tools\SetupDependencies.py
```

脚本下载项目指定的 Qt 和 Qt/MSBuild，保存在 Git 忽略的 `Artifacts/Dependencies`。完成后直接进入构建步骤。

### 使用已有安装

复制本地配置示例：

```powershell
Copy-Item .\Build\Local.props.example .\Build\Local.props
```

在 `Build/Local.props` 中填写实际路径：

```xml
<?xml version="1.0" encoding="utf-8"?>
<Project xmlns="http://schemas.microsoft.com/developer/msbuild/2003">
  <PropertyGroup>
    <QtRoot>C:\Qt\6.11.1\msvc2022_64</QtRoot>
    <QtMsBuild>C:\QtMsBuild</QtMsBuild>
  </PropertyGroup>
</Project>
```

`QtRoot` 应包含 `bin/qmake.exe`；`QtMsBuild` 应直接包含 `qt.props` 和 `qt.targets`。本地配置已被 Git 忽略。

部署脚本还需设置同一套 Qt 的路径：

```powershell
$env:PKNPACKER_QT_ROOT = 'C:\Qt\6.11.1\msvc2022_64'
```

## 构建并运行

```powershell
.\Tools\Deploy.ps1
.\Artifacts\Distribution\Release\PKNPacker.exe
```

部署脚本先构建 Release，再收集 GUI、CLI、运行时依赖、便携版使用说明和许可证，生成：

| 产物 | 位置 |
| --- | --- |
| 可整体复制的分发目录 | `Artifacts/Distribution/Release` |
| 分发 ZIP | `Artifacts/Distribution/PKNPacker-1.0.0-Windows-x64.zip` |

GUI 使用流程见[使用指南](Usage.md)，CLI 用法见[命令行参考](CommandLine.md)。便携版的实际验证范围见[验证说明](Validation.md)。

便携版不包含测试程序、测试向量、贡献指南、源码文档或文档图片。Qt 插件集中放在 `plugins`，第三方许可集中放在 `Licenses`；程序旁的 Qt 和 VC++ DLL 是便携运行依赖。每次打包均使用全新暂存目录，ZIP 不收录旧文件或用户工作区。更新本地分发目录时，仅清理上一份清单中记录的文件，并保留 `Workspaces` 和用户新增文件。

## GitHub Actions 手动发布

将 `.github/workflows/release.yml` 推送到仓库默认分支后，在 GitHub 的 **Actions → Build and Release → Run workflow** 中选择要构建的分支或标签，并填写以下参数：

| 参数 | 说明 |
| --- | --- |
| `tag` | 必填的 Release 标签，例如 `v1.0.0`；只允许英文字母、数字、点、下划线、加号和连字符，以字母或数字开头，并须符合 Git 标签规则 |
| `release_name` | 可选标题，留空时使用 `PKNPacker <tag>` |
| `prerelease` | 勾选后标记为预发布版本，默认关闭 |
| `draft` | 勾选后创建草稿 Release，默认直接发布 |

工作流在 Windows Server 2022 上配置 Python、下载或恢复缓存中的 Qt 依赖，构建 x64 Release 并运行核心测试。测试通过后，部署脚本收集 GUI、CLI、Qt 与 VC++ 运行库、便携版使用说明和许可证，生成 `PKNPacker-<tag>-Windows-x64.zip` 及其 `.sha256` 校验文件，自动创建 Release、生成发布说明并上传两个附件。工作流使用仓库提供的 `GITHUB_TOKEN`，无需另外配置个人访问令牌。

不存在的标签会创建在本次运行选定的提交上；已有标签必须指向同一提交。每次发布请使用尚未创建 Release 的标签；已存在的 Release 不会被覆盖。若仓库或组织策略限制 `GITHUB_TOKEN` 写入，需允许此工作流写入仓库内容才能发布。

运行页面会保留分发包、构建日志和测试报告 14 天；构建或测试失败时仍会上传已经生成的日志和报告，并停止发布。

本地也可指定 ZIP 文件名中的版本，省略 `-Version` 时保持默认的 `1.0.0`：

```powershell
.\Tools\Deploy.ps1 -Version v1.0.0
```
