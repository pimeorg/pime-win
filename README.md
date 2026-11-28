# PIME 拼音 · Windows TSF 输入法

Windows 平台的拼音输入法：输入法内核直接复用 `pime-core`（WAMR 运行插件链），
界面部分不引入任何 UI 框架，全部用原生 Win32 + Direct2D 绘制。

- TSF 文本服务（`ITfTextInputProcessorEx`）：应用内直接合成、提交、翻页、选词。
- 输入方案：全拼、双拼（小鹤 / 自然码 / 微软 / 搜狗）、五笔 86、仓颉，
  以及中文混输英文的「英文补全」；方案来自 `chains/*.yaml`，由插件链
  （`keymap` / `parse` / `translate` / `rank` / `format`）拼装。
- 候选条 / 模式气泡：单窗口分层窗口（`WS_EX_LAYERED` + `UpdateLayeredWindow`），
  Direct2D + DirectWrite 绘制，支持高 DPI、浅色/深色、悬停、鼠标点选；
  候选条可横排或竖排，字号分小 / 中 / 大三档。
- Shift 轻按切换中英文并弹出 `中` / `英` 气泡；PageUp/PageDown、上下键、
  `-` / `=`（含数字键盘）翻页；左右键在候选条上移动选中项（高亮显示），
  空格上屏选中项；数字键、鼠标也可选词。
- 设置窗口：左侧「常规 / 外观 / 关于」导航，可调输入方案、每页候选数、标点
  风格、候选排列与字号、启动时默认中文和主题。界面同样不依赖 UI 框架，用
  Direct2D + DirectWrite 自绘（卡片、分段控件、下拉菜单、开关、主题预览），
  支持键盘 Tab / Enter / Esc 操作。
- 中英文状态按应用分别记忆（`HKCU\Software\PIME\AppMode`），新应用默认使用
  设置里的「启动时默认中文」。
- 任务栏输入指示器显示当前状态的 `中` / `英` 图标（蓝色 / 灰色）；点击图标、
  轻按 Shift 或 Ctrl+Space 都能切换，图标随状态即时刷新。
- 组合文本带蓝色点状下划线；中文模式下标点默认转全角，覆盖
  `，。？！；：、《》（）【】｛｝～＠＃￥％＾＆＊－＿＋＝｜／｀`，双引号 / 单引号
  自动交替成对（`“”`、`‘’`）；设置里可改回英文半角；输入中按标点会先上屏
  候选词再输入标点，`v` 开头的表达式保留 ASCII 运算符。候选条显示时
  `-` / `=` 用于翻页，不再转成全角 `－` / `＝`。
- 安装器把输入法注册为 zh-CN 的键盘 TIP，并自动加入系统输入法列表。

## 目录

```
src/tsf        TSF 文本服务（注册、键事件、composition、显示属性）
src/ui         候选窗口与模式气泡（纯 Win32/D2D）
src/engine     pime-core 宿主封装（进程内 worker + 会话）
src/common     日志、设置（注册表）、Win32 工具
src/app        设置窗口与统一入口（--install/--demo/--keys）
src/demo       RichEdit 测试宿主（pime.exe --demo）
src/cli        内核探针（pime.exe --keys）
assets/chains  输入方案；assets/plugins 插件包
```

## 构建

依赖：Visual Studio 2022（MSVC x64）、CMake ≥ 3.25、Ninja；
以及同级检出的 `../pime-core` 与 `../pime-android`（提供 WAMR）。

```bat
build.bat
```

手动方式：

