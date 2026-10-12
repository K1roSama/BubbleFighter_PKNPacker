# 命令行参考

`PKNPacker.Cli` 提供资源浏览、导出、工作区封包和完整解码检查。构建与运行见[构建指南](Build.md)。

以下命令在仓库根目录运行，输入目录 `D:\Game\Packs` 请替换为实际本地路径：

```powershell
$cli = '.\Artifacts\Bin\x64\Release\PKNPacker.Cli.exe'
& $cli --help
```

在便携分发目录中运行时，将 `$cli` 改为 `'.\PKNPacker.Cli.exe'`。

帮助输出包含以下命令形式（节选）：

```text
PKNPacker.Cli — BubbleFighter PKN v2
  list <包...|Packs目录> [--entry 路径] [--prefix 目录] [--ext .xml]
  extract <包...|目录> --output 工作区根目录 [--overwrite] [--no-manifest]
  pack <工作区...> --output 输出目录 [--name Name_00000.pkn] [--overwrite]
  verify <包...|目录>
```

## 命令

| 命令 | 输入 | 结果 |
| --- | --- | --- |
| `list` | 一个或多个包，或包含 PKN 的目录 | JSON 条目列表，包含 ID、路径、大小、存储标志等信息 |
| `extract` | 包或目录，可指定选择器 | 导出资源；默认生成带清单的工作区 |
| `pack` | 一个或多个工作区，或通过 `--from-dir` 指定目录 | 检查变更，生成并验证新包 |
| `verify` | 一个或多个包，或包含 PKN 的目录 | 完整读取与解码，报告条目数、解码字节数和包 SHA-256 |

目录输入只扫描该目录下的 PKN，不递归扫描子目录。多个选择条件取并集；不指定选择器时，`list` 和 `extract` 处理全部条目。

### 浏览与导出

```powershell
# 列出所有包中的 XML 资源。
& $cli list 'D:\Game\Packs' --ext .xml --report .\XmlList.json

# 按内部路径导出单个文件。
& $cli extract 'D:\Game\Packs\Shader_00000.pkn' --entry Shader/Shader.vsh --target .\Shader.vsh

# 批量完整解包，每个包生成独立工作区。
& $cli extract 'D:\Game\Packs' --output .\Extracted --report .\ExtractReport.json

# 仅导出指定目录，生成部分工作区。
& $cli extract 'D:\Game\Packs\Shader_00000.pkn' --prefix Shader --output .\Selected
```

默认的批量解包布局为 `<输出根>/<包名不含扩展名>/Files/<内部路径>`，同一工作区根目录包含 `WorkspaceManifest.json`。使用 `--no-manifest` 时，资源直接导出到 `<输出根>/<包名不含扩展名>/<内部路径>`，不生成可重封清单。

`--target` 要求每个输入包的选择结果恰好只有一个条目，单文件导出建议只传入一个包。默认不覆盖已有文件；批量解包会记为跳过，单文件导出会失败。允许覆盖时显式添加 `--overwrite`。

### 检查与重封

```powershell
# 只检查已有工作区，不生成输出包。
& $cli pack .\Extracted\Shader_00000 --dry-run

# 导入替换内容并生成新包。
& $cli pack .\Extracted\Shader_00000 --replace 'Shader/Shader.vsh=.\EditedShader.vsh' --output .\Rebuilt

# 新增资源或登记删除已有资源。
& $cli pack .\Extracted\Shader_00000 --add 'Shader/Custom.vsh=.\Custom.vsh' --output .\WithCustom
& $cli pack .\Extracted\Shader_00000 --delete Shader/Shader.vsh --output .\WithoutShader

# 撤销指定资源的待提交操作。
& $cli pack .\Extracted\Shader_00000 --undo Shader/Shader.vsh --dry-run

# 多个工作区分别生成资源包。
& $cli pack .\Extracted\Shader_00000 .\Extracted\Config_00000 --output .\BatchRebuilt --report .\PackReport.json

# 使用新包名重新生成，不能直接重命名已有输出文件。
& $cli pack .\Extracted\Shader_00000 --name CustomShader_00000.pkn --output .\Renamed
```

显式编辑会保存到工作区清单，**包括 `--dry-run` 命令中的编辑**。应用顺序为撤销、替换、新增、删除；撤销恢复原映射，不会覆盖在 `Files` 中通过外部编辑器修改的内容。

