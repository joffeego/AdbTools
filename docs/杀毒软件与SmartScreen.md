# 杀毒软件报毒 / SmartScreen 拦截怎么办？

> 主 [README](../README.md) 的对应小节只给结论，这里是完整说明。

### 一句话结论

本程序**没有数字签名**。一个未签名的新文件在云信誉里是一张白纸，而它又确实会**调用子进程、联网下载、替换自身 exe** —— 这些行为在启发式规则里与恶意软件重叠。所以 Windows Defender 或第三方杀软**可能把安装包或主程序报成「木马/病毒」，这是误报（false positive），不是程序有毒。**

正确顺序是：**先用哈希核对文件确实来自本项目的 Release → 再把它加入白名单**（三步见下）。**不要**直接关掉杀毒软件。

### 为什么代码层面改到位了、还是会被报

从 0.9.7 起能做的都做了：可执行文件带版本信息、内嵌 manifest（`asInvoker` + DPI/长路径感知）、开启 ASLR、不再用 `powershell -ExecutionPolicy Bypass` 执行脚本、自更新与工具下载都校验 SHA-256、0.10.1 起主包不再包含任何第三方二进制、0.10.12 起安装包也带上了版本信息。**但这些只是减少「可疑特征」，消除不了误报**，原因是：

1. **没有数字签名**：杀软无法验证发布者，只能靠行为与信誉打分；
2. **信誉为零**：新发布的文件在 Defender 的云信誉里没有下载量、没有历史，默认从严；
3. **MOTW（来自互联网的标记）**：浏览器下载的文件带 `Zone.Identifier` 标记，SmartScreen 与部分杀软会因此额外警惕，解压出来的文件也会继承这个标记；
4. **行为特征与恶意软件重叠**：拉起子进程（`adb.exe` / `scrcpy.exe`）、联网下载、**把自己正在运行的 exe 替换掉**（自更新）；
5. **单文件自解压式安装包**（Inno Setup + LZMA 压缩）本身就是「打包器」特征；
6. **第三方二进制当替罪羊**：0.10.1 之前 adb/scrcpy 是打进主包的，而 `adb` 被很多杀软归为 `HackTool/RiskWare`，**Defender 报的是压缩包本身**，看起来就像我们的程序有毒。

### 程序里那些「看起来可疑」的行为，分别是在干什么

| 行为 | 用途 | 代码位置 |
| --- | --- | --- |
| 启动 `adb.exe` 子进程 | 列目录、上传下载、截图……整个程序的功能都靠它 | `core/process.cpp`、`core/adb.cpp` |
| 启动 `scrcpy.exe` 子进程 | 投屏 | `main.cpp` 的投屏部分 |
| 联网下载 | 只在你点「下载并安装 adb / scrcpy」「检查更新」时发生：Google 官方 platform-tools、GitHub Releases | `core/package.cpp` |
| 替换自身的 exe | 自更新：下载 zip → 校验 SHA-256 → 把旧 exe 改名、写入新 exe | `core/fileio.cpp` 的 `replaceFileOver()` |
| 写自己目录下的配置文件 | 设置/书签/命令/最近路径存在 exe 旁边，不写注册表、不写系统目录 | `core/store.cpp` |
| 退出时结束自己启动的 adb | 用 Job Object 保证不留下孤儿进程 | `core/process.cpp` |

程序**不写注册表（除安装包自身的卸载项）、不开机自启、不常驻后台**。

### 第 1 步：核对文件哈希（先做这个）

从 Release 页面下载时，每个文件旁边都有一个 `.sha256` 附件，内容是 `<哈希值>  <文件名>`：

```powershell
Get-FileHash .\AdbFileBrowser-windows-x64.zip -Algorithm SHA256
Get-Content .\AdbFileBrowser-windows-x64.zip.sha256
```

也可以直接和 GitHub 记录的摘要对比（不需要下载校验文件）：

```powershell
$rel = Invoke-RestMethod "https://api.github.com/repos/joffeego/AdbTools/releases/latest"
$rel.assets | Select-Object name, size, digest
```

两边一致 = 文件与官方发布完全一致、没有被替换或下载损坏；**这时候再加白名单才是安全的**。

### 第 2 步：Windows Defender 加白名单

**图形界面**：Windows 安全中心 →「病毒和威胁防护」→「病毒和威胁防护设置」下的「管理设置」→ 拉到底「排除项」→「添加或删除排除项」→「添加排除项」：

