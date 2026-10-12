# BubbleFighter PKNPacker 便携版

用于浏览、导出、编辑和重新封装 BubbleFighter KR1304 的 PKN v2 资源包。

## 启动

解压整个压缩包到可写目录，运行 `PKNPacker.exe`。Qt 和 VC++ DLL、`plugins`、`translations` 与 `qt.conf` 是运行依赖，请与程序一起保留。

1. 点击“打开 PKN”选择资源包，或通过“扫描目录”加载多个包。
2. 浏览、搜索和预览文件，通过“导出选中文件”或“完整解包”导出资源。
3. 编辑资源后导入替换；点击“检查变更”，再通过“批量封包”生成新包。

默认工作区保存在程序旁的 `Workspaces` 目录。重封已有包时，需要保留原包的路径、名称和内容；修改包名需要通过工具重新封包。

## 命令行

在解压目录的 PowerShell 中运行：

```powershell
.\PKNPacker.Cli.exe list 'D:\Game\Packs'
.\PKNPacker.Cli.exe verify 'D:\Game\Packs' --report .\VerifyReport.json
.\PKNPacker.Cli.exe --help
```

完整用法见[使用指南](https://github.com/K1roSama/BubbleFighter_PKNPacker/blob/main/Docs/Usage.md)和[命令行参考](https://github.com/K1roSama/BubbleFighter_PKNPacker/blob/main/Docs/CommandLine.md)。兼容范围和验证边界见[验证说明](https://github.com/K1roSama/BubbleFighter_PKNPacker/blob/main/Docs/Validation.md)。

## 文件与许可证

分发包只包含 GUI、CLI、运行依赖、使用说明和许可证。测试程序、测试向量、源码文档和图片不进入便携版。

`DistributionManifest.json` 记录分发文件的大小和 SHA-256。项目许可证见 [LICENSE](LICENSE)，第三方说明见 [ThirdPartyNotices.md](ThirdPartyNotices.md)，第三方许可全文统一存放于 `Licenses`。