封包默认拒绝覆盖已有输出；`--overwrite` 允许替换目标，但仍禁止覆盖输入原包。一个批次不能生成同名包，参与批次的工作区还会检查最终资源路径冲突。部分解包清单需要先补齐全部条目；源自原包的工作区始终需要可访问且内容未变的原包，用于核验身份。

### 从目录新建

```powershell
& $cli pack --from-dir .\CustomImages --namespace Images/Custom --name Custom_00000.pkn --workspace .\CustomWorkspace --output .\Rebuilt --mode 3
```

`--from-dir` 不能与已有工作区输入同时使用；必须提供 `--namespace`、`--name` 和 `--workspace`。输入目录中文件的相对路径会追加到命名空间下，例如 `icon.png` 对应 `Images/Custom/icon.png`。工作区会复制输入内容，后续可以继续打开、编辑和重封。

### 验证

```powershell
& $cli verify .\Rebuilt --report .\VerifyReport.json
```

`verify` 进行结构读取与完整解码。它不能仅靠包内信息检测未压缩正文的任意位损坏；重封过程会另外与预期内容 SHA-256 对照。索引密钥字段及验证能力的详细说明见 [PKN v2 格式](Format.md)。

## 选项

| 选项 | 适用命令 | 说明 |
| --- | --- | --- |
| `--entry 路径` | `list`、`extract` | 按完整内部路径选择，可重复指定 |
| `--id 数字` | `list`、`extract` | 按条目 ID 选择，可重复指定 |
| `--prefix 目录` | `list`、`extract` | 选择该路径及其下级目录，可重复指定 |
| `--ext 扩展名` | `list`、`extract` | 按扩展名选择，可带或不带 `.`，可重复指定 |
| `--output 目录` | `extract`、`pack` | 解包根目录或封包输出目录 |
| `--target 文件` | `extract` | 单文件导出目标 |
| `--no-manifest` | `extract` | 仅导出文件，不生成工作区清单 |
| `--replace 路径=文件` | `pack` | 导入替换内容，可重复指定 |
| `--add 路径=文件` | `pack` | 导入新增内容，可重复指定 |
| `--delete 路径`、`--undo 路径` | `pack` | 登记删除或撤销操作，可重复指定 |
| `--name Name_00000.pkn` | `pack` | 指定输出包名，只适用于单个工作区；新建时必填 |
| `--from-dir 目录` | `pack` | 从目录创建新包工作区 |
| `--namespace 路径` | `pack --from-dir` | 新资源的内部命名空间，必填 |
| `--workspace 目录` | `pack --from-dir` | 新工作区位置，必填 |
| `--mode 0–4` | `pack` | 新建或新增资源的存储模式，默认 `3` |
| `--dry-run` | `pack` | 检查变更并保存显式编辑，不生成输出包 |
| `--overwrite` | `extract`、`pack` | 明确允许覆盖目标文件 |
| `--logical-dir 目录` | `list`、`extract`、`verify` | 逻辑资源目录，默认 `packs`；重封使用工作区清单中的值，新建工作区固定使用 `packs` |
| `--jobs 数字` | 所有命令 | 独立任务并行数，范围 `1–8`，默认 `2` |
| `--report 文件.json` | 所有命令 | 保存独立 JSON 报告，禁止覆盖输入资源或工作区清单 |
| `--quiet` | 所有命令 | 关闭进度输出，保留 JSON 结果 |
| `--help` | 所有命令 | 显示帮助信息 |

存储模式：`0` 普通、`1` zlib、`2` 全部加密、`3` zlib 后全部加密、`4` 仅加密前 1024 字节。替换已有资源保留其存储标志；`--mode` 用于新建和新增资源。

## 报告与退出状态

正常命令的 JSON 结果输出到 stdout，进度输出到 stderr。指定 `--report` 时，另写一份 JSON 报告；报告使用 `PKNPacker.JobReport` schema、版本 `1`，包含 `results`、批次的 `tasks` 和 `exit_code`，预检失败时可能没有 `tasks`。任务数组记录成功、失败、跳过或取消及相应消息。

Ctrl+C 取消当前批次；一个包失败后，其他独立任务继续执行。取消或失败会清理未提交的临时包，已经完成的独立任务结果保留。

| 退出码 | 含义 |
| --- | --- |
| `0` | 成功，包含按策略跳过的结果 |
| `1` | 一个或多个任务失败，或报告写入失败 |
| `2` | 用法或预检失败 |
| `130` | 已取消 |

在 PowerShell 中可通过 `$LASTEXITCODE` 获取退出状态。完整工作区规则见[架构与工作区](Architecture.md)，兼容范围和验证记录见[验证说明](Validation.md)。