- 报的是**安装后的程序**：选「文件夹」，选 `C:\Users\<你>\AppData\Local\Programs\AdbTools`；
- 报的是**下载的安装包**：选「文件」，选 `AdbTools-Setup-x.y.z.exe`；
- 报的是**便携版**：选「文件夹」，选解压出来的 `AdbFileBrowser-windows-x64`。

**命令行方式**（管理员 PowerShell，效果相同）：

```powershell
Add-MpPreference -ExclusionPath "$env:LOCALAPPDATA\Programs\AdbTools"
Add-MpPreference -ExclusionPath "$env:USERPROFILE\Downloads\AdbTools-Setup-x.y.z.exe"
Get-MpPreference | Select-Object -ExpandProperty ExclusionPath   # 确认已生效
```

**如果开着「受控文件夹访问」（勒索软件防护）**：它会拦截程序替换自身 exe（也就是自更新）。安全中心 →「病毒和威胁防护」→「勒索软件防护」→「管理受控文件夹访问」→「通过受控文件夹访问允许某个应用」，把 `adb_browser.exe` 加进去。

### 第 3 步：SmartScreen 提示「Windows 已保护你的电脑」

- 点「**更多信息**」→「**仍要运行**」；
- 或者先解除「来自互联网」标记：右键文件 →「属性」→ 常规 → 勾选「解除锁定」→ 确定；等价命令：

```powershell
Unblock-File .\AdbFileBrowser-windows-x64.zip   # 或 .\AdbTools-Setup-x.y.z.exe
```

这一步只是去掉提示，**不代表文件安全** —— 所以请先做完第 1 步的哈希核对。

### 文件已经被隔离 / 删除了怎么办

安全中心 →「病毒和威胁防护」→「保护历史记录」→ 找到那条记录 →「操作」→「**允许在设备上**」（或「还原」）→ 然后再按第 2 步加排除项。只还原不加排除项的话，下次启动还会被删。

### 第三方杀软（火绒 / 360 / 管家 / 卡巴斯基 / ESET / Bitdefender / Avast / Norton / McAfee）

各家叫法不同，本质都是同一个东西：**信任区 / 白名单 / 排除项（Exclusions）**。操作套路一致：进设置 → 搜「信任」或「排除」→ 添加**文件**（安装包）或**文件夹**（程序目录）。常见入口：

| 软件 | 入口 |
| --- | --- |
| 火绒 | 设置 → 病毒防护 → 信任区 → 添加文件/目录 |
| 360 安全卫士 | 木马查杀 → 信任区（或 设置 → 白名单） |
| 腾讯电脑管家 | 病毒查杀 → 信任区 |
| 卡巴斯基 | 设置 → 安全威胁与排除 → 管理排除项 |
| ESET | 设置 → 检测引擎 → 排除 |
| Bitdefender | 保护 → 防病毒 → 设置 → 管理例外 |
| Avast / AVG | 设置 → 常规 → 排除项 |
| Norton / McAfee | 设置里搜索 Exclusions / 排除 |

这些软件一般也都有「误报反馈 / 上报样本」入口，把文件提交过去比只加白名单更彻底 —— 能帮到其他用户，也能让厂商修掉规则。

### 提交误报给微软（免费，最彻底）

1. 打开 <https://www.microsoft.com/en-us/wdsi/filesubmission>；
2. 身份选 **Software developer**（若是开发者）或 **Home customer**；
3. 上传被报的文件 —— 如果报的是整个 zip，就传 zip，并在说明里列出包内文件；
4. 提交类型选 **Incorrectly detected**（误报）；
5. 说明里写清：项目地址 `github.com/joffeego/AdbTools`、Apache-2.0 开源、构建由 GitHub Actions 公开可复现，并附上 SHA-256 和「保护历史记录」里的**完整检测名**（例如 `Trojan:Win32/Wacatac.B!ml` 这种）。

一般 24–48 小时内会更新特征库。如果你把**检测名 + 文件名 + 哈希**发到 [Issues](../../issues)，我也可以帮忙整理提交材料。

### 加白名单的代价（请看清楚）

把目录加入排除项 = **该目录下的文件不再被实时扫描**。所以顺序永远是「**先核对哈希与来源，再加白名单**」。本项目不提供、也不建议「关掉杀毒软件」这种做法；如果哈希对不上，请不要运行这个文件，而是到 [Issues](../../issues) 说明情况。
