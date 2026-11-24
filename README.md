# 墨行 Inkline 0.2

使用 C++17、Qt 6 Widgets 和 QScintilla 开发的多标签编辑器。Windows x64 用户可解压便携包并运行 Inkline.exe，请将同目录的 DLL 和插件一起保留。

从 InkEdit Native 升级时，首次启动会沿用原来的设置、搜索历史、已打开标签和未保存草稿。自定义配置目录可以通过 INKLINE_PROFILE 指定，也兼容旧环境变量 INKEDIT_NATIVE_PROFILE。

## 日常操作

- **修改配置**。右下角可选择语法、换行符和编码。点击 LF、CRLF 或 CR 会转换当前文档，保存后写入磁盘。编码菜单分别提供重新解码和保存时转换，支持 UTF-8、UTF-8 BOM、GBK、有／无 BOM 的 UTF-16LE 和 UTF-16BE，以及 Windows-1252。保存前会检查目标文件是否在其他程序中改动。
- **检查配置**。当前标签是 JSON、YAML 或 TOML 时，从编辑菜单选择“校验配置语法”查看错误位置，或选择“格式化配置”。格式化作用于编辑缓冲区，可以撤销，按保存后才写入文件。
- **批量改配置**。在编辑菜单打开“在文件夹中替换”（Ctrl+Shift+H），选择目录、文件匹配模式和替换规则。先生成预览，再逐文件或逐项勾选并应用。程序检查文件版本和打开的标签，跳过有未保存修改或已被外部改动的文件，并报告结果。
- **恢复草稿**。程序在后台保存打开的标签和修改过的内容，退出重启后可回到原处。关闭单个有改动的标签仍会询问是否保存。文件菜单可全部保存、查看最近文件、重新打开刚关闭的文件。
- **编辑和检索**。Ctrl+F 查找、Ctrl+H 替换，F3 和 Shift+F3 切换匹配。可按大小写、整词、正则检索，跨标签查找和文件夹搜索均可从编辑菜单进入。正则匹配使用独立进程，取消后可以立刻开始下一次搜索。在 macOS 使用系统对应快捷键。
- **检查改动**。编辑菜单可将当前缓冲区与磁盘或另一文件比较，也有行排序、去重、书签、列插入和宏回放。视图菜单提供缩进宽度、空白符、换行显示、关键词高亮、双栏阅读和双栏编辑。双栏编辑共享正文和撤销历史，两侧可独立滚动。工作区目录树可新建文件或文件夹、重命名和复制相对路径。
- **查看日志**。行号栏按原文件编号，折行时续行留空。普通 .log、.trace 和数字轮转日志可自动着色，其他文本可在状态栏手动选择“日志”。时间戳、级别、来源和键值字段分别显示。32 MiB 起自动只读，也可用文件菜单指定小日志。支持滚轮跨页、跟随最新、按字节／百分比／行号或完整日期时间跳转，以及 UTF-8 和 GBK。Ctrl+F 打开日志查找弹窗，文字查找支持继续扫描；筛选匹配行可启用正则、复制命中行、导出本批或导出全部并保留上下文。
- **追踪日志事件**。在只读日志窗口按 Ctrl+F，选择“筛选匹配行”。级别、来源、session_id、trace_id 和 request_id 可以与文字、正则和时间段组合使用，所有填写的条件同时生效。选中结果后可展开或复制完整事件；带行首时间戳的日志会把后续异常堆栈归入同一事件。
- **代码导航**。Go、Java、Rust 和 C/C++ 分别使用本机的 gopls、jdtls、rust-analyzer 和 clangd。可用 INKLINE_GO_LSP、INKLINE_JAVA_LSP、INKLINE_RUST_LSP、INKLINE_CPP_LSP 指定启动命令和参数的 JSON 数组。Python 代码菜单提供补全、定义、引用、签名和诊断。可自动发现 pylsp 或 pyright-langserver，也可用 INKLINE_PYTHON_LSP 指定启动参数的 JSON 数组。原生本地索引支持常见符号与导入场景，第三方库和动态类型的识别取决于连接的语言服务器。
- **AI 建议**。在“模型设置”选择或添加供应商，填写兼容 Chat Completions 的基础地址（如 https://example.com/v1）和 API Key，点击“获取模型”，选择后保存。也可手动输入模型 ID。可预览上下文、附加其他标签的代码、编辑回复并应用。在 AI 菜单启用“输入时显示 AI 灰字补全”后，已配置接口的代码和配置标签均可使用；支持行中和多行建议，Tab 接受整条，Alt+→ 接受下一个词，Esc 忽略。旧版授权仅发送光标前文，重新启用后可发送前后文，合计最多 4096 个字符。Key 只保存在本次运行的内存中。打包回归使用本机模拟服务验证流程。
- **检查安装包**。运行 `Inkline.exe --verify-install 检查结果.json` 可用临时配置验证窗口、编码转换、草稿恢复和图标。命令行测试驱动可在 tests 目录找到。

## 构建

Windows 使用同一个 MSYS2 UCRT64 前缀的 GCC、Qt 6 Base、Qt 6 5Compat 和 qscintilla-qt6，并安装 CMake、Ninja 和 Python 3（用于测试）。将前缀的 bin 加入 PATH。从项目根目录运行：

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

### 工程结构

- `src/main.cpp` 负责启动与命令行分发，`src/app/` 管理主窗口、文件、会话和安装自检。
- `inkcore` 处理文档、编码、配置解析及日志文件读取，`inklog` 提供日志显示与扫描，`inkcode` 集成 AI 与语言服务。
- `third_party/` 保存随源码分发的解析器及许可证。Qt 和 QScintilla 使用系统 SDK。
- `tests/` 通过 CTest 统一运行 C++ 回归和 Python 后台进程测试。仅构建发布程序可在配置时添加 `-DBUILD_TESTING=OFF`，无需 Python 或 Qt Test。

Windows 对比数据见 BENCHMARK.md。Windows 与 macOS 的构建、测试、打包和安装自检分别由 `.github/workflows/windows.yml` 和 `.github/workflows/macos.yml` 执行。
