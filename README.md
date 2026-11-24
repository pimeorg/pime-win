# PIME 拼音 · Windows TSF 输入法

Windows 平台的拼音输入法：输入法内核直接复用 `pime-core`（WAMR 运行插件链），
界面部分不引入任何 UI 框架，全部用原生 Win32 + Direct2D 绘制。

- TSF 文本服务（`ITfTextInputProcessorEx`）：应用内直接合成、提交、翻页、选词。
- 候选条 / 模式气泡：单窗口分层窗口（`WS_EX_LAYERED` + `UpdateLayeredWindow`），
  Direct2D + DirectWrite 绘制，支持高 DPI、浅色/深色、悬停、鼠标点选。
- Shift 轻按切换中英文并弹出 `中` / `英` 气泡；PageUp/PageDown、上下键翻页；
  数字键、鼠标或空格选词；每页候选数、输入方案、外观可在设置里调整。
- 中英文状态按应用分别记忆（`HKCU\Software\PIME\AppMode`），新应用默认使用
  设置里的「启动时默认中文」。
- 组合文本带蓝色点状下划线；中文模式下标点自动转全角（`，。？！；：` 等），
  输入中按标点会先上屏候选词再输入标点。
- 安装器把输入法注册为 zh-CN 的键盘 TIP，并自动加入系统输入法列表。

## 目录

```
src/tsf        TSF 文本服务（注册、键事件、composition、显示属性）
src/ui         候选窗口与模式气泡（纯 Win32/D2D）
src/engine     pime-core 宿主封装（进程内 worker + 会话）
src/common     日志、设置（注册表）、Win32 工具
src/app        设置窗口与 --install/--uninstall 入口
src/demo       RichEdit 测试宿主（仅测试用）
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

产物全部落在 `build\dist`（`pime_tsf.dll`、`pime-settings.exe`、
`pime-demo.exe`、`pime-cli.exe` 以及 `chains/`、`plugins/`）。

## 试用

```bat
build\dist\pime-demo.exe --pime
```

`--pime` 仅对当前进程启用 PIME 拼音，便于开发验证，不改变系统默认输入法。

## 安装 / 卸载

安装与卸载需要管理员权限：

```bat
build\dist\pime-settings.exe --install
build\dist\pime-settings.exe --uninstall
```

也可以直接运行 `pime-settings.exe` 打开设置窗口，在窗口里保存设置、安装/卸载，
或点「切换到 PIME」立即启用。安装后可在 Win+Space / 语言栏里选择「PIME 拼音」。

设置项保存于 `HKCU\Software\PIME`，运行日志写入
`%LOCALAPPDATA%\PIME\pime-<pid>.log`。

## 实现说明

- 内核静态链接进 `pime_tsf.dll`：TSF DLL 会被加载进任意宿主进程，不能依赖
  相对路径查找 `pime_core.dll`；插件链与插件包从 DLL 所在目录的
  `chains/`、`plugins/` 读取。
- 候选窗口不接收激活（`WS_EX_NOACTIVATE`），鼠标命中检测在分层位图上完成；
  模式气泡 900ms 后自动隐藏。
- 组合文本的显示属性通过 `GUID_PROP_ATTRIBUTE` 设置，值是
  `ITfCategoryMgr::RegisterGUID` 返回的 atom（`VT_I4`），不是属性对象本身。
- 插件链在空格/回车/鼠标选词时会自行清空拼音缓冲，但数字键选词不会；
  提交后宿主统一 `Reset` 会话，避免下一键在旧拼音上继续组合。
- 中文标点由 TIP 处理：`v` 开头的数字/表达式模式保留 ASCII 运算符，
  组合中的 `'` 仍作为拼音分隔符（`xi'an`）。
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
