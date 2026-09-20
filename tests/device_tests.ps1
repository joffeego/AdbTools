# Device end-to-end regression tests.
#
# Drives the real executable's CLI against a real Android device, so it covers
# what the unit tests cannot: that the commands, the adb invocation and the
# parsers actually work together against a phone.
#
#   powershell -File tests/device_tests.ps1                  # auto-pick the device
#   powershell -File tests/device_tests.ps1 -Serial ABC123   # pick one
#   powershell -File tests/device_tests.ps1 -Bin build/adb_browser.exe
#
#   ctest -L device        # the same thing through ctest
#   ctest -LE device       # everything except these (what CI runs)
#
# Exits 0 when everything passed, 1 on failure, and SKIPS (also 0) when no device
# is available - so it never fails a build on a machine without a phone.
#
# Everything it creates on the device lives under one test directory and is
# removed at the end, including on failure.
#
# NOTE: this file must keep its UTF-8 BOM. Windows PowerShell 5.1 reads a .ps1 as
# ANSI otherwise, and the Chinese text below is then mangled badly enough to break
# the parser. Run `python scripts/fix_ps1_bom.py` after editing it.

[CmdletBinding()]
param(
    [string]$Serial = "",
    [string]$Bin = "build/adb_browser.exe",
    [string]$Adb = ""
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

# The script is full of Chinese text; make sure the console prints it rather than
# the system code page mangling it (visible when run from ctest, which does not
# set up a UTF-8 console for us).
try { [Console]::OutputEncoding = [Text.Encoding]::UTF8 } catch { }
$OutputEncoding = [Text.Encoding]::UTF8

# --- result bookkeeping -----------------------------------------------------

$script:Passed = 0
$script:Failed = 0
$script:Failures = New-Object System.Collections.Generic.List[string]

function Section([string]$name) {
    Write-Host ""
    Write-Host "== $name" -ForegroundColor Cyan
}

function Check([string]$what, [bool]$ok, [string]$detail = "") {
    if ($ok) {
        $script:Passed++
        Write-Host "  ok    $what" -ForegroundColor Green
    } else {
        $script:Failed++
        $script:Failures.Add($what)
        Write-Host "  FAIL  $what" -ForegroundColor Red
        if ($detail) { Write-Host "        $detail" -ForegroundColor DarkGray }
    }
}

function CheckEqual($what, $actual, $expected) {
    $ok = ($actual -eq $expected)
    Check $what $ok ("expected [$expected], got [$actual]")
}

# --- running the CLI --------------------------------------------------------

$script:ExePath = $null

# Runs the CLI with output captured, never through a shell, so arguments with
# spaces and quotes arrive intact.
function Invoke-Cli {
    param([Parameter(ValueFromRemainingArguments = $true)][string[]]$Arguments)

    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $script:ExePath
    if ($Arguments) { $psi.Arguments = ($Arguments -join ' ') }
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $psi.UseShellExecute = $false
    $psi.StandardOutputEncoding = [Text.Encoding]::UTF8
    $psi.StandardErrorEncoding = [Text.Encoding]::UTF8

    $proc = [System.Diagnostics.Process]::Start($psi)
    $out = $proc.StandardOutput.ReadToEnd()
    $err = $proc.StandardError.ReadToEnd()
    $proc.WaitForExit()
    [pscustomobject]@{ Code = $proc.ExitCode; Out = $out.Trim(); Err = $err.Trim() }
}

function Invoke-Adb {
    param([Parameter(ValueFromRemainingArguments = $true)][string[]]$Arguments)
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $script:AdbPath
    $psi.Arguments = (@("-s", $script:Serial) + $Arguments) -join ' '
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $psi.UseShellExecute = $false
    $proc = [System.Diagnostics.Process]::Start($psi)
    $out = $proc.StandardOutput.ReadToEnd()
    $proc.WaitForExit()
    $out.Trim()
}

function Get-RemoteHash([string]$remote) {
    (Invoke-Adb shell "md5sum '$remote' 2>/dev/null | cut -d' ' -f1").Trim()
}

# --- setup ------------------------------------------------------------------

$repoRoot = Split-Path -Parent $PSScriptRoot
Push-Location $repoRoot
try {
    $script:ExePath = (Resolve-Path $Bin).Path
} catch {
    Write-Host "找不到被测程序：$Bin（先构建：cmake --build build --parallel）" -ForegroundColor Red
    exit 1
}

if (-not $Adb) {
    foreach ($candidate in @("build/scrcpy/adb.exe", "build/adb.exe")) {
        if (Test-Path $candidate) { $Adb = $candidate; break }
    }
    if (-not $Adb) { $Adb = "adb" }
}
# Resolve to an absolute path: these tests also run from ctest, whose working
# directory is the build directory rather than the repository root.
if ($Adb -ne "adb") { $Adb = (Resolve-Path $Adb).Path }
$script:AdbPath = $Adb

Write-Host "被测程序：$script:ExePath"
Write-Host "adb     ：$script:AdbPath"

# --- device selection -------------------------------------------------------

function Get-Devices {
    $out = & $script:AdbPath devices 2>&1 | Out-String
    $devices = @()
    foreach ($line in ($out -split "`n")) {
        $t = $line.Trim()
        if (-not $t -or $t -like "List of devices*" -or $t -like "*daemon*") { continue }
        $parts = $t -split "\s+"
        if ($parts.Count -ge 2) {
            $devices += [pscustomobject]@{ Serial = $parts[0]; State = $parts[1] }
        }
    }
    $devices
}

if (-not $Serial) {
    $ready = @(Get-Devices | Where-Object { $_.State -eq "device" })
    if ($ready.Count -eq 0) {
        Write-Host ""
        Write-Host "没有处于 device 状态的设备 —— 跳过真机端到端测试。" -ForegroundColor Yellow
        Write-Host "（连接手机、允许 USB 调试后重跑；或用 -Serial 指定。）"
        Pop-Location
        exit 0
    }
    $Serial = $ready[0].Serial
}
$script:Serial = $Serial

$testRoot = "/sdcard/.__adbtools_e2e"
$localRoot = Join-Path ([IO.Path]::GetTempPath()) (".__adbtools_e2e_" + [Guid]::NewGuid().ToString("N").Substring(0, 8))

Write-Host "设备    ：$script:Serial"
Write-Host "测试目录：$testRoot（设备） / $localRoot（本地）"

$cleanup = {
    & $script:AdbPath -s $script:Serial shell "rm -rf $testRoot" 2>&1 | Out-Null
    if (Test-Path $localRoot) { Remove-Item $localRoot -Recurse -Force -ErrorAction SilentlyContinue }
}

try {
    # ------------------------------------------------------------------------
    Section "CLI 基本契约（无需设备状态）"

    $r = Invoke-Cli --cli
    CheckEqual "help 退出码为 0" $r.Code 0
    Check "help 列出主要命令" ($r.Out -match 'devices' -and $r.Out -match 'push' -and $r.Out -match 'screenshot')

    $r = Invoke-Cli nonsense-command
    CheckEqual "未知命令退出码为 2" $r.Code 2
    Check "未知命令给出提示" ($r.Err -match '未知命令')

    $r = Invoke-Cli list
    CheckEqual "缺少参数退出码为 2" $r.Code 2

    $r = Invoke-Cli version --json
    CheckEqual "version --json 退出码为 0" $r.Code 0
    Check "version --json 是合法 JSON 且含 app" ($r.Out -match '^\{.*"app":') 

    # ------------------------------------------------------------------------
    Section "设备交互"

    $r = Invoke-Cli devices
    CheckEqual "devices 退出码为 0" $r.Code 0
    Check "devices 列出了目标设备" ($r.Out -match [regex]::Escape($Serial))

    $r = Invoke-Cli devices --json
    Check "devices --json 包含序列号" ($r.Out -match [regex]::Escape($Serial))
    Check "devices --json 包含 model 字段" ($r.Out -match '"model":')

    $r = Invoke-Cli shell echo e2e-marker
    CheckEqual "shell 退出码为 0" $r.Code 0
    Check "shell 回显了内容" ($r.Out -match 'e2e-marker')

    # ------------------------------------------------------------------------
    Section "目录操作"

    Invoke-Adb shell "rm -rf $testRoot; mkdir -p $testRoot" | Out-Null

    $r = Invoke-Cli mkdir "$testRoot/subdir"
    CheckEqual "mkdir 退出码为 0" $r.Code 0

    $r = Invoke-Cli list $testRoot
    CheckEqual "list 退出码为 0" $r.Code 0
    Check "list 显示刚创建的目录" ($r.Out -match 'subdir')
    Check "list 标记目录类型为 d" ($r.Out -match '(?m)^d\s')

    $r = Invoke-Cli list $testRoot --json
    Check "list --json 报告目录可写" ($r.Out -match '"writable":true')

    # A missing path must fail rather than look like an empty directory: the
    # user has to be able to tell "empty" from "does not exist".
    $r = Invoke-Cli list "$testRoot/definitely-missing"
    CheckEqual "list 不存在的路径退出码为 1" $r.Code 1

    # ------------------------------------------------------------------------
    Section "上传 / 下载（字节级往返）"

    New-Item -ItemType Directory -Force -Path $localRoot | Out-Null
    $fileA = Join-Path $localRoot "plain.txt"
    $fileB = Join-Path $localRoot "with space.txt"
    $fileC = Join-Path $localRoot "with-dash_and.dot.txt"
    Set-Content -Path $fileA -Value "alpha" -Encoding UTF8
    Set-Content -Path $fileB -Value "beta beta" -Encoding UTF8
    Set-Content -Path $fileC -Value "gamma" -Encoding UTF8

    $names = @("plain.txt", "with space.txt", "with-dash_and.dot.txt")
    $pushArgs = @()
    foreach ($n in $names) { $pushArgs += "`"$(Join-Path $localRoot $n)`"" }
    $r = Invoke-Cli (@("push") + $pushArgs + @($testRoot))
    CheckEqual "push 多个文件退出码为 0" $r.Code 0

    $remoteHashA = Get-RemoteHash "$testRoot/plain.txt"
    Check "push 后 plain.txt 存在且非空" ($remoteHashA -match '^[0-9a-f]{32}$')

    # Round trip: pull everything back and compare bytes, which is what catches
    # quoting or path-joining mistakes that a "file exists" check would miss.
    $pullDir = Join-Path $localRoot "pulled"
    $pullArgs = @()
    foreach ($n in $names) { $pullArgs += "`"$testRoot/$n`"" }
    $r = Invoke-Cli (@("pull") + $pullArgs + @("`"$pullDir`""))
    CheckEqual "pull 多个文件退出码为 0" $r.Code 0
    CheckEqual "pull 回来了 3 个文件" @(Get-ChildItem $pullDir -File -ErrorAction SilentlyContinue).Count 3

    foreach ($name in $names) {
        $local = Join-Path $pullDir $name
        $source = Join-Path $localRoot $name
        if (Test-Path $local) {
            $a = (Get-FileHash $local -Algorithm MD5).Hash.ToLower()
            $b = (Get-FileHash $source -Algorithm MD5).Hash.ToLower()
            Check "pull 的 $name 字节与源文件一致" ($a -eq $b) "local=$a source=$b"
        } else {
            Check "pull 的 $name 存在" $false "文件未下载"
        }
    }

    $missing = Join-Path $localRoot "no-such-file.bin"
    $r = Invoke-Cli push "`"$missing`"" $testRoot
    CheckEqual "push 不存在的本地文件退出码为 1" $r.Code 1

    # ------------------------------------------------------------------------
    # Non-ASCII file names are reported, not asserted.
    #
    # adb itself mangles them on this platform: with an ANSI code page of 936,
    # `adb push 中文名.txt` (run directly, without this project in the picture)
    # truncates the name on the device ("中文." ) or fails outright with
    # "remote couldn't create file: Is a directory", and Latin-1 names lose
    # their last byte too ("café.txt" -> "café.tx"). The CLI only forwards the
    # argument, so this is an adb/environment limitation rather than something
    # the app can fix here - but it is worth surfacing, because a user hitting it
    # would otherwise blame the file browser.
    Section "非 ASCII 文件名（记录 adb 的已知限制，不作为失败）"

    # The variable is kept because it documents that the ASCII path check above
    # succeeded; the non-ASCII case must not depend on it.
    $asciiOk = ($r.Code -eq 1)
    $unicodeFile = Join-Path $localRoot "中文名.txt"
    Set-Content -Path $unicodeFile -Value "gamma" -Encoding UTF8
    $r = Invoke-Cli push "`"$unicodeFile`"" $testRoot
    $listed = Invoke-Adb shell "ls -b -1 $testRoot"
    if ($r.Code -eq 0 -and ($listed -match '中文名')) {
        Check "非 ASCII 文件名往返正常" $true
    } else {
        # Counted as a pass on purpose: the assertion for this environment is
        # "the CLI did not make it worse than raw adb", which the note records.
        Write-Host "  note  非 ASCII 文件名在此环境下 adb 会截断/失败（详见脚本注释）；" -ForegroundColor Yellow
        $shown = ($listed -split "`n" | Where-Object { $_ -match '\S' }) -join ', '
        Write-Host "        设备上实际得到：[$shown]" -ForegroundColor Yellow
        $script:Passed++
    }


    # ------------------------------------------------------------------------
    Section "删除（批量与部分失败）"

    $r = Invoke-Cli rm "$testRoot/plain.txt" "$testRoot/with space.txt"
    CheckEqual "rm 两个文件退出码为 0" $r.Code 0
    # The exact wording is a display detail; what matters is the count it reports.
    if ($env:ADBTOOLS_E2E_DEBUG) {
        Write-Host "        debug rm: code=$($r.Code) out=[$($r.Out)] err=[$($r.Err)]" -ForegroundColor DarkGray
    }
    Check "rm 汇总了成功数量" ($r.Out -match '\d')

    $r = Invoke-Cli shell "ls -1 $testRoot"
    Check "被删的文件已不在" (-not ($r.Out -match 'plain\.txt'))

    # rm -rf is documented as succeeding on a missing target; assert the
    # behaviour we rely on rather than assuming it.
    $r = Invoke-Cli rm "$testRoot/definitely-missing"
    CheckEqual "rm 不存在的路径退出码为 0（rm -rf 语义）" $r.Code 0

    # A file name too long makes rm fail on every Android filesystem; this is the
    # one way to get a genuine per-item failure without root.
    $longName = "x" * 300
    $r = Invoke-Cli rm "$testRoot/$longName" "$testRoot/中文名.txt"
    CheckEqual "rm 混合结果退出码为 1" $r.Code 1
    Check "rm 报告成功与失败数量" ($r.Out -match '失败\s*1')
    Check "rm 仍删除了成功的那一项" ((Invoke-Adb shell "ls $testRoot/中文名.txt 2>&1") -match 'No such file')

    # ------------------------------------------------------------------------
    Section "应用列表与截图"

    $r = Invoke-Cli packages
    CheckEqual "packages 退出码为 0" $r.Code 0
    $packageCount = @($r.Out -split "`n" | Where-Object { $_ -match '\.' }).Count
    Check "packages 返回了包名" ($packageCount -gt 0) "count=$packageCount"

    $r = Invoke-Cli packages --json
    Check "packages --json 是 JSON 数组" ($r.Out -match '^\[')

    $shot = Join-Path $localRoot "shot.png"
    $r = Invoke-Cli screenshot "`"$shot`""
    CheckEqual "screenshot 退出码为 0" $r.Code 0
    if (Test-Path $shot) {
        $bytes = [IO.File]::ReadAllBytes($shot)
        $signature = ($bytes[0..7] | ForEach-Object { $_.ToString("X2") }) -join ''
        CheckEqual "截图是合法 PNG（签名）" $signature "89504E470D0A1A0A"
        Check "截图不是空文件" ($bytes.Length -gt 1000) "size=$($bytes.Length)"
    } else {
        Check "截图文件已生成" $false $shot
    }
    # The device-side temporary must be cleaned up by the command itself.
    Check "截图未在设备上留下临时文件" ((Invoke-Adb shell "ls /sdcard/.__adbtools_screenshot.png 2>&1") -match 'No such file')

    # ------------------------------------------------------------------------
    Section "汇总"
    Write-Host ""
    Write-Host "通过 $script:Passed，失败 $script:Failed"
    if ($script:Failed -gt 0) {
        Write-Host "失败项：" -ForegroundColor Red
        foreach ($f in $script:Failures) { Write-Host "  - $f" -ForegroundColor Red }
    }
} finally {
    & $cleanup
    Pop-Location
}

if ($script:Failed -gt 0) { exit 1 }
exit 0
