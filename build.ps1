#!/usr/bin/env pwsh

param (
    [string]$QtDir = "C:/SoftWare/Qt/6.11.1/mingw_64",
    [string]$MinGbDir = "C:/SoftWare/Qt/Tools/mingw1310_64",
    [string]$CmakeDir = "",
    [string]$Prefix = "",
    [string]$ProjectDir = "",
    [string]$SubDir = "",
    [string]$Type = "Release",
    [string]$Compiler = "auto",
    [string]$Target = "",
    [string]$DeployDir = "",
    [switch]$Deploy,
    [switch]$DeployOnly,
    [switch]$Help
)

if ([string]::IsNullOrEmpty($ProjectDir)) { $ProjectDir = $PSScriptRoot }

if ($Help) {
    Write-Host ""
    Write-Host "使用说明:"
    Write-Host "  ./build.ps1 [Options]"
    Write-Host ""
    Write-Host "可选参数:"
    Write-Host "  -Compiler <type>      指定编译器类型 (mingw, msvc, 默认为 auto 自动识别)"
    Write-Host "  -ProjectDir <path>    指定 CMake 项目根目录（默认脚本所在目录）"
    Write-Host "  -SubDir <name>        指定仓库内子目录作为 CMake 项目根（等价于 -ProjectDir <repo>/<name>）"
    Write-Host "  -Target <name>        只编译指定 CMake 目标（如 ZzClipboard）；可传多个，用逗号分隔"
    Write-Host "  -Deploy               编译完成后，将应用及运行环境打包到独立目录"
    Write-Host "  -DeployOnly           跳过编译，仅对已有构建产物执行打包"
    Write-Host "  -DeployDir <path>     指定打包输出目录（默认 <项目>/dist/<目标>_<Type>）"
    Write-Host "  -Qt <path>            指定对应编译器的 Qt 库路径"
    Write-Host "  -MinGb <path>         (仅限 MinGW) 指定 MinGW 工具链路径"
    Write-Host "  -CmakeDir <path>      指定 CMake bin 目录 (默认自动从 Qt Tools 检测)"
    Write-Host "  -Type <type>          指定构建类型 (Debug 或 Release)"
    Write-Host ""
    Write-Host "使用示例:"
    Write-Host "  ./build.ps1"
    Write-Host "  ./build.ps1 -Target ZzClipboard"
    Write-Host "  ./build.ps1 -SubDir tools/helper -Target HelperTool"
    Write-Host "  ./build.ps1 -Deploy"
    Write-Host "  ./build.ps1 -DeployOnly -DeployDir dist/ZzClipboard_portable"
    Write-Host "  ./build.ps1 -Compiler msvc -Qt 'C:/SoftWare/Qt/6.7.0/msvc2019_64' -Deploy"
    Exit 0
}

if (-not [string]::IsNullOrWhiteSpace($SubDir)) {
    $ResolvedSubDir = Join-Path $PSScriptRoot $SubDir
    if (-not (Test-Path $ResolvedSubDir)) {
        Write-Error "[错误] 子目录不存在: $ResolvedSubDir"
        Exit 1
    }
    $ProjectDir = $ResolvedSubDir
}

if ([string]::IsNullOrEmpty($Prefix)) { $Prefix = $QtDir }
$ProjectDir = $ProjectDir -replace '\\', '/'
$QtDir = $QtDir -replace '\\', '/'
$MinGbDir = $MinGbDir -replace '\\', '/'
$CmakeDir = $CmakeDir -replace '\\', '/'
$Prefix = $Prefix -replace '\\', '/'
$DeployDir = $DeployDir -replace '\\', '/'

function Add-ToPath {
    param([string[]]$Dirs)
    $Valid = $Dirs | Where-Object { $_ -and (Test-Path $_) }
    if ($Valid) { $env:PATH = ($Valid -join ';') + ';' + $env:PATH }
}

function Require-Command {
    param([string]$Name, [string]$Hint)
    if (-not (Get-Command $Name -ErrorAction SilentlyContinue)) {
        Write-Error "[错误] 未找到 $Name。$Hint"
        Exit 1
    }
}