```bat
call "D:\vs\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

产物全部落在 `build\dist`：一个前端可执行文件 `pime.exe`、文本服务
`pime_tsf.dll`，以及 `chains/`、`plugins/`。

```bat
build\dist\pime.exe            :: 设置窗口
build\dist\pime.exe --install  :: 注册输入法（管理员）
build\dist\pime.exe --demo     :: 测试编辑器
build\dist\pime.exe --keys nihao  :: 无界面内核探针
```

## 试用

```bat
build\dist\pime.exe --demo --pime
```

`--pime` 仅对当前进程启用 PIME 拼音，便于开发验证，不改变系统默认输入法。

## 安装 / 卸载

安装与卸载需要管理员权限：

```bat
build\dist\pime.exe --install
build\dist\pime.exe --uninstall
```

直接运行 `pime.exe` 会打开设置窗口，可保存设置、安装/卸载，或点
「切换到 PIME」立即启用。安装后可在 Win+Space / 语言栏里选择「PIME 拼音」。

设置项保存于 `HKCU\Software\PIME`，运行日志写入
`%LOCALAPPDATA%\PIME\pime-<pid>.log`。

## 实现说明

- 内核静态链接进 `pime_tsf.dll`：TSF DLL 会被加载进任意宿主进程，不能依赖
  相对路径查找 `pime_core.dll`；插件链与插件包从 DLL 所在目录的
  `chains/`、`plugins/` 读取。
- 只有一个前端 exe：设置窗口、安装器、`--demo` 测试编辑器和 `--keys` 内核
  探针都从 `pime.exe` 进入；安装/卸载按钮用 `runas` 重新启动自身。
- 候选窗口不接收激活（`WS_EX_NOACTIVATE`），鼠标命中检测在分层位图上完成；
  模式气泡 900ms 后自动隐藏。
- 设置窗口没有任何原生控件：卡片、分段选择、下拉菜单、开关和主题预览全部
  用 Direct2D/DirectWrite 绘制，下拉菜单画在窗口内并处理悬停、点击与
  Tab/Enter/Esc 键盘操作。主题设置只影响候选窗口与气泡，设置窗口本身跟随
  系统深浅色。
- 任务栏的中/英图标是一个 TSF 语言栏项：`GetInfo` 返回
  `GUID_LBI_INPUTMODE`（Windows 8+ 的输入指示器只显示这一项），语言栏通过
  `ITfSource::AdviseSink` 挂上 `ITfLangBarItemSink`，切换模式后调用
  `OnUpdate(TF_LBI_ICON | TF_LBI_TEXT | TF_LBI_TOOLTIP)` 让它重取图标。
  `GetIcon` 每次从 `pime_tsf.dll` 资源重新 `LoadImage`，不能返回 `LR_SHARED`
  句柄：语言栏会 `DestroyIcon` 释放收到的图标。
- 中英文状态同时写进线程管理器的 `GUID_COMPARTMENT_KEYBOARD_OPENCLOSE`
  compartment：输入指示器、Ctrl+Space、触摸键盘以及通过 IMM32 兼容层查询
  `ImmGetOpenStatus` 的程序看到的是同一个状态。反向也成立——系统改变该
  compartment（Ctrl+Space、外部 IME 切换）时，`ITfCompartmentEventSink`
  通知我们把引擎、气泡和任务栏图标一起切过去。
- 组合文本的显示属性通过 `GUID_PROP_ATTRIBUTE` 设置，值是
  `ITfCategoryMgr::RegisterGUID` 返回的 atom（`VT_I4`），不是属性对象本身。
- 插件链在空格/回车/鼠标选词时会自行清空拼音缓冲，但数字键选词不会；
  提交后宿主统一 `Reset` 会话，避免下一键在旧拼音上继续组合。
- 中文标点由 TIP 处理：`v` 开头的数字/表达式模式保留 ASCII 运算符，
  组合中的 `'` 仍作为拼音分隔符（`xi'an`）。
- 组合期间把选区放在组合区间末尾：RichEdit 默认把光标留在组合起点，
  光标会停在第一个字母下面，和微软拼音的行为不一致。
- 注册表：`HKCR\CLSID\{...}\InprocServer32`（Apartment）、CTF 语言配置、
  TSF 分类；输入法列表项通过 `input.dll!InstallLayoutOrTip` 写入，
  卸载时以 `ILOT_UNINSTALL` 标志移除。由于该 API 在刚注册配置的同一进程内
  不生效，安装器会在注册完成后启动一次自身的 `--sync-input-list` 子进程，
  确保输入法列表立即出现。
- `assets/plugins/pime-plugin-pime-speech-sherpa-1.0.0` 只带 manifest、没有
  平台二进制，加载器会跳过它（日志中一条 `CORRUPT` 警告）；拼音输入不受影响。

## 已知限制

- Windows 11 的记事本等 MSIX 打包应用启用了注册表写入虚拟化：TIP 在宿主进程
  内写 `HKCU\Software\PIME` 时，改动只对该应用包私有（应用内切换中英文、
  重启后仍保持），设置窗口看到的是真实注册表的值，两者可能不同。
  普通未打包程序（以及设置窗口本身）没有这个隔离。
- 替换 `pime_tsf.dll` 后，已经在运行的程序仍使用内存里的旧模块：TSF 的进程内
  组件不能被热替换（实测宿主进程会一直持有已加载镜像，切走/切回输入法只会
  重建 TSF 对象，不会重新读磁盘；换文件名重新注册也一样）。重启这些程序或
  注销后即生效，新启动的程序会自动加载新 DLL。

## 许可证

本仓库以 [Apache License 2.0](LICENSE) 授权，版权归 pime 贡献者所有。
复用的 [pime-core](../pime-core)、[pime-public-plugin](../pime-public-plugin)
同样以 Apache 2.0 授权。
