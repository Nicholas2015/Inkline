# 墨行 Inkline 0.2

使用 C++17、Qt 6 Widgets 和 QScintilla 开发的多标签编辑器。Windows x64 用户可解压便携包并运行 Inkline.exe，请将同目录的 DLL 和插件一起保留。

从 InkEdit Native 升级时，首次启动会沿用原来的设置、搜索历史、已打开标签和未保存草稿。自定义配置目录可以通过 INKLINE_PROFILE 指定，也兼容旧环境变量 INKEDIT_NATIVE_PROFILE。

## 日常操作

- **修改配置**。右下角可选择语法、换行符和编码。点击 LF、CRLF 或 CR 会转换当前文档，保存后写入磁盘。编码菜单分别提供重新解码和保存时转换，支持 UTF-8、UTF-8 BOM、GBK、有／无 BOM 的 UTF-16LE 和 UTF-16BE，以及 Windows-1252。保存前会检查目标文件是否在其他程序中改动。
- **恢复草稿**。程序在后台保存打开的标签和修改过的内容，退出重启后可回到原处。关闭单个有改动的标签仍会询问是否保存。文件菜单可全部保存、查看最近文件、重新打开刚关闭的文件。
- **编辑和检索**。Ctrl+F 查找、Ctrl+H 替换，F3 和 Shift+F3 切换匹配。可按大小写、整词、正则检索，跨标签查找和文件夹搜索均可从编辑菜单进入。正则匹配使用独立进程，取消后可以立刻开始下一次搜索。在 macOS 使用系统对应快捷键。
- **检查改动**。编辑菜单可将当前缓冲区与磁盘或另一文件比较，也有行排序、去重、书签、列插入和宏回放。视图菜单提供缩进宽度、空白符、换行显示、关键词高亮和双栏阅读。
- **查看日志**。行号栏按原文件编号，折行时续行留空。普通 .log、.trace 和数字轮转日志可自动着色，其他文本可在状态栏手动选择“日志”。时间戳、级别、来源和键值字段分别显示。32 MiB 起自动只读，也可用文件菜单指定小日志。支持滚轮跨页、跟随最新、按字节／百分比／行号跳转，以及 UTF-8 和 GBK。Ctrl+F 打开日志查找弹窗，文字查找支持继续扫描；筛选匹配行可启用正则、复制命中行、导出本批或导出全部并保留上下文。
- **写 Python**。代码菜单提供补全、定义、引用、签名和诊断。可自动发现 pylsp 或 pyright-langserver，也可用 INKLINE_PYTHON_LSP 指定启动参数的 JSON 数组。原生本地索引支持常见符号与导入场景，第三方库和动态类型的识别取决于连接的语言服务器。
- **AI 建议**。配置兼容 Chat Completions 的服务地址与模型，按需提供 Key。可预览上下文、附加其他标签的代码、编辑回复并应用。行内续写按标签启用。Key 只保存在本次运行的内存中。打包回归使用本机模拟服务验证流程。
- **检查安装包**。运行 `Inkline.exe --verify-install 检查结果.json` 可用临时配置验证窗口、编码转换、草稿恢复和图标。命令行测试驱动可在 tests 目录找到。

## 构建

Windows 使用同一个 MSYS2 UCRT64 前缀的 GCC、Qt 6 Base、Qt 6 5Compat 和 qscintilla-qt6，并安装 CMake 和 Ninja。将前缀的 bin 加入 PATH。从项目根目录运行：

```powershell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=C:/msys64/ucrt64
cmake --build build
ctest --test-dir build --output-on-failure
./packaging/windows.ps1 -QtPrefix C:/msys64/ucrt64
```

macOS 使用对应架构的 Qt 6、Qt 6 Core5Compat 和 Qt 6 版 QScintilla。以下是 Homebrew 构建入口，尚待 macOS 实机验证：

```sh
brew install cmake ninja qtbase qt5compat qscintilla2
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH="$(brew --prefix qtbase);$(brew --prefix qt5compat);$(brew --prefix qscintilla2)"
cmake --build build
ctest --test-dir build --output-on-failure
"$(brew --prefix qtbase)/bin/macdeployqt" build/Inkline.app
```

源码按文档、编辑命令、搜索、日志、语言服务和 AI 分模块组织。Windows 对比数据见 BENCHMARK.md。macOS 自动构建和验证流程在 .github/workflows/macos.yml。
