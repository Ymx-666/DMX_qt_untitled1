# DMX LaTeX 文档目录

这个目录用于保存项目后续所有 LaTeX 文档数据。

## 目录结构

- `main.tex`：主文档入口。
- `DMX说明文档.tex`：DMX 软件、硬件、算法和量化指标技术说明文档。
- `DMX软硬件框架图.tex`：可单独编译和导出的软硬件总体框架图。
- `references.bib`：参考文献数据库。
- `figures/`：图片、截图、实验结果图。
- `build/`：编译中间文件和 PDF 输出目录。
- `build.sh`：本地编译脚本。
- `build_documentation.sh`：说明文档独立编译脚本。
- `build_architecture.sh`：软硬件框架图 PDF/PNG 编译脚本。

## 编译

安装 LaTeX 环境后执行：

```bash
cd /home/sht/work/DMX_qt/latex
./build.sh
```

输出文件：

```text
/home/sht/work/DMX_qt/latex/build/main.pdf
```

编译说明文档：

```bash
cd /home/sht/work/DMX_qt/latex
./build_documentation.sh
```

输出文件：

```text
/home/sht/work/DMX_qt/latex/build/DMX说明文档.pdf
```

编译独立软硬件框架图：

```bash
cd /home/sht/work/DMX_qt/latex
./build_architecture.sh
```

输出包括 A4 横向 PDF、A4 PNG 和适合直接插入文档的裁剪版 PNG：

```text
/home/sht/work/DMX_qt/latex/build/DMX软硬件框架图.pdf
/home/sht/work/DMX_qt/latex/build/DMX软硬件框架图.png
/home/sht/work/DMX_qt/latex/build/DMX软硬件框架图-裁剪版.png
```