function Get-CMakeGenerator {
    param(
        [string]$SelectedCompiler,
        [bool]$HasNinja
    )

    if ($SelectedCompiler -eq "msvc") {
        if ($HasNinja) { return "Ninja" }
        return "Visual Studio 17 2022"
    }
    if ($HasNinja) { return "Ninja" }
    return "MinGW Makefiles"
}

function Test-IsMultiConfigGenerator {
    param([string]$Generator)
    return $Generator -match '^Visual Studio'
}

function Resolve-BuiltExecutable {
    param(
        [string]$BuildDir,
        [string]$Type,
        [string]$PreferredName
    )

    $searchRoots = @(
        $BuildDir,
        "$BuildDir/$Type",
        "$BuildDir/Release",
        "$BuildDir/Debug"
    ) | Select-Object -Unique

    if (-not [string]::IsNullOrWhiteSpace($PreferredName)) {
        foreach ($root in $searchRoots) {
            $candidate = "$root/$PreferredName.exe"
            if (Test-Path $candidate) {
                return (Resolve-Path $candidate).Path
            }
        }
    }

    foreach ($root in $searchRoots) {
        if (-not (Test-Path $root)) { continue }
        $exe = Get-ChildItem -Path $root -Filter *.exe -File -ErrorAction SilentlyContinue |
            Where-Object { $_.DirectoryName -notmatch 'CMakeFiles|_autogen|CompilerId' } |
            Sort-Object LastWriteTime -Descending |
            Select-Object -First 1
        if ($exe) {
            return $exe.FullName
        }
    }

    return $null
}

function Copy-IfExists {
    param(
        [string]$SourceDir,
        [string]$FileName,
        [string]$DestDir
    )

    $sourcePath = Join-Path $SourceDir $FileName
    if (Test-Path $sourcePath) {
        Copy-Item -Path $sourcePath -Destination $DestDir -Force
        return $true
    }
    return $false
}

function Invoke-QtPackage {
    param(
        [string]$ProjectDir,
        [string]$BuildDir,
        [string]$Type,
        [string]$QtDir,
        [string]$MinGbDir,
        [string]$SelectedCompiler,
        [string]$TargetName,
        [string]$OutputDir
    )

    $windeployqt = Join-Path $QtDir "bin/windeployqt.exe"
    if (-not (Test-Path $windeployqt)) {
        Write-Error "[错误] 未找到 windeployqt.exe: $windeployqt"
        Exit 1
    }

    $preferredExeName = if (-not [string]::IsNullOrWhiteSpace($TargetName)) {
        ($TargetName -split ',')[0].Trim()
    } else {
        ""
    }

    $builtExe = Resolve-BuiltExecutable -BuildDir $BuildDir -Type $Type -PreferredName $preferredExeName
    if (-not $builtExe) {
        Write-Error "[错误] 未在构建目录中找到可执行文件，请先编译或检查 -Target 参数。构建目录: $BuildDir"
        Exit 1
    }

    $exeFile = Get-Item $builtExe
    $packageLabel = if ($preferredExeName) { $preferredExeName } else { [System.IO.Path]::GetFileNameWithoutExtension($exeFile.Name) }

    if ([string]::IsNullOrWhiteSpace($OutputDir)) {
        $OutputDir = "$ProjectDir/dist/${packageLabel}_$Type"
    }
    $OutputDir = $OutputDir -replace '\\', '/'

    if (Test-Path $OutputDir) {
        Write-Host "[状态] 清理旧打包目录: $OutputDir"
        Remove-Item -Path $OutputDir -Recurse -Force
    }
    New-Item -ItemType Directory -Path $OutputDir -Force | Out-Null

    $targetExe = Join-Path $OutputDir $exeFile.Name
    Copy-Item -Path $exeFile.FullName -Destination $targetExe -Force
    Write-Host "[状态] 已复制可执行文件: $targetExe"

    $deployArgs = @(
        "--$($Type.ToLower())",
        "--compiler-runtime",
        "--no-translations",
        "--no-system-d3d-compiler",
        "--no-opengl-sw"
    )

    $deployArgs += $targetExe

    Write-Host "[状态] 正在收集 Qt 运行库 (windeployqt)..."
    & $windeployqt @deployArgs
    if (-not $?) {
        Write-Error "[错误] windeployqt 执行失败。"
        Exit 1
    }

    if ($SelectedCompiler -eq "mingw") {
        Write-Host "[状态] 正在复制 MinGW 运行库..."
        $mingwBin = Join-Path $MinGbDir "bin"
        $mingwDlls = @(
            "libgcc_s_seh-1.dll",
            "libgcc_s_dw2-1.dll",
            "libstdc++-6.dll",
            "libwinpthread-1.dll"
        )
        foreach ($dll in $mingwDlls) {
            if (Copy-IfExists -SourceDir $mingwBin -FileName $dll -DestDir $OutputDir) {
                Write-Host "  + $dll"
            }
        }
    }

    $sqlPlugins = Join-Path $QtDir "plugins/sqldrivers"
    if (Test-Path $sqlPlugins) {
        $destPlugins = Join-Path $OutputDir "plugins/sqldrivers"
        New-Item -ItemType Directory -Path $destPlugins -Force | Out-Null
        $sqlDriver = if ($SelectedCompiler -eq "mingw") { "qsqlite.dll" } else { "qsqlite.dll" }
        if (Copy-IfExists -SourceDir $sqlPlugins -FileName $sqlDriver -DestDir $destPlugins) {
            Write-Host "  + plugins/sqldrivers/$sqlDriver"
        }
    }

    Write-Host "[成功] 打包完成！独立运行目录: $OutputDir"
    Write-Host "       启动程序: $targetExe"
}

