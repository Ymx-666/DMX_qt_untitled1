# Windows 运行环境部署指南 (方案 A - MSVC 2013)

---

## 当前环境状态

| 组件 | 状态 | 路径 |
|------|------|------|
| **Qt 5.4.2** | ✅ 已安装 | `E:\.trae\qt5.4.2\5.4\msvc2013_64_opengl\` |
| **OpenCV** | ✅ 已部署 | `win_setup_test/3rdparty/opencv/build/` |
| **FFmpeg** | ✅ 已部署 | `win_setup_test/3rdparty/ffmpeg/` |
| **MSVC 2013 编译器** | ⏳ **待安装** | 需安装 Visual Studio 2013 或 Build Tools |

---

## 1. 获取 MSVC 2013 编译器

请安装 **Visual Studio 2013** 或 **MSVC 2013 Build Tools**：
- 下载地址：[Visual Studio 2013 官方下载](https://visualstudio.microsoft.com/vs/older-downloads/)
- 安装时请务必勾选 **"Visual C++"** 组件。

---

## 2. 编译项目

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
E:\.trae\qt5.4.2\5.4\msvc2013_64_opengl\bin\qmake.exe -spec win32-msvc2013
nmake
```

---

## 3. 运行时注意事项

- 可执行文件生成在 `release/` 或 `debug/` 目录
- 运行前请确保 DLL 路径已加入 PATH，或将 FFmpeg/OpenCV 的 `bin` 目录下的 `.dll` 文件复制到 `.exe` 同目录下
