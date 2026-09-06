# DMX 当前交接索引

最后核对：2026-09-04
项目目录：`/home/sht/work/DMX_qt`

## 1. 当前基准

- 正在执行的基线任务：`OPS-001`
- 候选分支：`ops/OPS-001-engineering-baseline-20260904`
- 基准起点：`main@83b3f3b`
- 任务记录：`docs/项目管理/OPS-001_2026-09-04工程基线.md`
- 工程化策略：`docs/项目管理/DMX项目工程化管理策略.md`

2026-08-18 版长交接文档已归档到 `docs/archive/TASK_HANDOFF_20260818.md`。其中“YOLO 默认停用”等内容已过期，不能作为当前运行依据。

## 2. 两个程序

DMX 和 DMX_test 共用同一套 C++ 源码和 `dmx_config.json`：

- `DMX.pro`：现场设备版，从 `/mnt/dmx_share` 读取设备图像。
- `DMX_test.pro`：回放版，定义 `DMX_TEST_BUILD`，程序名为 `DMX_test`。
- `run_dmx.sh`：现场一键启动入口。
- `run_dmx_test.sh`：2026-07-23 本地回放一键启动入口，按 8 s/圈模拟 UDP 通知。

编译：

```bash
./build_linux.sh
./build_test.sh
```

## 3. 当前实时检测链路

1. RGB/BW 路径通知由 UDP 8001 进入，图像解码后旋转、镜像并写入 360 度全景缓存。
2. RGB 和 BW 各自的前 3 个完整全景只用于背景/天空 Mask 建模，不进入任何目标检测路径。
3. 全景 Mask 计算和归档在独立后台任务中执行；Mask 完成后，从下一圈开始检测。
4. 现场启动配置为 RGB/BW 双流、天空边界内缩 64 px、Direct YOLO 开启。
5. Direct YOLO 通过 `directyolomanager.*` 调度 `tools/dmx_direct_yolo_worker.py`，使用 weights5 ONNX；传统路径保留为诊断辅助。
6. YOLO 结果经置信度、天空覆盖率和队列时延预算判决，再进入 RGB/BW 时空事件融合。
7. 主界面显示紧凑极坐标雷达、目标列表和 360 度环绕裁图；右键可标记误检并写入固定负样本目录。

## 4. 关键外部依赖

- 数据盘：`/mnt/dmx4t`
- 设备共享盘：`/mnt/dmx_share`
- 现场数据根目录：`/mnt/dmx4t/data`
- 回放 UDP 原始日志：`/mnt/dmx4t/data/raw_log/20260723`
- 回放图像基准：`/mnt/dmx4t/data/replay_sources/baseline_20260723`
- weights5：`/home/sht/data/低小慢数据/yolo26s/result/jiao/pt/weights5/weights/best.onnx`
- Python 环境：`/home/sht/work/DMX_qt/.venv`

模型、数据和运行日志不提交到 Git，发布和测试报告必须记录它们的版本或 SHA-256。

## 5. 常用验证

```bash
# 统一入口
./scripts/test_fast.sh
./scripts/test_build.sh
./scripts/test_replay_smoke.sh

# 也可单独执行原始入口
# Python 测试
python3 -m pytest -q tests

# 四组 C++ 测试
qmake appconfig_tests.pro && make && ./appconfig_tests
qmake panoramacache_tests.pro && make && ./panoramacache_tests
qmake manualnegativestore_tests.pro && make && ./manualnegativestore_tests
qmake compacttargetradarpanel_tests.pro && make && ./compacttargetradarpanel_tests

# 完整构建
./build_linux.sh
./build_test.sh
```

统一入口、结果目录和 CI 边界详见 `docs/testing/README.md`。实际基线验证结果以 `docs/testing/` 中对应报告为准，不在本文档中累加历史日志。

## 6. 开发约束

- 新任务使用 `Issue -> 开发分支 -> Commit -> PR -> 测试报告 -> Run ID -> Release`。
- 不直接在 `main` 上开发或强制推送。
- 算法改动必须与 2026-07-23 固定回放基准对比。
- 设备端口、挂载、配置、模型和数据版本变化必须在 Issue/PR 中单独说明。