if (-not $DeployOnly -and -not (Test-Path "$ProjectDir/CMakeLists.txt")) {
    Write-Error "[错误] 未找到 CMakeLists.txt: $ProjectDir"
    Exit 1
}

$SelectedCompiler = $Compiler.ToLower()

if ($SelectedCompiler -eq "auto") {
    if ($QtDir -like "*msvc*") {
        $SelectedCompiler = "msvc"
    } else {
        $SelectedCompiler = "mingw"
    }
}

if (-not $DeployOnly) {
    if ($SelectedCompiler -eq "msvc") {
        Write-Host "[状态] 正在检测并载入 MSVC 环境..."
        $VsWherePath = "${env:ProgramFiles(x86)}/Microsoft Visual Studio/Installer/vswhere.exe"
        if (-not (Test-Path $VsWherePath)) {
            $VsWherePath = "${env:ProgramFiles}/Microsoft Visual Studio/Installer/vswhere.exe"
        }
        if (Test-Path $VsWherePath) {
            $VsInstallPath = & $VsWherePath -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
            if ($VsInstallPath) {
                $DevCmdPath = "$VsInstallPath/Common7/Tools/VsDevCmd.bat"
                if (Test-Path $DevCmdPath) {
                    $Vars = cmd /c "`"$DevCmdPath`" -arch=amd64 && set"
                    foreach ($Var in $Vars) {
                        if ($Var -match '^(.*?)=(.*)$') {
                            $Name = $Matches[1]
                            $Value = $Matches[2]
                            if ($Name -notin @("Prompt", "WINEVENT_IPC_NAME")) {
                                [System.Environment]::SetEnvironmentVariable($Name, $Value, [System.EnvironmentVariableTarget]::Process)
                            }
                        }
                    }
                    Write-Host "[成功] MSVC 环境变量已成功载入 (x64)"
                }
            }
        }
        if (-not (Get-Command cl -ErrorAction SilentlyContinue)) {
            Write-Error "[错误] 未能成功激活 MSVC 编译器 (cl.exe)，请确认是否安装了 Visual Studio C++ 开发组件。"
            Exit 1
        }
    }
    elseif ($SelectedCompiler -eq "mingw") {
        if (-not (Test-Path "$QtDir/bin")) { Write-Error "[错误] Qt bin 不存在: $QtDir/bin"; Exit 1 }
        if (-not (Test-Path "$MinGbDir/bin")) { Write-Error "[错误] MinGW bin 不存在: $MinGbDir/bin"; Exit 1 }
        Add-ToPath @("$QtDir/bin", "$MinGbDir/bin")
    }
}

$QtToolsDir = if ($MinGbDir) { Split-Path $MinGbDir -Parent } else { "C:/SoftWare/Qt/Tools" }
if ([string]::IsNullOrEmpty($CmakeDir)) { $CmakeDir = "$QtToolsDir/CMake_64/bin" }
Add-ToPath @($CmakeDir, "$QtToolsDir/Ninja", "$QtDir/bin")

$HasNinja = [bool](Get-Command ninja -ErrorAction SilentlyContinue)
$Generator = Get-CMakeGenerator -SelectedCompiler $SelectedCompiler -HasNinja $HasNinja
$IsMultiConfig = Test-IsMultiConfigGenerator -Generator $Generator

if (-not $DeployOnly) {
    Require-Command cmake "请通过 Qt Maintenance Tool 安装 CMake，或用 -CmakeDir 指定路径。"
    if ($Generator -eq "Ninja") {
        Require-Command ninja "请通过 Qt Maintenance Tool 安装 Ninja，或确认 Qt/Tools/Ninja 存在。"
    }
}

Set-Location $ProjectDir
$BuildDir = "$ProjectDir/build_$Type"

if (-not $DeployOnly) {
    Write-Host "====================================================================="
    Write-Host "                     Qt CMake 综合编译配置"
    Write-Host "====================================================================="
    Write-Host "[项目路径] : $ProjectDir"
    Write-Host "[编译器]   : $SelectedCompiler"
    Write-Host "[构建生成器]: $(if ($Generator) { $Generator } else { '系统默认' })"
    Write-Host "[构建模式] : $Type"
    if ($Target) { Write-Host "[编译目标] : $Target" }
    if ($SubDir) { Write-Host "[子目录]   : $SubDir" }
    Write-Host "====================================================================="

    Write-Host "[状态] 开始配置 CMake..."
    $CmakeConfigureArgs = @(
        "-S", $ProjectDir,
        "-B", $BuildDir,
        "-DCMAKE_PREFIX_PATH=$Prefix"
    )
    if (-not $IsMultiConfig) {
        $CmakeConfigureArgs += "-DCMAKE_BUILD_TYPE=$Type"
    }
    if ($Generator) { $CmakeConfigureArgs = @("-G", $Generator) + $CmakeConfigureArgs }
    & cmake @CmakeConfigureArgs 2>&1 | ForEach-Object { Write-Host $_ }
    if ($LASTEXITCODE -ne 0) { Write-Error "[错误] CMake 配置失败！"; Exit 1 }

    Write-Host "[状态] 开始编译项目..."
    $CmakeBuildArgs = @("--build", $BuildDir, "--parallel")
    if ($IsMultiConfig) {
        $CmakeBuildArgs += @("--config", $Type)
    }
    if (-not [string]::IsNullOrWhiteSpace($Target)) {
        foreach ($buildTarget in (($Target -split ',') | ForEach-Object { $_.Trim() } | Where-Object { $_ })) {
            $targetBuildArgs = $CmakeBuildArgs + @("--target", $buildTarget)
            Write-Host "[状态] 编译目标: $buildTarget"
            & cmake @targetBuildArgs 2>&1 | ForEach-Object { Write-Host $_ }
            if ($LASTEXITCODE -ne 0) { Write-Error "[错误] 编译目标 '$buildTarget' 失败！"; Exit 1 }
        }
    } else {
        & cmake @CmakeBuildArgs 2>&1 | ForEach-Object { Write-Host $_ }
        if ($LASTEXITCODE -ne 0) { Write-Error "[错误] 编译失败！"; Exit 1 }
    }

    Write-Host "[成功] 项目编译完成！构建产物在: $BuildDir"
}

if ($Deploy -or $DeployOnly) {
    Write-Host "====================================================================="
    Write-Host "                     Qt 应用打包"
    Write-Host "====================================================================="
    Invoke-QtPackage `
        -ProjectDir $ProjectDir `
        -BuildDir $BuildDir `
        -Type $Type `
        -QtDir $QtDir `
        -MinGbDir $MinGbDir `
        -SelectedCompiler $SelectedCompiler `
        -TargetName $Target `
        -OutputDir $DeployDir
}
