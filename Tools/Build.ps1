param([ValidateSet('Debug','Release')][string]$Configuration = 'Release', [switch]$CoreOnly)
$ErrorActionPreference = 'Stop'
$pknProjectRoot = Split-Path -Parent $PSScriptRoot
$vswherePath = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$visualStudioPath = & $vswherePath -latest -products '*' -version '[17.0,18.0)' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $visualStudioPath) { throw '需要 Visual Studio 2022 的 C++ 桌面开发组件。' }
$msbuildPath = Join-Path $visualStudioPath 'MSBuild\Current\Bin\MSBuild.exe'
$pknLogDirectory = Join-Path $pknProjectRoot 'Artifacts\Logs'
New-Item -ItemType Directory -Path $pknLogDirectory -Force | Out-Null
$pknBuildTarget = if ($CoreOnly) { Join-Path $pknProjectRoot 'Source\PKNPacker.Core\PKNPacker.Core.vcxproj' } else { Join-Path $pknProjectRoot 'PKNPacker.sln' }
& $msbuildPath $pknBuildTarget /m "/p:Configuration=$Configuration" /p:Platform=x64 /v:minimal "/flp:logfile=$pknLogDirectory\${Configuration}Build.log;verbosity=normal"
if ($LASTEXITCODE -ne 0) { throw "$Configuration 构建失败，详见日志。" }
