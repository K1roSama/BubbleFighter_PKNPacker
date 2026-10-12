param(
    [switch]$SkipBuild,
    [ValidatePattern('^[0-9A-Za-z][0-9A-Za-z._+-]*$')]
    [string]$Version = '1.0.0'
)
$ErrorActionPreference = 'Stop'
$pknRoot = [System.IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot))
if (-not $SkipBuild) { & (Join-Path $PSScriptRoot 'Build.ps1') -Configuration Release }
$bin = Join-Path $pknRoot 'Artifacts\Bin\x64\Release'
$publishedDistribution = Join-Path $pknRoot 'Artifacts\Distribution\Release'
$distribution = [System.IO.Path]::GetFullPath((Join-Path $pknRoot ("Artifacts\Distribution\.staging-" + [guid]::NewGuid().ToString('N'))))
if (-not $distribution.StartsWith($pknRoot + '\', [System.StringComparison]::OrdinalIgnoreCase)) { throw '分发目录必须位于工程内。' }
New-Item -ItemType Directory -Path $distribution -Force | Out-Null
try {
    foreach ($name in 'PKNPacker.exe','PKNPacker.Cli.exe') {
        Copy-Item -LiteralPath (Join-Path $bin $name) -Destination (Join-Path $distribution $name) -Force
    }
    $qtRoot = if ($env:PKNPACKER_QT_ROOT) { $env:PKNPACKER_QT_ROOT } else { Join-Path $pknRoot 'Artifacts\Dependencies\Qt' }
    $qtDeployment = Join-Path $qtRoot 'bin\windeployqt.exe'
    & $qtDeployment --release --no-translations --no-opengl-sw --no-compiler-runtime --dir $distribution --plugindir (Join-Path $distribution 'plugins') (Join-Path $distribution 'PKNPacker.exe')
    if ($LASTEXITCODE -ne 0) { throw 'windeployqt 失败。' }
    New-Item -ItemType Directory -Path (Join-Path $distribution 'translations') -Force | Out-Null
    Copy-Item -LiteralPath (Join-Path $qtRoot 'translations\qtbase_zh_CN.qm') -Destination (Join-Path $distribution 'translations\qtbase_zh_CN.qm') -Force
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    $vs = & $vswhere -latest -products '*' -version '[17.0,18.0)' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (-not $vs) { throw '需要 Visual Studio 2022 的 C++ 桌面开发组件。' }
    $redist = Get-ChildItem -LiteralPath (Join-Path $vs 'VC\Redist\MSVC') -Directory | Where-Object { $_.Name -match '^14\.' } | Sort-Object Name -Descending | Select-Object -First 1
    if (-not $redist) { throw '缺少 v143 VC++ 可再分发运行库。' }
    Get-ChildItem -LiteralPath (Join-Path $redist.FullName 'x64\Microsoft.VC143.CRT') -Filter '*.dll' -File | ForEach-Object {
        Copy-Item -LiteralPath $_.FullName -Destination (Join-Path $distribution $_.Name) -Force
    }
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'DistributionREADME.md') -Destination (Join-Path $distribution 'README.md') -Force
    Copy-Item -LiteralPath (Join-Path $pknRoot 'LICENSE') -Destination (Join-Path $distribution 'LICENSE') -Force
    $notices = Get-Content -LiteralPath (Join-Path $pknRoot 'ThirdPartyNotices.md') -Raw -Encoding utf8
    $notices = $notices.Replace('(ThirdParty/Zlib/LICENSE)', '(Licenses/zlib-LICENSE.txt)').Replace('(ThirdParty/Json/LICENSE.MIT)', '(Licenses/json-LICENSE.MIT)')
    $notices = $notices.Replace('`ThirdParty` 下，便携分发沿用相同路径。', '`ThirdParty` 下，便携分发集中存放于 `Licenses`。')
    $notices | Set-Content -LiteralPath (Join-Path $distribution 'ThirdPartyNotices.md') -Encoding utf8
    $licenseDirectory = Join-Path $distribution 'Licenses'
    New-Item -ItemType Directory -Path $licenseDirectory -Force | Out-Null
    Get-ChildItem -LiteralPath (Join-Path $pknRoot 'Licenses') -File | ForEach-Object {
        Copy-Item -LiteralPath $_.FullName -Destination $licenseDirectory -Force
    }
    Copy-Item -LiteralPath (Join-Path $pknRoot 'ThirdParty\Zlib\LICENSE') -Destination (Join-Path $licenseDirectory 'zlib-LICENSE.txt') -Force
    Copy-Item -LiteralPath (Join-Path $pknRoot 'ThirdParty\Json\LICENSE.MIT') -Destination (Join-Path $licenseDirectory 'json-LICENSE.MIT') -Force
    @('[Paths]', 'Prefix=.', 'Plugins=plugins', 'Translations=translations') | Set-Content -LiteralPath (Join-Path $distribution 'qt.conf') -Encoding ascii
    $receipt = @(Get-ChildItem -LiteralPath $distribution -Recurse -File | Sort-Object FullName | ForEach-Object {
        [ordered]@{ path=$_.FullName.Substring($distribution.Length+1).Replace('\','/'); size=$_.Length; sha256=(Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant() }
    })
    $receipt | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $distribution 'DistributionManifest.json') -Encoding utf8
    $archive = Join-Path $pknRoot "Artifacts\Distribution\PKNPacker-$Version-Windows-x64.zip"
    $pknZipScript = @'
import json, pathlib, sys, zipfile
folder, target = map(pathlib.Path, sys.argv[1:])
manifest = json.loads((folder / 'DistributionManifest.json').read_text(encoding='utf-8-sig'))
with zipfile.ZipFile(target, 'w', compression=zipfile.ZIP_DEFLATED, compresslevel=6) as output:
    for item in manifest:
        output.write(folder / item['path'], item['path'])
    output.write(folder / 'DistributionManifest.json', 'DistributionManifest.json')
'@
    $pknPython = if ($env:PKNPACKER_PYTHON) { $env:PKNPACKER_PYTHON } else { 'python' }
    & $pknPython -c $pknZipScript $distribution $archive
    if ($LASTEXITCODE -ne 0) { throw '创建分发 ZIP 失败。' }

    # Remove only previously deployed files; preserve workspaces and user-added files.
    $oldManifest = Join-Path $publishedDistribution 'DistributionManifest.json'
    if (Test-Path -LiteralPath $oldManifest) {
        $oldFiles = @(Get-Content -LiteralPath $oldManifest -Raw -Encoding utf8 | ConvertFrom-Json | ForEach-Object {
            $oldPath = [System.IO.Path]::GetFullPath((Join-Path $publishedDistribution $_.path))
            if (-not $oldPath.StartsWith($publishedDistribution + '\', [System.StringComparison]::OrdinalIgnoreCase)) { throw '旧分发清单包含分发目录外路径。' }
            if (-not $oldPath.StartsWith((Join-Path $publishedDistribution 'Workspaces') + '\', [System.StringComparison]::OrdinalIgnoreCase)) { $oldPath }
        })
        foreach ($oldFile in $oldFiles) {
            if (Test-Path -LiteralPath $oldFile -PathType Leaf) { Remove-Item -LiteralPath $oldFile -Force }
        }
        Get-ChildItem -LiteralPath $publishedDistribution -Directory -Recurse | Sort-Object { $_.FullName.Length } -Descending | ForEach-Object {
            if ($_.FullName -ne (Join-Path $publishedDistribution 'Workspaces') -and
                -not $_.FullName.StartsWith((Join-Path $publishedDistribution 'Workspaces') + '\', [System.StringComparison]::OrdinalIgnoreCase) -and
                -not (Get-ChildItem -LiteralPath $_.FullName -Force | Select-Object -First 1)) {
                Remove-Item -LiteralPath $_.FullName -Force
            }
        }
    }
    New-Item -ItemType Directory -Path $publishedDistribution -Force | Out-Null
    Get-ChildItem -LiteralPath $distribution | ForEach-Object {
        Copy-Item -LiteralPath $_.FullName -Destination $publishedDistribution -Recurse -Force
    }
    Write-Output $archive
} finally {
    if (Test-Path -LiteralPath $distribution) { Remove-Item -LiteralPath $distribution -Recurse -Force }
}
