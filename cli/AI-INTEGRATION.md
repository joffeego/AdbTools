# 让其它 AI / 脚本调用 AdbTools CLI

**这是一份接口契约，不是使用教程。** 本文里每条命令、每个字段名、每个退出码、每段输出，
都在 `0.10.14` 的**真实构建**上跑过（有设备侧用假 adb 模拟，见第 7 节）。凡是我没有实测、
只从代码读出来的，都会明确标注。

给 AI 用的话，直接跳到最后 [第 9 节](#9-可直接粘贴给-ai-的提示词)：那里有一段可以整段复制的提示词。

---

## 1. 可执行文件

| 场景 | 路径 |
| --- | --- |
| 默认安装（Inno Setup） | `%LOCALAPPDATA%\Programs\AdbTools\adb_browser.exe` |
| 便携版（zip 解压） | `<解压目录>\adb_browser.exe` |
| 从源码构建 | `<build 目录>\adb_browser.exe` |

同一个 exe 有两种模式：

```
adb_browser.exe                     # 打开图形界面 —— 脚本里千万不要这样调
adb_browser.exe <命令> [参数...]     # 命令行模式，执行完退出
adb_browser.exe --cli               # 只打印命令行帮助，不开窗口
```

> ⚠️ **没有参数 = 开窗口**。脚本/集成里永远要带一个命令（或 `--cli`），否则会弹出一个 GUI 并一直挂着。

**冒烟测试**（任何集成第一步都该跑）：

```powershell
$exe = "$env:LOCALAPPDATA\Programs\AdbTools\adb_browser.exe"
& $exe version --json
# 期望：{"adb":"37.0.1","adbPath":"...","app":"0.10.14"}   ← 字段名见第 4 节
& $exe devices --json
# 期望：[{"serial":"...","state":"device","model":"...","product":"..."}]
```

`version` 不需要设备；`devices` 需要 adb。两者都不需要图形界面。

---

## 2. 调用约定（先读完这一节再写代码）

1. **输出编码是 UTF-8，行尾是 CRLF。**
   实测字节（`devices` 的输出）：
   `... 46 61 6B 65 5F 50 68 6F 6E 65 0D 0A` —— `0D 0A` 就是 CRLF。
   用 Python 要 `text=True, encoding="utf-8"`；用 Node 要 `buf.toString("utf8")`；
   用 PowerShell 读重定向文件要 `-Encoding utf8`。
   **用系统的 ANSI 代码页（简体中文机器上是 GBK/936）解码会得到乱码**，
   例如「全部成功（1 项）」会变成「鍏ㄩ儴鎴愬姛锛? 椤癸級」。这不是程序在输出乱码，是读的人解错了。
   拆行时记得去掉结尾的 `\r`。
2. **结果走 stdout，问题走 stderr。** 所以 `... --json > out.json` 只会拿到你要的数据，
   报错不会污染它（实测：`list /nope` 的 stdout 是空的，报错在 stderr）。
3. **退出码**：`0` 成功、`1` 操作失败、`2` 用法错误。
   **例外**：`shell` 直接把设备上那条命令的退出码原样返回，所以可能是任何值（见第 3 节）。
4. **`--json`、`--serial <序列号>` 写在命令名之后、任意位置都行**，解析器只认这两个开关。
   **其它 `--xxx` 会被当成位置参数**（例如 `list --json` 会被理解成"缺路径"→ 退出码 2，而不是"列当前目录"）。
5. **设备选择**：只连一台且状态是 `device` 时可以不写 `--serial`；连了多台而不写会**报错退出 1**，
   错误信息里列出候选序列号。写了 `--serial` 就照用（哪怕 adb 还没列出它，`adb connect` 之后立刻调用也行）。
6. **路径由你的 shell 负责引号**，程序把参数按 `argv` 原样转给 adb —— 除非是 `shell`：
   `shell` 会把命令名之后的**所有参数用空格拼成一整串**交给设备 shell，所以那里的引号由你控制。
7. **每条命令内部有超时，但没有超时参数。** 调用方自己设外层超时；具体数字见第 3 节末尾。

---

## 3. 命令参考

`-h` / `--help` 在任意命令后都能打印帮助；`help` 是独立命令。

| 命令 | 用法 | 设备 | `--json` 输出 |
| --- | --- | --- | --- |
| `devices` | `devices` | 需要 | 数组 |
| `list` | `list <远程路径>` | 需要 | 对象 |
| `push` | `push <本地文件...> <远程目录>` | 需要 | 批量结果对象 |
| `pull` | `pull <远程路径...> <本地目录>` | 需要 | 批量结果对象 |
| `rm` | `rm <远程路径...>` | 需要 | 批量结果对象 |
| `mkdir` | `mkdir <远程路径>` | 需要 | **什么都不打印** |
| `shell` | `shell <命令...>` | 需要 | **无效**（原样输出设备返回的文本） |
| `install` | `install <本地 apk>` | 需要 | 对象 |
| `uninstall` | `uninstall <包名>` | 需要 | **什么都不打印** |
| `packages` | `packages` | 需要 | 字符串数组 |
| `screenshot` | `screenshot [输出文件]` | 需要 | 对象 |
 |
| `version` | `version` | 不需要 | 对象 |

### devices

```console
$ adb_browser.exe devices
FAKEDEVICE001  device  Fake_Phone
$ adb_browser.exe devices --json
[{"serial":"FAKEDEVICE001","state":"device","model":"Fake_Phone","product":"fake"}]
```

文本模式是 `序列号  状态  型号`（型号可能为空）。`state` 常见值：`device`（可用）、
`offline`、`unauthorized`、`no permissions`。**只有 `device` 才能执行其它命令。**

没有任何设备时：**退出码仍然是 0**，文本模式打印 `(没有连接的设备)`，`--json` 打印 `[]`
（实测：4 字节 `[]` + CRLF）。所以"退出码 0"只代表命令跑通了，**必须再看列表里有没有可用设备**。

### list

```console
$ adb_browser.exe list /sdcard
d              Jan 1 12:00  DCIM
d              Jan 1 12:00  Download
$ adb_browser.exe list /sdcard/Download
-  5.2 KB  Jan 1 12:00  notes.txt
```

文本模式每行是 `类型  大小  修改时间  名称`：类型 `d` 目录 / `l` 符号链接 / `-` 普通文件 / `?` 其它；
目录的大小列是空白；符号链接会追加 ` -> 目标`。**这是给人看的格式，脚本请用 `--json`。**

路径不存在时：stderr 上是设备自己的报错（例如 `ls: /nope: No such file or directory`），
退出码 `1`，stdout 为空。

### push / pull / rm（批量）

一次可以给多个文件，**每个都会尝试**，不会因为一个失败就停下：

```console
$ adb_browser.exe push a.txt missing.txt /sdcard/CliTest
成功 1，失败 1："missing.txt"（找不到本地文件）        [退出码 1]

$ adb_browser.exe push a.txt /sdcard/CliTest           [退出码 0]
全部成功（1 项）
```

- `push` 的最后一个参数是设备上的**目录**，前面的都是本地文件。
- `pull` 的最后一个参数是本地**目录**（不存在会自动创建），前面的是设备路径。
- `rm` 的参数全是设备路径；目录会被递归删除。
- 文本摘要**永远会打印**（包括全部成功），所以"没有输出"不等于"成功"。

### mkdir / uninstall

```console
$ adb_browser.exe mkdir /sdcard/CliTest
已创建 /sdcard/CliTest
$ adb_browser.exe mkdir /sdcard/CliTest --json
（无输出，退出码 0）
```

**`--json` 加上去不会产生 JSON**：这两条命令的 JSON 模式只是"不打印人话"，靠退出码判断成败。
不要写"解析它的 JSON"的代码。

### shell

```console
$ adb_browser.exe shell echo hello
hello                     [退出码 0]
$ adb_browser.exe shell echo hello --json
hello                     [退出码 0]   ← --json 对 shell 没有作用
```

整串命令原样交给设备 shell，**stdout 就是设备命令的 stdout**（不做任何包装），
**退出码就是设备命令的退出码**（例如 `shell ls /nope` 会返回 `ls` 的退出码，通常 1 或 2）。
要结构化结果就在设备上自己输出 JSON，再用你的语言解析。

### install / packages / screenshot

```console
$ adb_browser.exe install app.apk --json
{"installed":"app.apk"}                 [退出码 0]
$ adb_browser.exe packages --json
["com.example.alpha","com.example.beta","org.fake.gamma"]
$ adb_browser.exe screenshot --json
{"file":"C:\\Users\\me\\Downloads/screenshot.png"}
```

- `install` 覆盖安装（`-r`）；APK 必须在本地存在，否则退出码 `1`（错误信息 `找不到 APK：<路径>`）。
- `packages` 只列**第三方**应用（`pm list packages -3`），已排序。
- `screenshot` 不写文件名时保存到**系统下载目录**的 `screenshot.png`，会覆盖同名文件。
  返回的是绝对路径（注意实测输出里用的是反斜杠加正斜杠混排：`C:\Users\me\Downloads/screenshot.png`，
  直接用 `Path` 类处理即可，不要手动拼字符串）。


### 内置超时（实测自 `cli/cli.cpp`）

| 命令 | 内部超时 | 命令 | 内部超时 |
| --- | --- | --- | --- |
| `devices` | 20 秒 | `install` | 300 秒 |
| `list` | 30 秒 | `uninstall` | 120 秒 |
| `push` | 600 秒 | `shell` | 120 秒 |
| `pull` | 600 秒 | `packages` | 30 秒 |
| `rm` | 60 秒 | `screenshot` | 60 秒（+ 拉取 120 秒） |
| `mkdir` | 30 秒 | `version` | 15 秒 |

---

## 4. JSON 契约（逐字段）

**所有 JSON 都压缩成一行**，字段名区分大小写。`size` 是数字（字节），其余字符串/布尔。

### `devices --json` → 数组

```json
[{"serial":"FAKEDEVICE001","state":"device","model":"Fake_Phone","product":"fake"}]
```

| 字段 | 类型 | 说明 |
| --- | --- | --- |
| `serial` | string | 传给 `--serial` 的值 |
| `state` | string | `device` 才可用 |
| `model` | string | 可能为空串 |
| `product` | string | 可能为空串 |

无设备时是 `[]`（退出码仍是 0）。

### `list --json` → 对象

```json
{"path":"/sdcard","writable":true,"entries":[
  {"name":"DCIM","dir":true,"link":false,"linkTarget":"","size":4096,"date":"Jan 1 12:00","perms":"drwxr-xr-x"}]}
```

| 字段 | 类型 | 说明 |
| --- | --- | --- |
| `path` | string | 原样回显你传的路径 |
| `writable` | bool | 目录可写才为 `true`；标记缺失时**保守为 `false`** |
| `entries[].name` | string | 名字（可能含空格，`ls` 里被拆开的会重新拼回） |
| `entries[].dir` | bool | 目录 |
| `entries[].link` | bool | 符号链接 |
| `entries[].linkTarget` | string | 链接目标，非链接时是空串 |
| `entries[].size` | number | 字节；目录通常是 4096（不要当真实大小用） |
| `entries[].date` | string | **设备 `ls` 的原始日期文本**（`Jan 1 12:00` 或 `2025-01-01 12:00`），没有归一化 |
| `entries[].perms` | string | 权限字符串 |

`.` 和 `..` 永远不出现。目录为空时 `entries` 是 `[]`。

### `push` / `pull` / `rm --json` → 批量结果对象

```json
{"total":2,"attempted":2,"succeeded":1,"failed":1,
 "failures":[{"label":"missing.txt","error":"找不到本地文件"}]}
```

| 字段 | 类型 | 说明 |
| --- | --- | --- |
| `total` | number | 请求的项数 |
| `attempted` | number | 真正尝试过的项数 |
| `succeeded` / `failed` | number | 成功 / 失败数 |
| `failures[].label` | string | 失败项的名字（文件基本名，不是完整路径） |
| `failures[].error` | string | 失败原因（可能是设备/系统的原文，语言不固定） |

**判定成败请用 `failed == 0` 或退出码**，不要靠字符串匹配 `全部成功` 这类中文。

### `install --json` / `screenshot --json` → 对象

```json
{"installed":"app.apk"}
{"file":"C:\\Users\\me\\Downloads/screenshot.png"}
```

（JSON 里的 Windows 路径是转义过的 `\\`，按 JSON 标准解析即可。）

### `packages --json` → 字符串数组

```json
["com.example.alpha","com.example.beta","org.fake.gamma"]
```

---

## 5. 错误处理（照这个判定树写）

```
退出码 0  → 成功。批量命令再看 failed 字段是否为 0。
退出码 2  → 你的参数写错了（命令名拼错 / 参数个数不对 / 把 --xxx 当成了开关）。
            这时 stderr 有「用法：...」或「未知命令：...」，不要重试，改代码。
退出码 1  → 环境或操作失败。stderr 上是原因，常见几类：
              · 找不到 adb。        → 提示装 platform-tools 或把 adb.exe 放到程序目录/PATH
              · 没有连接的设备。    → 提示插线并允许 USB 调试
              · 连接了多个设备，请用 --serial 指定：\n  <序列号1>\n  <序列号2>
              · 没有可用的设备（需要处于 device 状态）：\n  <序列号>  (unauthorized)
              · <设备自己的报错原文，例如 ls: /nope: No such file or directory>
其它值    → 只可能来自 shell（设备命令的退出码原样返回）。
```

实践建议：

- **不要解析中文提示来决定逻辑**，用退出码 + `--json` 字段；中文只用来给用户看。
- `unauthorized` 需要用户在手机上点授权弹窗，**重试没有意义**，应该提示用户。
- 设备名/包名/路径都来自设备，可能包含空格与任意 Unicode，**不要拼 shell 字符串**，
  按参数数组传（本文所有示例都是这么做的）。
- 需要"这台设备到底连上没有"的稳定判断：`devices --json` 里是否存在 `state == "device"`。

---

## 6. 各语言调用示例

> 所有示例都假设 exe 路径已确定；统一要点是：**参数按数组传、stdout 按 UTF-8 解码、看退出码**。

### PowerShell

```powershell
$exe = "$env:LOCALAPPDATA\Programs\AdbTools\adb_browser.exe"
$json = & $exe devices --json | Out-String
if ($LASTEXITCODE -ne 0) { throw "devices 失败：$LASTEXITCODE" }
$devices = $json | ConvertFrom-Json
$serial  = ($devices | Where-Object { $_.state -eq 'device' } | Select-Object -First 1).serial
& $exe list /sdcard --json --serial $serial | ConvertFrom-Json | ForEach-Object { $_.entries }
```

### Python

```python
import json, subprocess
EXE = r"C:\Users\me\AppData\Local\Programs\AdbTools\adb_browser.exe"

def cli(*args: str) -> tuple[int, str, str]:
    # encoding 必须是 utf-8：系统默认会用 GBK 解码，中文会变乱码
    p = subprocess.run([EXE, *args], capture_output=True, text=True, encoding="utf-8", timeout=120)
    return p.returncode, p.stdout, p.stderr

code, out, err = cli("list", "/sdcard", "--json")
if code != 0:
    raise RuntimeError(f"list 失败({code}): {err.strip()}")
for e in json.loads(out)["entries"]:
    print(("DIR " if e["dir"] else "FILE"), e["name"], e["size"])
```

### Node.js

```js
const { execFile } = require("node:child_process");
const EXE = String.raw`C:\Users\me\AppData\Local\Programs\AdbTools\adb_browser.exe`;

function cli(args, timeout = 120000) {
  return new Promise((resolve, reject) => {
    execFile(EXE, args, { encoding: "buffer", timeout, maxBuffer: 64 * 1024 * 1024 },
      (error, stdout, stderr) => {
        const out = stdout.toString("utf8"), err = stderr.toString("utf8");
        const code = error ? (error.code ?? 1) : 0;
        if (code !== 0 && typeof code !== "number") return reject(error);
        resolve({ code, out, err });
      });
  });
}

const { code, out, err } = await cli(["devices", "--json"]);
if (code !== 0) throw new Error(`devices 失败: ${err}`);
console.log(JSON.parse(out));   // [{ serial, state, model, product }]
```

### C# / .NET

```csharp
using System.Diagnostics;
using System.Text;

static (int Code, string Out, string Err) Cli(string exe, params string[] args)
{
    var psi = new ProcessStartInfo(exe) { RedirectStandardOutput = true, RedirectStandardError = true,
        StandardOutputEncoding = Encoding.UTF8, StandardErrorEncoding = Encoding.UTF8, UseShellExecute = false };
    foreach (var a in args) psi.ArgumentList.Add(a);   // ArgumentList 自动处理引号，别拼字符串
    using var p = Process.Start(psi)!;
    string o = p.StandardOutput.ReadToEnd(), e = p.StandardError.ReadToEnd();
    p.WaitForExit();
    return (p.ExitCode, o, e);
}

var (code, json, err) = Cli(exe, "list", "/sdcard", "--json");
if (code != 0) throw new InvalidOperationException(err);
```

### bash（Git Bash / WSL 调 Windows exe）

```bash
exe="/c/Users/me/AppData/Local/Programs/AdbTools/adb_browser.exe"
if ! out=$("$exe" devices --json); then echo "失败: $?" >&2; exit 1; fi
echo "$out" | python -c 'import json,sys; print([d["serial"] for d in json.load(sys.stdin) if d["state"]=="device"])'
```

---

## 7. 没有手机也能自测（假 adb）

仓库里带了一个**假 adb**（`tools/fakeadb.cpp`），它实现了 app 真正会调用的那部分协议，
并把手里的一个目录当成"手机"。这样集成测试不需要真机：

```powershell
# 1) 编译假 adb（任意 C++17 编译器；Windows 上 MinGW 或 MSVC 都行）
c++ -std=c++17 -O2 -static tools/fakeadb.cpp -o build/fakesdk/platform-tools/adb.exe

# 2) 造一部"手机"
New-Item -ItemType Directory -Force build\fake-phone\sdcard\Download | Out-Null
"hello" | Set-Content build\fake-phone\sdcard\Download\a.txt

# 3) 让 app 用假 adb、把那个目录当手机
$env:ANDROID_SDK_ROOT = "$PWD\build\fakesdk"     # app 从 SDK 目录找 adb
$env:FAKE_PHONE_ROOT  = "$PWD\build\fake-phone"  # "手机"的文件系统

# 4) 正常调用 CLI
& .\build\adb_browser.exe devices --json
& .\build\adb_browser.exe list /sdcard/Download --json
& .\build\adb_browser.exe push .\local.txt /sdcard/Download --json
& .\build\adb_browser.exe screenshot .\shot.png --json
```

假 adb 支持：`devices` / `version` / `shell cat|ls -la|mkdir|rm|pm list packages|screencap|echo` /
`push` / `pull`。**它的行为跟真机一致的部分**：列表格式、写入标记协议、
`push` 到目录补文件名、`ls` 不存在的路径返回非 0。**不一致的部分**：数据不真实（时间戳固定、
包名是假的）、没有权限/离线/未授权等真实故障模拟。所以它适合验证"我的代码有没有正确解析和判定"，
不适合验证"这根线插好了没有"。

---

## 8. 已知限制与坑

- **非 ASCII 文件名**：在 ANSI 代码页不是 UTF-8 的机器上（简体中文是 936），
  `adb push 中文名.txt` 会失败或把名字截断（`中文.`），拉丁字符也会掉最后一个字节（`café.txt` → `café.tx`）。
  这是 **adb 本身**在那种代码页下的行为，不是本程序能修的。集成时建议：要么用 ASCII 名，
  要么自己先把文件重命名再推。
- **大文件/目录树没有进度输出**：`push`/`pull` 只在结束时给结果。要进度就自己分片或改用
  `adb` 原生命令并自行解析。
- **没有递归目录 pull/push 的语义封装**：`pull <目录>` 依赖 adb 自己递归，程序不报告每文件结果。

- **`mkdir` / `uninstall` 的 `--json` 不输出 JSON**（见第 3 节）。
- **`--serial` 不校验设备是否存在**：写了就照用，所以 `list --serial 不存在的序列号` 会拿到 adb 的报错。
- **路径里的引号由调用方处理**：`adb_browser.exe` 自己不会帮你转义 shell 元字符
  （`shell` 除外，那一整串是交给设备 shell 的）。
- **中文提示文本可能随版本变化**：任何逻辑判断都请用退出码和 JSON 字段。

---

## 9. 可直接粘贴给 AI 的提示词

把下面整段（含本文路径）发给要写集成代码的 AI：

```text
请在我们的项目里接入 AdbTools 的命令行接口。它的接口契约在仓库文件 cli/AI-INTEGRATION.md 里，
请先完整读一遍，再动手写代码。要点（细节以那份文档为准）：

1. 可执行文件：%LOCALAPPDATA%\Programs\AdbTools\adb_browser.exe（便携版则在解压目录）。
2. 调用形式：adb_browser.exe <命令> [参数...] [--json] [--serial <序列号>]。
   绝对不要不带参数调用它 —— 那会打开图形界面。
3. 一律加 --json，用 JSON 字段判断结果；不要解析中文提示文本。
4. stdout 是 UTF-8 + CRLF，必须按 UTF-8 解码（Python 要显式 encoding="utf-8"，否则中文乱码）。
   结果在 stdout，报错在 stderr。
5. 退出码：0 成功 / 1 操作失败 / 2 用法错误；shell 会把设备命令的退出码原样返回。
6. 参数按数组传（不要拼 shell 字符串）；路径可能含空格与非 ASCII 字符。
7. 设备选择：只有一台可用设备时可省略 --serial；多台必须指定，否则退出码 1 并列出候选。
8. push/pull/rm 是批量命令：每个都会尝试，用 JSON 里的 failed / failures 判断部分失败。
9. 需要超时保护：外壳自己设超时（push/pull 内部上限 10 分钟，install 5 分钟）。
10. 没有真机时可以按 cli/AI-INTEGRATION.md 第 7 节用 tools/fakeadb.cpp 造一个假 adb 自测，
    但最终要在一台真机上跑一遍冒烟测试：version --json、devices --json、list /sdcard --json。
```

---

## 10. 本文档的验证方式（可复现）

本文所有输出都是实测，不是照代码猜的。复现步骤：

```powershell
# 用第 7 节的假 adb 环境，把每条命令的真实 stdout / stderr / 退出码都录下来
$exe = ".\build\adb_browser.exe"
foreach ($case in @(
    @('help'), @('version','--json'), @('devices','--json'),
    @('list','/sdcard'), @('list','/sdcard','--json'), @('list','/nope'),
    @('mkdir','/sdcard/CliTest'), @('mkdir','/sdcard/CliTest2','--json'),
    @('push','.\local.txt','.\missing.txt','/sdcard/CliTest','--json'),
    @('rm','/sdcard/CliTest','--json'), @('shell','echo','hello'),
    @('packages','--json'), @('install','.\dummy.apk','--json'),
    @('uninstall','com.example.alpha','--json'), @('screenshot','.\shot.png','--json'),
    @('list'))) {
  $o = "$env:TEMP\o.txt"; $e = "$env:TEMP\e.txt"
  $p = Start-Process $exe -ArgumentList $case -NoNewWindow -Wait -PassThru `
       -RedirectStandardOutput $o -RedirectStandardError $e
  "exit=$($p.ExitCode)  $($case -join ' ')"
  Get-Content $o -Raw -Encoding utf8      # ← 必须 utf8，否则中文乱码（这就是第 2 节的证据）
  Get-Content $e -Raw -Encoding utf8
}
```

写这份文档时顺带发现并修掉的一个真实缺陷（记录在此，便于以后回归）：
`list` 一条**不存在的路径**时，stderr 上除了设备自己的报错，还会多出两行内部协议文本
（`__WRITE__` 和可写标志 `1`）。现在在 `core::listingErrorText` 里统一裁掉，
GUI 和 CLI 都走这个函数 —— 也就是说这条命令的失败输出现在只有 `ls: ...` 一行。
