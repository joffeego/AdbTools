# ADB 文件浏览器（AdbTools）

一个基于 [EUI-NEO](https://github.com/sudoevolve/EUI-NEO) 框架、用 C++17 编写的 **Android ADB 文件管理器 + 工具箱**。自带设备文件浏览、上传/下载、截图、APK 安装、应用管理、实时 logcat、scrcpy 投屏，以及 adb / scrcpy 的版本检测与一键更新。

- 界面：自绘无边框窗口 + 圆角 + 深色/浅色主题 + 自定义主题色 + 字体/字号/缩放设置
- 平台：Windows 10/11（MinGW-w64 编译）

---

## 截图

主界面（浅色主题）：

![ADB 文件浏览器主界面](screenshots/adbtools.png)

---

## 功能特性

- **设备管理**：自动探测 adb（SDK / PATH / 程序目录），列出并选择连接的设备，断线自动检测
- **文件管理**：浏览设备文件系统、多选、上传/下载、重命名、删除、新建文件夹、复制/移动（设备内剪贴板）、拖拽上传
- **路径导航**：可编辑路径栏、目录自动补全、后退/前进、快捷路径（书签）、显示隐藏文件、按名称/大小/日期排序
- **常用命令**：内置「adb shell」与「宿主命令」两类常用命令，可自定义增删
- **截图 / 安装 APK / 设备信息 / 无线连接**
- **应用管理**：列出第三方应用、卸载、清除数据
- **文本 / 图片预览**：直接预览设备上的文本和图片
- **实时 logcat**：持续滚动、默认清空历史、大小写不敏感的正则过滤
- **scrcpy 投屏**：内嵌到独立手机样式窗口，圆角、主题联动、窗口大小记忆
- **更新检测**：检测并一键更新 adb（Google 官方源）与 scrcpy（GitHub 官方源）
- **个性化**：深色/浅色主题、6 种主题色、系统字体/字重/字号、界面缩放（含动画）

---

## 下载与安装

到 [Releases](../../releases) 页面下载：

| 文件 | 说明 |
| --- | --- |
| `AdbTools-Setup-*.exe` | **推荐**。安装向导，自动创建开始菜单快捷方式（可选桌面快捷方式），并附带卸载程序。安装到用户目录（`%LOCALAPPDATA%\Programs\AdbTools`），无需管理员权限。 |
| `AdbFileBrowser-windows-x64.zip` | 便携版，解压后双击 `adb_browser.exe` 即可运行，无需安装。 |
| `AdbTools-tools-windows-x64.zip` | **可选**。adb + scrcpy（投屏用）。需要离线自带工具时再下，解压到程序目录即可（得到 `scrcpy\` 子目录）。 |

每个文件旁边都有对应的 `.sha256` 校验文件。

> **为什么 adb / scrcpy 不在主包里？**
> `adb.exe`、`scrcpy.exe` 和它们依赖的 DLL 是第三方二进制，**没有数字签名**。把它们一起打进压缩包，会让杀毒软件（尤其是 Windows Defender）把**整个下载包**报成病毒 —— 而且报的是压缩包本身，看起来就像我们的程序有毒。所以现在主包只装我们自己的程序，第三方工具单独发一个可选包。
> 程序在需要时会自己从官方源下载 adb / scrcpy，并校验官方公布的校验值，所以**完全不下这个可选包也能正常用**。

**首次启动会有一个「首次使用设置」对话框**：如果发现你缺 adb 或 scrcpy，它会列出两者的状态，并给「下载并安装」按钮，也可以点「全部下载」一次装好。装到程序目录，下载完立刻可用，不用重启。不想装投屏的话，勾上「不再提示」就不会再弹了。

> ⚠️ **0.10.2 有个「装上打不开」的严重问题，0.10.3 已修好。**
> 如果你的机器上 **PATH 里含有中文目录**（很常见，比如装了微信开发者工具），0.10.2 会一秒闪退、什么提示都没有。
> 原因是程序在启动时扫描 PATH 找 scrcpy，遇到中文目录时 `std::filesystem` 转换字符集失败并抛异常，而界面层是关掉异常编译的，异常逃出来就变成直接终止进程。0.10.3 改成不抛异常的方式查找，并加了回归测试。
> 这个问题其实从更早的版本就潜伏着（没有装 Android SDK 的中文 Windows 用户会一启动就崩），只是 0.10.2 才让它影响所有人。**请用 0.10.4 或更新版本。**

> 🔧 **0.10.4 修了两个关于 adb 更新的问题**（都会让人以为「更新 adb 失败」）：
> 1. 程序把 adb 的**协议版本**（`1.0.41`）当成了 platform-tools 的**修订号**去和 Google 公布的 `37.0.1` 比较，于是**永远认为 adb 需要更新**，每次检查都提示「更新到 37.0.1」——哪怕你已经是最新版。现在改成读 `adb version` 第二行的修订号（`Version 37.0.1-…`），装的是最新版就不会再提示。
> 2. 真要更新时，替换 `adb.exe` 会**失败**（提示文件被占用）：adb 服务端和残留的 adb 进程正在运行这个 exe，而 Windows 不允许覆盖正在运行的镜像 —— `adb kill-server` 也救不了（它只杀服务端，杀不掉残留的客户端进程）。现在改成**先把旧文件改名让开、再写入新文件**（Windows 允许重命名正在运行的 exe），并加了真实场景的回归测试。

> 🧹 **0.10.5：程序退出时会连带清理自己启动的 adb 子进程。**
> 以前如果程序崩溃、或者从任务管理器强杀，它启动的 `adb`（包括 adb 服务端）会变成孤儿进程一直留着。这些残留进程不只是占内存 —— 它们锁着 `adb.exe`，直接导致上面第 2 条那个「更新失败」。现在所有由本程序启动的子进程都被放进一个 Windows **Job Object**，程序一退出（正常关闭、崩溃、被强杀都算）系统就会把它们全部结束。
> **副作用（预期行为）**：如果 adb 服务端是**本程序启动的**，关掉程序后它会一起退出；下次谁需要 adb 时会自动重新拉起（Android Studio 等工具也会自动重启它）。如果是别的工具先启动的服务端，本程序不会去动它。

### 用电脑自己的程序编辑手机文件（0.10.6 新增）

手机上很多文件本程序没法编辑（Office 文档、PDF、代码、图片…）。现在在文件上**右键 → 「用电脑程序打开」**：

1. 程序把文件**下载**到本机（`程序目录\edit-cache\<设备>\<手机路径>`，保持原文件名，这样 Windows 才知道该用什么程序打开）；
2. 用你系统**默认关联的程序**打开它（.txt → 记事本、.jpg → 看图、.docx → Word……）；
3. **你在那个程序里一保存，改动就自动 `adb push` 回手机的原路径**（实测约 1–3 秒，程序在后台时稍慢），并提示「已同步到手机」。

工具栏上的**铅笔图标**打开「外部编辑」管理列表：每个文件显示状态（已同步 / 检测到改动 / 同步中 / 失败），可以「立即同步」或「停止」（停止前会把欠的改动先推回去）。可以同时编辑多个文件，各自监听、各自推回。

> 同步策略是**直接覆盖手机上的原文件**。如果手机上的同一文件在你打开之后又被别处改过，本地保存的内容会覆盖它。程序退出前会尽量把还欠着的改动推回去，避免刚改完就关程序而丢失。

> ⚠️ **0.10.6 还修了一个「同一操作只能用一次」的 bug。** 框架的异步任务队列把任务 key 当成一次性的：key 跑完就不再接受，而且是**静默拒绝**——不报错、不回调，调用方在调用前设的「忙」标志会永远卡住。结果是这些操作在一次运行里**只有第一次有效**：刷新设备、检查更新、上传、截图、安装/卸载 APP、粘贴、执行命令、保存编辑……现在都改成可重复执行。**如果你以前遇到过「点第二次没反应」，原因就是这个。**

> 🚀 **0.10.12：修「有的电脑打开特别慢」和一个「点了没反应」，并给安装包补上版本信息。**
> - **启动慢的元凶找到了并修掉**：启动时要找 adb / scrcpy，会逐个探测 PATH 里的目录，而**只要 PATH 里有连不上的网络位置**（公司共享盘、NAS、映射的网络盘符 —— 笔记本不在公司 / VPN 断开时就是这个状态），对它的**第一次**文件查询就会**卡满 TCP 连接超时**。实测把一个不可达地址（`\\192.0.2.201\share`，TEST-NET-1 保留网段）放进 PATH：
>   - 第一次查询 `GetFileAttributesW` 阻塞 **21026 毫秒**；同一台主机之后只要 0.1 毫秒（系统有负缓存）；**换一台不可达主机再卡 21 秒** —— 所以 PATH 里有几条这种条目，启动就有几个 21 秒。
>   - 用发布版实测（**直接启动 exe 并轮询窗口何时出现**，不经过 `cmd.exe`）：**0.10.9 的窗口要 21.3 秒才出现并响应；0.10.10 之后是 0.28 秒。** 用一个「本地已有 scrcpy.exe」的对照实验确认了卡住的正是 scrcpy 的 PATH 扫描（有本地 scrcpy 时 0.24 秒，去掉又变回 21.3 秒）。
>   - 现在会跳过**不会有 adb/scrcpy 的 PATH 条目**：UNC 网络路径、盘符后面没有驱动器的条目、以及**映射的网络盘符**（资源管理器里那个「红叉盘」，点它会卡很久的那个），并给整个扫描加 1.5 秒上限。副作用是「adb 装在映射网络盘上」这种极少见的情况不再被 PATH 扫描发现 —— 这种情况请用 `ANDROID_SDK_ROOT` 或把 adb 放到程序目录。
> - **顺便纠正我自己的一个测量错误**：早先用命令行 `adb_browser.exe version` 测，得出「2.0 秒 → 21.3 秒」，那个 21 秒其实是 **cmd.exe 自己的**开销 —— cmd.exe 启动时会扫描整个 PATH 建命令索引，遇到不可达的网络路径一样卡 21 秒（用 `where.exe` 做对照，同样 21.2 秒）。真正的程序在命令行下并不慢，因为本机有 Android SDK 的 adb，扫描会提前命中；卡的是图形界面的 scrcpy 扫描。**结论不变（这个 bug 是真的、现在修好了），但判断这类问题必须直接启动 exe 并测「窗口何时出现」。** 从 0.10.11 起 `tests/test_adbpath.cpp` 用不可达地址做回归测试：扫描一旦回退，测试会立刻失败。
> - **「点了没反应」现在会告诉你原因**：以前显卡侧初始化失败时程序是**静默退出**的（连错误都不显示），所以既看不出是杀软拦了、缺 DLL 还是显卡驱动太旧。现在会弹一个对话框，写明失败环节并列出常见原因（显卡驱动/OpenGL 3.3、杀软拦截、解压不完整），并提示可以用命令行 `adb_browser.exe devices` 确认程序本身能否运行。
> - **安装包现在也带版本信息了**：以前 `AdbTools-Setup-x.y.z.exe` 里**没有任何版本资源**（右键属性 → 详细信息是空的），未签名 + 无版本信息的组合正是杀软启发式最不喜欢的样子，也让人没法确认自己下的是哪一版。现在属性里能看到版本号、公司、产品名，并且发版流程会**断言**「程序与安装包的版本号必须等于 tag」，防止再出现「版本号漏改」。另外安装器现在**明确要求 Windows 10/11**：在更老的系统上会直接提示不满足要求，而不是装完打不开（本程序依赖 UCRT 和 OpenGL 3.3）。
> - 关于杀软：**0.10.1 起主包/安装包确实已经不含 adb/scrcpy 等第三方二进制**，所以如果还报毒，触发点就是**本程序自己的 exe 没有数字签名**（加上 Inno Setup 打包、联网、调用子进程这些行为特征）。这不是代码能消掉的，**唯一根治办法是代码签名**；短期请把文件提交到微软误报申诉入口，并把杀软报的**检测名+文件名**告诉我。

> 🪟 **0.10.9：窗口每次打开都居中，尺寸继续沿用上次。**
> - **位置永远重算**：以前窗口位置是"操作系统随便放"（级联、或同标题窗口的旧位置），换了尺寸/分辨率/显示器后就可能偏在角落甚至探出屏幕。现在**每次启动都把窗口放到工作区正中央**（工作区 = 排除任务栏后的区域），多显示器时放在它所在那台显示器的中间。位置**不记忆**——因为一旦记忆就会过期。
> - **尺寸仍然沿用上次**，并且修掉了一个「窗口每次启动都长大一圈」的 bug：保存的是窗口外框、而恢复时按客户区设置，两者差一个边框，于是**每次关掉再打开都会宽 15px、高 37px**（实测 5 次循环：900→915→929→943→957）。现在保存和恢复用同一套坐标，实测连续 5 次启动**稳定保持 900x600**，居中偏差 0px。
> - 顺带：窗口尺寸现在**改动后约 1 秒就自动存下**，不再只在点程序内的「关闭」按钮时才保存 —— 用 Alt+F4、任务栏关闭或崩溃退出也能记住。
> 🔧 **0.10.8：修掉设置里重叠的控件。** 0.10.7 加字体搜索框和「只显示支持中文的字体」勾选框时，把它们和「字体」标签放在了同一行却没有错开横向位置 —— 标签文字和勾选框文字**叠印在一起**。现在这一行明确分成三段互不重叠：`字体`（左侧）／勾选框（中间）／搜索框（右侧），并顺手让字体列表的高度自动贴合窗口，底部不再留空。
> 🎛 **0.10.7：设置界面的两处改进。**
> 1. **弹窗现在可以拖动**：按住弹窗顶部标题区域就能把它拖到旁边（所有弹窗都是），位置会记住；拖到屏幕外也会自动留一条边，不会拖丢。
> 2. **字体列表能用了**：以前是一个包含**全部 315 个已安装字体**的平铺列表，大多数是纯英文/符号字体，选错了界面上所有中文都会变成方框 —— 这就是「翻半天找不到正经字体」的原因。现在：
>    - 默认勾选**「只显示支持中文的字体」**（本机实测 315 → 52），选错字体不再会把界面搞坏；
>    - 顶部有**搜索框**，输入 `雅黑` / `yahei` / `noto` 都能直接过滤；
>    - 自动**合并冗余的字重变体**（如 "Noto Sans SC Light" 会被 "Noto Sans SC" + 字重选择器取代），本机列表最终 **31 个**，都是能用的中文字体；
>    - 列表上方显示「匹配 N / M 个字体」。

便携版解压后的目录结构：

```
AdbFileBrowser-windows-x64/
├── adb_browser.exe        # 主程序（自带 assets，界面字体/图标字体在内）
├── assets/                # 界面字体 / 图标字体
└── (scrcpy/)              # 可选：解压 AdbTools-tools 包后才会出现
```

运行前提：

1. Windows 10/11；
2. 手机开启「开发者选项 → USB 调试」，用数据线连接电脑并授权；
3. 也可以使用无线调试（`adb connect <IP>:<端口>`）。

> 说明：程序会自动寻找 adb，顺序是 `ANDROID_SDK_ROOT`/`ANDROID_HOME` → `%LOCALAPPDATA%\Android\Sdk` → `PATH` → 程序目录 → 程序目录下的 `scrcpy\`。
> 一个都找不到时，除了首次启动的设置对话框，界面也会显示「未找到 adb」并给两个按钮：**下载并安装 adb**（自动下载官方 platform-tools 到程序目录）和**手动选择 adb**。
> 之后想单独装 adb 或 scrcpy，点标题栏最左侧的更新按钮，里面两项各有一个「安装 / 更新」按钮。

---

## 关于数字签名（本软件未签名）

**当前发布的版本没有数字签名**，因此：

- Windows 可能显示「未知发布者」，SmartScreen 可能提示"已阻止"；
- 个别杀毒软件可能误报（原因、白名单步骤与误报申诉见下方「杀毒软件报毒 / SmartScreen 拦截怎么办？」）。

这是**已知且预期**的情况，不是程序有问题。代码签名证书需要付费或有资格门槛（开源项目证书只能签发给法律实体），本项目目前不打算引入签名；如果你介意这一点，可以从源码自行编译（见下方「从源码编译」），或按下方说明把程序加入杀软白名单。

**隐私政策**：本程序**不会**在未经用户明确要求的情况下向任何联网系统传输信息。所有网络访问都只在你主动操作时发生：检查/执行 adb、scrcpy、本软件的更新（访问 Google 官方仓库与 GitHub Releases），以及你自己触发的下载。第三方组件的隐私政策请参见各自项目主页（[scrcpy](https://github.com/Genymobile/scrcpy)、[Android SDK Platform-Tools](https://developer.android.com/tools/releases/platform-tools)）。

---

## 从源码编译

### 依赖

- [MinGW-w64](https://www.mingw-w64.org/)（GCC **12 或更新**，含 `mingw32-make`）；推荐 [WinLibs](https://winlibs.com/) 或 MSYS2
- [CMake](https://cmake.org/) 3.14+
- Git

### 编译步骤

```bash
# 1. 克隆仓库
git clone --recursive https://github.com/joffeego/AdbTools.git
cd AdbTools

# 2. 配置（生成 MinGW Makefiles）
cmake -S . -B build -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release

# 3. 编译（并行）
cmake --build build --parallel
```

编译产物为 `build/adb_browser.exe`，字体资源会自动拷贝到 `build/assets/`。

> `3rd/EUI-NEO/` 是**已随仓库一起分发的框架源码**（含本项目的少量定制修改），CMake 使用 `EUI_DEPS_MODE=bundled` 全离线编译，不需要联网拉取依赖。

### 运行

```bash
# 直接运行（会使用 PATH / SDK 中的 adb；找不到时界面会提供「下载并安装 adb」）
./build/adb_browser.exe

# 可选：把 adb/scrcpy 放到程序目录（投屏与 adb 探测需要），发布包里也是这么分的
./scripts/fetch_scrcpy.ps1 -Version latest -OutDir build/scrcpy
```

---

## 目录结构

```
AdbTools/
├── app_main.cpp            # 入口：有参数走命令行，无参数开图形界面
├── main.cpp                # 界面与交互（GUI 层）
├── cli/                    # 命令行模式（见 cli/README.md）
├── core/                   # 纯逻辑（无 UI 依赖，有单元测试）——见 core/README.md
├── tests/                  # core 的单元测试 + 真机端到端测试（ctest，无第三方框架）
├── regex_wrap.cpp          # std::regex 的异常隔离封装（logcat 过滤用）
├── adb_browser.rc          # Windows 资源脚本（版本信息 + 内嵌 manifest）
├── adb_browser.manifest    # 应用清单（asInvoker / longPathAware）
├── CMakeLists.txt          # 构建脚本
├── LICENSE                 # Apache-2.0
├── THIRD_PARTY_NOTICES.md  # 第三方组件与许可
├── scripts/
│   ├── fetch_scrcpy.ps1    # 下载并解压 scrcpy（内含 adb）
│   └── fix_ps1_bom.py      # 修复 .ps1 的 UTF-8 BOM（PS 5.1 没 BOM 会按 ANSI 读）
├── screenshots/            # README 截图
├── .github/workflows/      # CI（构建 + 单元测试）与发版
└── 3rd/EUI-NEO/            # 框架源码（bundled，含少量定制修改）
```

---

## 命令行模式

**同一个 exe，两种模式**：不带参数开图形界面，带参数则执行命令后退出。

```bash
adb_browser.exe                       # 图形界面
adb_browser.exe help                  # 命令行帮助
adb_browser.exe devices               # 列出设备
adb_browser.exe list /sdcard/Download # 列目录（含可写标志）
adb_browser.exe push ./a.apk /sdcard/ # 上传
adb_browser.exe pull /sdcard/a.txt .  # 下载
adb_browser.exe rm /sdcard/a.txt      # 删除
adb_browser.exe shell getprop ro.product.model
adb_browser.exe logcat -n 50              # 取最近 50 行日志
adb_browser.exe screenshot shot.png   # 截屏（实测输出为标准 PNG）
adb_browser.exe install app.apk       # 安装 APK（带 -r 覆盖）
adb_browser.exe packages --json       # JSON 输出，便于脚本处理
```

- 退出码：`0` 成功 / `1` 操作失败 / `2` 用法错误
- 多设备时用 `--serial <序列号>` 指定
- 批量命令（push / pull / rm）会**尝试全部参数**并汇总 `已完成 N，失败 M` —— 与图形界面的批量操作同一条规则
- 图形界面与命令行共用 `core/` 的全部逻辑，不存在两份实现

> Windows 上程序是 GUI 子系统（没有自己的控制台），命令行模式会挂到调用者的控制台上；如果输出已被重定向（管道/文件），则**不会**改动它，避免把输出从读取方抢走。

---

### 架构：`main.cpp` 与 `core/`

**`core/` 里放纯逻辑**：没有 UI、没有全局状态、没有平台头文件（`process`/`adbpath` 两个例外在 `core/README.md` 里说明了原因）。这条规则由 CI 强制检查（`core-purity` job），不是靠自觉。

这样做有三个直接好处：

1. **能写单元测试** —— 版本比较、更新包的 JSON/XML 解析、sha256 校验文件解析、原子写文件等，全部可以脱离界面验证；
2. **能查出真 bug** —— 补测试的过程里立刻暴露了 3 个问题：`parseSize()` 会接受负数、`adbShortVersion()` 大小写敏感导致漏读真实 adb 输出、以及更新 sidecar 回退路径以前从未被验证过。

运行测试：

```bash
cmake --build build --parallel
cd build && ctest --output-on-failure        # 单元测试 + 真机端到端测试
ctest -LE device                             # 只跑单元测试（不需要设备）
ctest -L device                              # 只跑真机测试（没插设备会自动跳过）
# 或直接看每个用例： ./adbtools_tests.exe
```

真机测试（`tests/device_tests.ps1`，42 条断言）用 CLI 驱动一台真实手机，覆盖设备列表、列目录、
push/pull/rm/mkdir、安装卸载、截屏、logcat 等；所有临时文件都在 `/sdcard/.__adbtools_e2e` 下并在结束时清理。

新增代码时：纯逻辑放 `core/`（并补测试），界面相关放 `main.cpp`。判断标准很简单 —— **如果一个函数需要"应用上下文"才能工作，它还不适合进 core，先拆分**。

---

## 更新机制

「检查更新」对话框（标题栏最左侧下载图标）会检测并更新三样东西：

- **本软件**：从你配置的 GitHub 仓库 `releases/latest` 获取最新版本，点击「更新到 X」会下载、解压并替换 `adb_browser.exe`（更新完成后重启生效）。下载的升级包会先与 Release 公布的 **SHA-256** 校验值比对，不一致就中止安装；
- **adb**：从 Google 官方仓库 `dl.google.com/.../repository2-1.xml` 解析最新 platform-tools 版本（同时读取官方公布的校验值和准确的 Windows 包地址）；
- **scrcpy**：从 GitHub Releases API 获取最新版本，并校验发布方公布的 SHA-256（更新前会先关闭投屏释放文件占用）。

> 📌 **发版时同步版本号**：`kAppUpdateRepo` 已指向本仓库 `joffeego/AdbTools`，自更新地址无需再改；
> 每次发版需要同步改 **5 个文件里的 6 处**版本号 —— 代码内唯一的来源是 `core/appinfo.h` 的 `kAppVersion`
> （图形界面和命令行都引用它），另外四处是 `adb_browser.rc` 的 `FILEVERSION/PRODUCTVERSION` 与版本字符串、
> `adb_browser.manifest` 的 `assemblyIdentity`、以及 `installer.iss` 的 `MyAppVersion`
> （这些格式没法 include 头文件，只能各留一份拷贝）。提交后打 `vX.Y.Z` 标签推送即可自动发版
> （CI 会在 Release 里附带每个安装包的 `.sha256` 校验文件）。
>
> 若网络无法访问 GitHub，对应行会显示「最新版本：未知」，不影响其它项。

---

## 杀毒软件报毒 / SmartScreen 拦截怎么办？

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
| 启动 `adb.exe` 子进程 | 列目录、上传下载、截图、logcat……整个程序的功能都靠它 | `core/process.cpp`、`core/adb.cpp` |
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

---

## 快捷键

| 按键 | 功能 |
| --- | --- |
| F5 | 刷新当前目录 |
| F2 | 重命名 |
| Backspace | 返回上级目录 |
| Enter | 进入选中的文件夹 |
| Delete | 删除选中项 |
| Ctrl+L | 编辑路径 |
| Esc | 关闭当前弹窗 |

---

## 第三方组件与许可

本软件以 **Apache License 2.0** 开源。它使用了以下第三方组件（详见 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)）：

- [EUI-NEO](https://github.com/sudoevolve/EUI-NEO)（Apache-2.0）—— UI 框架
- [scrcpy](https://github.com/Genymobile/scrcpy)（Apache-2.0）—— 投屏（运行时下载，不随源码分发）
- [Android SDK Platform-Tools (adb)](https://developer.android.com/tools/releases/platform-tools)（Apache-2.0）—— 调试桥（运行时下载，不随源码分发）
- 字体：Font Awesome 7 Free、JingNanJunJunTi、YouSheBiaoTiHei

---

## 免责声明

本项目仅供学习与合法的设备调试使用。请勿将其用于违反相关法律法规或侵犯他人权益的用途。使用本软件产生的一切后果由使用者自行承担。
