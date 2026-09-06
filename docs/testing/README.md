# DMX 测试入口

## 1. 快速测试

```bash
./scripts/test_fast.sh
```

覆盖内容：

- 跟踪 JSON 文件语法；
- 跟踪 Shell 脚本语法；
- Python 模块编译和 `pytest`；
- AppConfig、PanoramaCache、ManualNegativeStore、CompactTargetRadarPanel 四组 QtTest；
- Git 空白错误检查。

## 2. 双程序构建

```bash
./scripts/test_build.sh
```

脚本在独立目录中从头构建 `DMX` 和 `DMX_test`，不会覆盖桌面启动脚本使用的现有二进制文件。

## 3. 本地回放冒烟测试

```bash
./scripts/test_replay_smoke.sh
```

默认运行 45 秒。脚本启动本地 UDP 回放发送端和离屏 `DMX_test`，检查：

- 回放控制应答；
- RGB/BW 图像通知接收；
- Direct YOLO worker 就绪；
- 图像读取失败计数保持为零。

该测试依赖 2026-07-23 原始日志、本地回放图像、预生成天空 Mask、weights5 ONNX 和项目 Python 虚拟环境。任何依赖缺失都会明确失败，不会作为通过或静默跳过。路径可通过脚本 `--help` 中列出的环境变量覆盖。

需要先重建 `DMX_test` 时执行：

```bash
./scripts/test_replay_smoke.sh --build
```

## 4. 结果目录

默认结果目录：

```text
test-results/<Run ID>/<suite>/
```

可通过 `DMX_TEST_RESULTS_ROOT` 改到数据盘。每个测试包含：

- `metadata.txt`：Run ID、Git 提交、分支、主机和时间；
- `status.txt`：通过/失败和退出码；
- `logs/`：分步骤日志；
- JUnit XML 或二进制 SHA-256 等专项结果。

重复使用同一 Run ID 会失败，防止覆盖既有证据。

## 5. GitHub Actions

`.github/workflows/pr-fast-ci.yml` 在 Pull Request 和 `main` 推送时运行：

- `Fast tests`：快速测试入口；
- `Linux builds`：两个 Linux 程序构建。

日志和测试报告保留 14 天。云端 CI 不运行回放，不访问共享盘、4T 数据盘、GPU 模型、相机或转台；回放、性能和真机验证继续在本地主机执行。
