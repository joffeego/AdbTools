# 第三方组件声明（Third-Party Notices）

本软件（AdbTools）以 Apache License 2.0 开源，并使用了以下第三方组件。它们各自保留其原始许可与版权声明。

## 直接包含在源码仓库中的组件

### EUI-NEO
- 项目：https://github.com/sudoevolve/EUI-NEO
- 许可：Apache License 2.0
- 说明：本项目使用的 UI 框架，源码随仓库分发于 `3rd/EUI-NEO/`，并包含本项目的少量定制修改（无边框窗口、多窗口、字体热更新、异步关闭等）。

### 字体
- Font Awesome 7 Free（图标字体）：`3rd/EUI-NEO/assets/Font Awesome 7 Free-Solid-900.otf`
  - 许可：SIL OFL 1.1 / MIT（Free 版）
- JingNanJunJunTi、YouSheBiaoTiHei（界面字体）
  - 许可：以 EUI-NEO 随附字体分发

> 上述组件的详细许可文本位于各自源码目录下的 LICENSE / NOTICE 文件中。

## 运行时下载、不随源码分发的组件

这些二进制不进入 Git 仓库，由 `scripts/fetch_scrcpy.ps1` 或「检查更新」功能在构建/运行时从官方源下载。

### scrcpy
- 项目：https://github.com/Genymobile/scrcpy
- 许可：Apache License 2.0
- 下载源：https://github.com/Genymobile/scrcpy/releases

### Android SDK Platform-Tools（adb）
- 项目：https://developer.android.com/tools/releases/platform-tools
- 许可：Apache License 2.0
- 下载源：https://dl.google.com/android/repository/platform-tools-latest-windows.zip

---

如需完整的许可文本，请访问上述各项目链接。
