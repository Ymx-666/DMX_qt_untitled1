# Windows 运行环境部署指南 (方案 A - MSVC 2013)

---

## 当前环境状态

| 组件 | 状态 | 路径 |
|------|------|------|
| **Qt 5.4 (MinGW 32-bit)** | ✅ 已安装 | `E:\qt5.4\5.4\mingw491_32\` |
| **Qt 5.4 (MSVC 2013 64-bit)** | ✅ 已安装 | `E:\.trae\Qt\5.4\msvc2013_64\` |
| **MSVC 2013 编译器** | ✅ 已安装 | `E:\.trae\Microsoft Visual Studio 12.0\` |
| **OpenCV / FFmpeg (MSVC x64)** | 可选 | 不影响编译；如启用需 `.lib/.dll` |

---

## 1. 获取 MSVC 2013 编译器

请安装 **Visual Studio 2013** 或 **MSVC 2013 Build Tools**：
- 下载地址：[Visual Studio 2013 官方下载](https://visualstudio.microsoft.com/vs/older-downloads/)
- 安装时请务必勾选 **"Visual C++"** 组件。

---

## 2. 安装 Qt 5.4 (MSVC 2013 64-bit)

- 安装 Qt 5.4 的 **MSVC2013 64-bit** 套件（例如 `E:\.trae\Qt\5.4\msvc2013_64\`）
- 确保 `qmake.exe` 存在于：
  - `...\msvc2013_64\bin\qmake.exe`

---

## 3. 编译项目

### 自动编译 (推荐)
双击运行 [build_msvc2013.bat](file:///e:/.trae/program/DMX_qt/untitled1/win_setup_test/build_msvc2013.bat)，脚本会自动：
1. 初始化 MSVC 2013 x64 编译环境
2. 调用 `qmake` 生成 Makefile
3. 调用 `nmake` 编译项目

### 手动编译
1. 打开 **"x64 Native Tools Command Prompt for VS 2013"**
2. 执行：
```bash
cd E:\.trae\program\DMX_qt\untitled1
E:\Qt\5.4\msvc2013_64\bin\qmake.exe -spec win32-msvc2013
nmake
```

---

## 4. 依赖库注意事项（OpenCV/FFmpeg）

当前工程对 OpenCV/FFmpeg 不做硬依赖（未配置时也可编译）。如需启用，请提供与 MSVC2013 x64 匹配的 `.lib/.dll` 并在 qmake 时传入路径变量。

---

## 5. 运行时注意事项

- 可执行文件生成在 `release/` 或 `debug/` 目录
- 运行前请确保 Qt/OpenCV/FFmpeg 的 `.dll` 文件与 `.exe` 位于同目录，或对应 `bin` 已加入 PATH
