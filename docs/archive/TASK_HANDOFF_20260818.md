# DMX_qt 项目与任务交接文档

最后更新：2026-08-18
项目路径：`/home/sht/work/DMX_qt`
当前目标：持续维护 DMX 主控程序、目标识别推理链路、新目标雷达界面和相关文档。

## 0. 维护规则

这个文件作为后续长期交接文档使用。每完成一轮功能、修复或重要分析后，都应同步更新本文件。

更新时至少检查以下位置：

- `最后更新` 日期。
- `2. 当前已完成事项`。
- `3. 当前待办与风险`。
- `5. 关键实现说明` 中是否有逻辑变化。
- `8. 验证记录` 中是否需要追加本次编译、运行或数据验证结果。

维护原则：

- 已经落地并编译通过的内容写入“已完成”。
- 只做过方案分析但未改代码的内容写入“待办”。
- 未实测或只在模拟数据上验证的内容必须标注“待确认”。
- 不要把显示层临时校准写成算法数据变更。
- 不要随意重置 git 工作区。当前仓库有较多已修改和未跟踪文件，默认都按已有工作成果处理。

## 1. 项目概况

DMX_qt 是 Qt/C++ 主控程序，用于低空小目标观测设备的图像接收、全景拼接缓存、转台控制、录制保存、疑似目标识别和雷达化显示。

主要能力：

- 通过设备路径/UDP 事件接收 RGB 与 BW 图像。
- 根据转台角度把图像写入全景缓存。
- 显示主界面全景条、主雷达图、目标捕获图和算法反馈图。
- 控制转台串口、设备运行/停止、正交输出等。
- 按 tile 前若干帧建立背景和地平线模型。
- 使用 v1 天空 mask 提取天空检测区域。
- 在天空区域执行传统点目标候选筛选。
- 保留 YOLOv26s ONNX 模型接入能力；当前配置默认停用 YOLO，先用传统算法。
- 保存疑似目标 ROI JPG 与 `manifest.jsonl`。
- 将疑似目标推送到新目标雷达页面独立图层显示。

构建方式：

```bash
cd /home/sht/work/DMX_qt
bash build_linux.sh
```

程序输出：

```text
/home/sht/work/DMX_qt/build_linux/DMX
```

核心配置：

```text
/home/sht/work/DMX_qt/dmx_config.json
```

## 2. 当前已完成事项

### 2.1 文档

- 已编写软件使用说明书：`docs/dmx_software_user_manual.md`。
- 已编写目标雷达显示策略文档：`docs/target_display_strategy.md`。
- 本文件作为长期交接文档：`TASK_HANDOFF.md`。

### 2.2 目标识别推理链路

已完成当前版本的 v1 天空 mask + 传统点目标识别链路，YOLOv26s 保留为可选能力但当前默认停用：

- 每个全景 tile 使用前 `detect.backgroundFrames` 帧建立背景，当前配置为 3。
- 背景 ready 后生成 v1 天空 mask：
  - `clear_sky ∪ cloud_sky`。
  - 保留多块大天空区域。
  - 排除孤立零散小块。
  - 通过形态开运算掐断过细通道，避免区域增长沿细缝蔓延。
- 传统算法只在 v1 天空 mask 内做点扩散、BlackHat 暗点增强、形状紧致度、模板高通相关性和 NMS 筛选。
- 检测时会对天空 mask 再做轻微内缩，降低树叶/建筑边缘附近误检。
- 参考纹理模板配置为 `detect.referenceTemplatePath`，当前默认：
  - `/mnt/dmx4t/DMX_yangben/20260714/_analysis/wrj.jpg`
- YOLOv26s ONNX 接入 `VideoWorker::detectCandidates()`。
- 当前 `dmx_config.json` 中 `detectYolo.enabled = false`，不再默认调用 ONNX。
- 后续如果重新启用 YOLO，`detectYolo.mode = hybrid` 仍支持：
  - 传统算法先召回候选。
  - YOLO 对传统候选 ROI 二次确认。
  - 为防止传统算法漏召回，按间隔在天空 mask 内选择少量窗口做 YOLO 补检。
- YOLO 输出解析支持两类格式：
  - NMS 后格式：`x1, y1, x2, y2, score, class_id`。
  - 常规 YOLO 格式：`cx, cy, w, h, objectness/classes...` 或无 objectness 的 class scores。
- 已增加慢速背景更新：
  - `detect.backgroundUpdateAlpha`
  - `detect.backgroundUpdateInterval`
  - `detect.backgroundProtectRadius`
- 背景更新会保护疑似目标周围区域，避免目标被吸收到背景。

### 2.3 ROI 保存

已调整为目标位于 ROI 图像中央：

- 使用固定尺寸裁剪，当前 `detect.cropSize = 256`。
- 裁剪中心使用最终候选目标中心。
- manifest 写入：
  - `roiTargetX = cropSize / 2`
  - `roiTargetY = cropSize / 2`
  - `frameX/frameY`
  - `panoX/panoY`
  - `angle`
  - `detector`
  - `classId/className`
  - `yoloScore`

输出目录：

```text
detect.saveRoot/YYYYMMDD/HH/
```

当前配置为：

```text
/mnt/dmx4t/data/candidates
```

### 2.4 新目标雷达界面

`radar_ui/` 目录已经加入主工程 `DMX.pro`。

已完成基础界面：

- 左侧目标 ROI 列表：`TargetListWidget`。
- 中间目标雷达图：`TargetRadarWidget`。
- 右侧目标信息面板：`TargetInfoPanel`。
- 右下目标预览：`TargetPreviewPanel`。
- 独立目标记录结构：`TargetRecord`。
- 图像查看弹窗：`ImageViewerDialog`。

目标雷达页面已经作为主界面 page stack 中的页面切换显示，不是单独测试弹窗。

入口：

- `MainWindow::onOpenTargetRadarWindow()`
- 工具栏按钮文字在主界面和目标雷达页面之间切换。

实时联动：

- `VideoThread::candidateDetected(...)` 进入 `MainWindow::onCandidateDetected(...)`。
- 主界面旧雷达图追加检测红点。
- 新目标雷达窗口调用 `TargetRadarWindow::addOrUpdateTarget(...)`。
- 扫描角通过 `TargetRadarWindow::setScanAngle(...)` 同步。
- BW 全景缩略图通过 `TargetRadarWindow::setLivePanorama(...)` 作为雷达背景。

### 2.5 目标雷达显示策略

已实现独立目标显示图层：

- 每次识别到目标后更新目标图层。
- 目标可信度用图标大小表示。
- 目标出现时间新鲜度用红色深浅表示。
- 新近目标为深红色，较旧目标逐渐变浅。
- 过期或低新鲜度目标使用虚线效果。
- 目标状态分为：
  - `new`
  - `tracking`
  - `confirmed`
  - `stale`
- 连续目标按方位角和全景高度合并。

左侧 ROI 列表替换策略已实现：

- 最多展示 7 个目标。
- 不使用简单 FIFO 队列。
- 按状态、可信度、时间新鲜度、命中次数、评分综合排序。
- 当前选中目标尽量保留在列表中。

### 2.6 ROI 点击查看

已实现：

- 点击左侧 ROI 缩略图会打开 `ImageViewerDialog`。
- 弹窗支持：
  - 放大。
  - 缩小。
  - 适合窗口。
  - 原始大小。
  - `Ctrl + 鼠标滚轮` 缩放。

待补充的重要点：

- 当前弹窗打开的是 `TargetRecord.imagePath`，也就是目标 ROI 截图。
- 用户最初希望点击疑似目标后展示“对应全图”并可缩放查看。
- 如果要严格满足“全图查看”，需要在检测保存和信号链路中继续传递原始全图/源图路径，或能通过 `source`、`date/hour`、`panoX/panoY` 反查对应全图。

### 2.7 雷达刻度与临时正北校准

已完成：

- 新目标雷达图外圈增加角度刻度。
- 刻度包含 5 度、10 度、30 度层级。
- 0/90/180/270 标注为北/东/南/西。
- 支持本次运行内显示正北校准。

操作方式：

- 左键点击雷达图上的参考方位，再点击顶栏“设为正北”。
- 或右键点击雷达图参考方位，选择“将该方向设为显示正北”。
- 点击“正北归零”恢复设备原始方位显示。

重要约束：

- 正北校准只影响显示。
- 不写入配置文件。
- 程序关闭后恢复默认。
- 不修改算法输出的原始方位角。
- 不影响 ROI、manifest、目标合并和历史追溯。

显示换算：

```text
displayAzimuthDeg = normalize(rawAzimuthDeg - northOffsetDeg)
rawAzimuthDeg = normalize(displayAzimuthDeg + northOffsetDeg)
```

### 2.8 编译状态

最近一次功能编译验证：

```bash
bash build_linux.sh
```

结果：通过。
说明：2026-07-16 已将 v1 天空 mask、传统点目标识别和 YOLO 默认停用配置集成到主程序后编译通过。输出程序为 `/home/sht/work/DMX_qt/build_linux/DMX`。

### 2.9 原始 UDP 日志与模拟基准数据准备

已新增 8001 原始 UDP 文本日志：

- 配置项：`paths.rawLogRoot`。
- 当前配置：`/mnt/dmx4t/data/raw_log`。
- 环境变量覆盖：`DMX_RAW_LOG_ROOT`。
- 记录入口：`VideoWorker::processPathDatagrams()`。
- 写入时机：`readDatagram()` 后立即写，早于路径解析、角度分发、UI 丢帧、解码重试和检测处理。
- 文件路径：
  - `rawLogRoot/YYYYMMDD/HH/udp_8001_YYYYMMDD_HH_启动时间_pid.jsonl`
- 每行 JSON 字段：
  - `rxMs`
  - `rxIso`
  - `sender`
  - `len`
  - `text`
- 每条 UDP 写入后立即 `flush()`，降低异常退出造成原始日志丢失的风险。
- 后续模拟发送方可按 `rxMs` 差值回放 `text`，用于本地发送、本地接收和实时检测性能测试。

已创建本地 4T 盘模拟基准数据镜像根目录：

```text
/mnt/dmx4t/data/replay_sources/baseline_20260723/raw/20260723/RGB
/mnt/dmx4t/data/replay_sources/baseline_20260723/raw/20260723/BW
```

后续如果将这次采集的原始图像按 `/data/raw/...` 的相对层级复制到该根目录，模拟回放时可使用：

```bash
DMX_SHARE_MOUNT=/mnt/dmx4t/data/replay_sources/baseline_20260723
```

这样原始 UDP 日志中的 `/data/raw/...` 路径会被主程序映射到本地基准数据目录。

### 2.10 YOLO 硬确认诊断日志

2026-07-23 追加 `YOLODBG` 每圈诊断，用于定位“传统候选有输入，但最终没有目标输出”的原因。

- 代码位置：`VideoWorker::finalizeRoundCandidates()`。
- 触发条件：`detectYolo.enabled=true` 且 `detectYolo.mode=confirm/hybrid`。
- 诊断推理阈值：`min(confirmThreshold, 0.00005)`；最终通过仍使用正式 `confirmThreshold`，不降低实际报警阈值。
- 每圈 `YOLODBG` 会列出送入 YOLO 的候选窗口：
  - `px/py`：候选全景坐标。
  - `sc/ch/resp/ct/tmpl`：传统候选分数、cheap 分数、响应、对比度和模板相关值。
  - `det`：YOLO 诊断阈值下输出框数量。
  - `drone=raw/weighted`、`bird=raw/weighted`：最好 drone/bird 原始分数和中心加权分数。
  - `reject`：拒绝原因，常见为 `no_det`、`no_drone`、`low`、`bird`、`bad_box`、`round_nms`。
  - `keep=1`：该候选通过 YOLO 确认并进入保存/上报流程。

2026-07-23 追加修正：

- `1x6x8400` 输出按 raw YOLO 格式 `[cx, cy, w, h, class0, class1]` 解析，不再误判为 NMS 后的 `[x1, y1, x2, y2, conf, class]`。
- `AppConfig` 中 YOLO 置信度阈值下限从 `0.01` 放开到 `0.000001`，支持当前 weights2 的低分数尺度。
- 当前测试配置：
  - `detectYolo.confThreshold=0.0001`
  - `detectYolo.confirmThreshold=0.0001`
  - `detectYolo.supplementThreshold=0.0003`
- `YOLODBG` 诊断阈值改为 `min(confirmThreshold, 0.00005)`，最终保存/上报仍按正式 `confirmThreshold`。

2026-07-23 继续修正：

- `YOLODBG` 增加 `shape/rows/pre/nms/rawmax/cls/wmax/mxbox` 等字段，用于判断实时裁图送入 YOLO 后的原始最大分、阈值前候选数和 NMS 后候选数。
- 新增配置 `detectYolo.fallbackTraditionalOnEmpty`。
- 当前配置打开该兜底：YOLO 硬确认整圈无输出时，仍输出传统候选，`detector=traditional_realtime_round_yolo_fallback`，同时保留 YOLO 诊断日志。

### 2.11 天空 mask 边缘内收

2026-07-23 追加 `detect.skyShrinkPixels`，用于整体收缩检测阶段天空 mask，压制树冠、房屋和地物边缘被划成天空后造成的点目标误检。

- 当前配置：`detect.skyShrinkPixels=32`。
- 作用位置：`VideoWorker::collectTileCandidates()` 中 `detectionSkyMask` 生成阶段。
- 实现方式：对全分辨率天空 mask 做椭圆核腐蚀，核尺寸为 `2 * skyShrinkPixels + 1`。
- 保护逻辑：如果腐蚀后天空区域过小，会保留原 mask，避免极端情况下整片天空被吃掉。
- 日志：`DETECT ... using panorama sky mask slices ... shrink=32px`。
- 调参建议：
  - 树/房屋边缘误检仍多：调到 `48`。
  - 贴近天空边缘的目标漏检：调回 `16` 或 `24`。

### 2.12 候选闪烁与杂波抑制

2026-07-23 针对外场反馈“无人机时不时被检测到，有时没有”追加两类稳定性调整：

- 当前实测日志显示一圈检测汇总约 `9-10s` 一次，原 `detectUi.radarHoldMs=5000` 会导致红点在下一圈到来前自然过期。
- 当前配置改为 `detectUi.radarHoldMs=20000`，覆盖约两圈，降低显示层闪烁。
- 传统候选新增局部杂波指标 `clutter`：
  - 在候选中心周围 128x128 区域计算高通纹理和响应密度。
  - 树冠、云边、画面边界这类周围纹理密集的点会被扣分；极高杂波且模板/点扩散不强时直接过滤。
  - `YOLODBG` 追加 `clu=` 字段，`manifest.jsonl` 追加 `clutter` 字段，便于后续看日志判断阈值强弱。
- 注意：编译后如果已有 DMX 进程仍在运行，`/proc/<pid>/exe` 会显示 `(deleted)`；必须退出并重新启动程序，新二进制和新配置才会生效。

### 2.13 20260723 独立基准回放程序

2026-07-29 已完成独立 `dmx_test` 基准回放入口，检测处理逻辑与实时主程序共用同一套源代码，不复制或分叉检测算法。

数据完整性：

- 原始日志：`/mnt/dmx4t/data/raw_log/20260723`。
- 图像数据：`/mnt/dmx_share/raw/20260723`。
- UDP 文本共 19,422 条，BW/RGB 各 9,711 帧。
- 两路帧序号均连续为 `0..9710`，无重复、无缺帧。
- 19,422 个日志图像路径全部存在，图像总量约 55 GB。
- 原始正常采集阶段每路约 `1.772 fps`，BW 到同序号 RGB 的中位间隔为 `102ms`。

固定回放规则：

- 不连接或操控转台，不使用当天转台角度。
- 打开程序后先暂停，点击“设备运行”才开始回放；“设备停止”可暂停，再次运行可继续。
- 固定 `8s/圈`、`16` 组/圈，即每 `500ms` 一组。
- 每组先发 BW，`102ms` 后发 RGB，合计 `4 UDP/s`，每路 `2 fps`。
- 原始日志中的程序重启和长时间采集停顿不带入固定 8 秒档位。
- 9,711 组完整回放一次耗时 `4855.5s`，即 `1h20m55.5s`。

2026-07-30 全景方向和雷达平滑修正：

- 回放初版把递增帧号映射为递增角度，等价于右转；实机配置和 20260723 角度日志均为左转，帧号增加时角度递减。
- 回放帧现按 `0, 337.5, 315...` 的左转顺序写入全景缓存，恢复与实时 DMX 相同的块排列方向。
- 像素层从未分叉：RGB/BW 都继续调用 `rotateCCW90(..., true)`，等价于先逆时针旋转 90°、再水平镜像；BW 的 180° 缓存对齐也保持不变。
- 主雷达初版只在 BW 路径到达时更新，即 `2Hz`、每次跳 `22.5°`，所以肉眼明显卡顿。
- 现使用 `30ms` 精确定时器按 `45°/s` 连续左转，并在每个 BW 帧到达时以理论角度校准相位；该频率与实机约 `33Hz` 的角度回传刷新相当。
- 检测、YOLO、天空 mask 和候选汇总逻辑均未修改。

隔离方式：

- 实时主程序默认继续监听 `8001`。
- `dmx_test` 监听本地 `127.0.0.1:18001`，本地控制端口为 `18501`。
- 测试输出独立写入 `/mnt/dmx4t/data/dmx_test`，不会污染实时日志和候选目录。
- 独立构建产物：`/home/sht/work/DMX_qt/build_linux_test/DMX_test`。
- 桌面入口：`/home/sht/桌面/dmx_test.desktop`。
- 详细说明：`docs/dmx_test_replay_20260723.md`。

### 2.14 dmx_test 专用识别实验

2026-07-30 起，识别算法和交互优化先在 `dmx_test` 验证，用户确认效果后再更新正式 DMX。

本轮测试版专用修改：

- ROI 弹窗在回放模式下拦截 `Ctrl+滚轮`，缩放锚点改为当前滚动视口中心。
- 锚点按当前显示图像的归一化坐标保存，异步缩放图返回后恢复到窗口中心；拖动到任意区域后继续缩放不会回到原图中心。
- 正式模式仍保留原有鼠标位置锚点行为，尚未合入上述交互修改。
- `run_dmx_test.sh` 当前设置 `DMX_DETECT_SKY_SHRINK_PIXELS=64`，将测试版天空 mask 内收由正式配置的 `32px` 提高到 `64px`；曾先试用 `48px`，用户查看全景叠加图后要求继续内收。
- `run_dmx_test.sh` 当前启用 `DMX_DETECT_SKY_GEOMETRY_CLEANUP=1`，仅在回放模式增加天空几何清理：
  - 每列只保留从全景顶部连续向下的天空，删除树冠/建筑下方通过侧向细缝接入的口袋和多段天空。
  - 在 1/4 全景 mask 上使用 `21x21` 椭圆核开运算切断窄颈，只保留面积足够且仍与顶部相连的主体。
  - 用 `41x41` 邻域边界密度定位不规则轮廓，在其周围 `61x61` 区域应用 `25x25` 局部深度腐蚀，约等于额外内收 48 个原图像素。
  - 清理后仍叠加现有全局 `64px` 内收；规则边界不额外扩大局部腐蚀。
  - 任一步导致天空少于原 mask 的 50% 时会触发面积保护回退。
- `dmx_config.json` 和 `build_linux/DMX` 均未更新；正式程序继续使用 `32px`。

实测注意：

- 第三圈生成 `16384x1024` 全景天空 mask 耗时约 `6535ms`。
- 初始化期间 BW 工作队列一度积压 14 帧，随后出现一次 `drop=13`；进入检测后单帧也出现约 `1.7-1.9s` 的处理耗时。
- 该性能问题不是内收参数未生效，而是全景 mask 生成和检测仍在 BW 工作线程同步执行。后续优化识别算法时应优先消除初始化阻塞和持续积压，否则固定 8 秒每圈基准会跳帧。

当前 mask 诊断导出：

- 仅回放模式且设置 `DMX_SKY_MASK_EXPORT_DIR` 时启用；不设置环境变量时不增加在线处理或写盘。
- 导出直接使用 `VideoWorker::updateAndGetPanoramaSkyMaskSlice()` 建立的首个 BW 全景样本和 `maskSmall`，不是另写 Python 近似算法。
- 输出包括在线模型输入、48px 内收前 mask、内收后最终 mask、全景叠加图、缩略总览和 `manifest.json`。
- 2026-07-30 基准导出目录：`/mnt/dmx4t/data/dmx_test/analysis/sky_mask_20260723_shrink48_20260730_1345`。
- 叠加图图例：青色为最终检测天空，红色为 48px 内收剔除区域。
- 结果：内收前 `2744012` 像素（16.36%），内收后 `2599273` 像素（15.49%），共剔除 `144739` 像素，约占原天空 mask 的 5.27%。
- 人工检查：树冠、楼体和立杆边缘形成连续红色退让带，青色最终检测区未继续覆盖这些边缘。
- 诊断模式需要额外执行 16 个 4096x4096 mask 腐蚀和图像写盘，本次总耗时约 26.9 秒；这是导出开销，常规不设置环境变量时仍约 6.5 秒。
- 用户复核后将测试参数提高到 `64px`，导出目录为 `/mnt/dmx4t/data/dmx_test/analysis/sky_mask_20260723_shrink64_20260730_1410`。
- `64px` 最终 mask 为 `2554089` 像素（15.22%），相较 `48px` 再剔除 `45184` 像素；累计剔除原天空 mask 的约 6.92%，大块天空仍保持连通。
- 几何清理版导出目录：`/mnt/dmx4t/data/dmx_test/analysis/sky_mask_20260723_geometry_v1_shrink64_20260730`。
- 导出现在分别保存原始时序 mask、几何清理后 mask、全局内收后最终 mask；叠加图红色包含几何清理与全局内收共同剔除的范围。
- 几何清理日志：`raw=2744012 top=2563455 neck=2560652 rough=448 final=2521444`。其中顶部连续性删除 `180557` 像素，窄颈阶段删除 `2803` 像素，不规则段局部深度内收删除 `39208` 像素。
- 最终叠加 64px 内收后为 `2355014` 像素（14.04%），比仅使用 64px 全局内收再减少 `199075` 像素（7.79%）。
- 原 mask 有 463 列包含不从顶部开始的侧向区域、431 列包含多段垂直天空；几何清理后两项均为 0，5 个主要天空连通区仍保留。
- 本次诊断导出写入 6 张 16384x1024/4096x256 图，完整建模加导出耗时约 42.3 秒；正常桌面测试不设置导出目录，因此没有这部分写盘耗时。

### 2.15 20260723 天空区域 512 数据集

2026-07-30 按人工标注训练需求，完成 20260723 全量 RGB/BW 天空窗口导出。

- 新增脚本：`tools/generate_20260723_sky_512_dataset.py`。
- 输入：`/mnt/dmx_share/raw/20260723`，RGB/BW 各 `9711` 张 4096x4096 JPG。
- 输出根目录：`/mnt/dmx4t/DMX_yangben/20260723`。
- 处理规则：
  - 像素方向与 DMX 主程序完全一致：逆时针旋转 90°，再水平镜像。
  - 复用人工检查通过的几何清理 + 64px 内收最终 mask：
    `/mnt/dmx4t/data/dmx_test/analysis/sky_mask_20260723_geometry_v1_shrink64_20260730/bw_04_mask_after_shrink_64px.png`。
  - 源帧序号按 16 帧一圈映射到左转全景方位；BW 使用与主程序一致的 8 tile/180° 对齐偏移。
  - 全图使用无重叠 512x512 网格；窗口只要与最终天空 mask 有至少一个像素交集就保留，不再运行传统候选或 YOLO 初筛。
  - 保存图保留源图像素和地平线边界上下文，mask 只用于选窗；通用裁剪函数对右侧/底部不足 512 的区域填纯黑。本批源图恰好能被 512 整除，因此实际输出 `pad_right/pad_bottom` 均为 0。
  - RGB/BW 输出均为 512x512 三通道 JPG，质量 95。
  - 脚本支持多线程和断点续跑；清单按源图实时落盘。
- 全量结果：
  - RGB：`115937` 张。
  - BW：`115937` 张。
  - 合计：`231874` 张，占用约 `6.9G`。
  - 图像目录：`images/RGB/<HHMM>`、`images/BW/<HHMM>`。
  - `manifest.csv`：每个窗口的源图、帧序号、方位、mask、左上角坐标、天空像素数和覆盖率。
  - `source_summary.csv`：全部 `19422` 张源图的方位、mask 和导出窗口数，包括无天空方位的 0 窗口记录。
  - `masks/world_tiles/tile_00.png` 至 `tile_15.png`：16 个 4096x4096 方位 mask。
  - `masks/stream_slot_map.csv`：RGB/BW 源序号槽到世界方位 mask 的映射。
  - `dataset_summary.json`：完整参数和最终数量。
- 独立核验：
  - 实际文件数与 manifest 集合完全一致，集合差异为 0。
  - manifest 文件名无重复。
  - 全部 `231874` 张 JPEG 均为 512x512、3 components。
  - manifest 的 16 方位映射、坐标步长、尺寸、padding 和天空覆盖条件异常数均为 0。
  - 输出目录无 `.tmp` 或非 JPG 训练图残留。

### 2.16 20260723 传统候选 + YOLO 确认五图预览

2026-07-30 在执行 20260723 全量疑似目标挖掘前，先生成 5 张效果预览供人工复核。

- 新增脚本：`tools/make_20260723_traditional_yolo_preview.py`。
- 传统候选池来自同一批 20260723 原始帧的 `dmx_test` 回放结果：
  `/mnt/dmx4t/data/dmx_test/candidates/20260723/15/manifest.jsonl`。
- YOLO 使用正式配置的 weights2：
  `/home/sht/data/低小慢数据/yolo26s/result/jiao/pt/weights2/best.onnx`，
  类别顺序为 `drone, bird`，输入 640x640。
- 处理方式：
  - 按传统分数取前 300 个不同候选 JPEG。
  - 独立重新运行 YOLO，不沿用旧 manifest 中的 fallback 结论。
  - 使用 256px 中心半径对 YOLO 分数加权。
  - 要求中心加权 drone 分数不低于 0.02，且至少为 bird 分数的 1.1 倍。
  - 去掉重复回放文件，并在最终预览中每个原始源帧最多保留一张。
- 结果：
  - 300 个传统候选中有 42 个通过本次 YOLO 条件。
  - 取分数最高的 5 张，中心加权 drone 分数为 `0.2283-0.3060`。
  - 五张目视均含有小型飞行目标。
  - 输出目录：`/mnt/dmx4t/DMX_yangben/20260723/traditional_yolo_preview_5`。
  - `images/` 为不带框的 512x512 干净训练图。
  - `annotated/` 为检查图，红框是 YOLO drone 框，黄框是传统连通域框。
  - `contact_sheet_annotated.jpg` 为五图横向总览。
  - `manifest.csv` 保存传统/Yolo 分数、框坐标和源图追溯信息。
- 五图预览已由用户确认；后续全量结果见 `2.17`。

### 2.17 20260723 传统候选 + YOLO 确认全量数据集

2026-07-30 按用户确认的五图效果，完成全部 `231874` 张 RGB/BW 天空窗口的疑似目标挖掘。

- 新增脚本：`tools/mine_20260723_traditional_yolo_dataset.py`。
- 输入：`/mnt/dmx4t/DMX_yangben/20260723/manifest.csv` 及其 RGB/BW 512 窗口。
- 输出：`/mnt/dmx4t/DMX_yangben/20260723/traditional_yolo_dataset`。
- 处理规则：
  - 每个天空窗口最多保留 3 个高召回传统候选。
  - 源帧按 16 帧一圈汇总，候选映射回 65536x4096 全景坐标。
  - 每路每圈使用 256px 环绕全景空间 NMS，最多取 8 个候选进入 YOLO。
  - YOLO 使用正式 `weights2/best.onnx`，输入 640x640，中心权重半径 256px。
  - 仅保留中心加权 drone 分数不低于 0.02，且不低于 bird 分数 1.1 倍的候选；不启用传统 fallback。
  - 最终窗口从方向变换后的原始 4096x4096 帧重新以传统候选为中心裁取，避免 512 网格边界引入假黑边；只有越过真实源帧边界时才填黑。
  - 输出为不带框的 512x512 三通道 JPG，便于后续人工标注；传统框、YOLO 框、分数和源图追溯信息保存在 CSV。
  - 以 `group_summary.csv` 为分组完成日志，支持中断后直接续跑。
- 全量结果：
  - 完成 RGB `607` 圈、BW `607` 圈，共 `1214` 个分组。
  - 实际扫描 RGB `115937`、BW `115937`，合计 `231874` 个天空窗口。
  - 最终保留 RGB `211` 张、BW `246` 张，合计 `457` 张。
  - 图片目录已按用户要求扁平化：全部 RGB 直接位于 `images/RGB/`，全部 BW 直接位于 `images/BW/`，不再按 `<HHMM>` 建时间子目录。
  - `manifest.csv` 共 457 行，保存传统特征、全景/源帧/ROI 坐标、YOLO drone/bird 原始与中心加权分数。
  - `group_summary.csv` 共 1214 行，记录每圈窗口数、传统候选数、YOLO 输入数、确认数和耗时。
  - `run_summary.json` 为最终完整汇总，明确 `fallbackTraditionalOnEmpty=false`。
- 独立核验：
  - 457 条 manifest 与 457 个实际 JPG 一一对应，无重复、无缺失、无额外训练图。
  - 全部 JPG 均能解码且为 512x512 三通道。
  - RGB/BW 分组均完整覆盖 round `0-606`，无重复分组；分组窗口合计与原数据集完全一致。
  - 全部 457 条记录均满足本次 YOLO 阈值和 drone/bird 比例要求。
  - 传统候选框中心相对输出中心偏差中位数约 1.58px、最大约 4.72px。
  - 输出目录无临时文件残留。
- 人工抽查：
  - 最高分样本以真实小型飞行目标为主。
  - 由于为人工标注准备而采用 0.02 的低阈值，低分段包含树叶边缘等 YOLO 硬负样本，这是预期保留内容，后续人工标注时应标为负样本。
- 完整数据处理进度汇报：`docs/20260723_data_processing_progress.md`。

### 2.18 2026-08-17 dmx_test 最终模型、目标分类与雷达背景

最新模型已经只同步到 `dmx_test` 桌面启动链路：

- 模型：`/home/sht/data/低小慢数据/yolo26s/result/jiao/pt/weights最终改版/best.onnx`。
- SHA-256：`d304e90952355ca144091e08842e13d029e4691c43b190f24a91a421e8db8db2`。
- 固定输入：`1x3x512x512`。
- 输出：`1x300x6`，为已经完成 NMS 的
  `x1,y1,x2,y2,confidence,class_id`，不能按旧模型的
  `cx,cy,w,h,class_scores` 解码。
- 类别：`drone`、`bird`、`civilian_airliners`。
- `run_dmx_test.sh` 通过 `DMX_YOLO_MODEL`、`DMX_YOLO_INPUT_SIZE=512`
  和 `DMX_DIRECT_YOLO_CLASS_NAMES` 覆盖测试版配置；正式版配置和启动脚本
  未切换模型。

直接 YOLO worker 已按模型元数据自动选择 end-to-end 或旧 raw 解码器，并在
启动时校验固定输入尺寸。三类检测结果都会写入候选清单并传到 Qt 界面，不再
把飞鸟结果提前丢弃。`classId/className` 已贯通 worker、manager、检测信号、
`TargetRecord`、`TARGET_ASSOC` 和 `DETECT_UI` 日志。

目标雷达测试界面已完成：

- 左侧列表增加“类别”，显示“无人机 / 飞鸟 / 民航客机 / 未知目标”。
- 右侧选中目标详情同步显示类别。
- 列表卡片重新排版，分类、来源、命中次数、状态和评分均可完整显示。
- 同源或跨圈类别冲突不合并；同序号 RGB/BW 只有在方位、高度和框形态高度
  重合时允许类别冲突合并，并由分类置信度更高的一路决定最终类别。
- RGB/BW 检测类别一致时继续按原有规则合并，避免同一目标重复列出。

背景模式已由占位项改为真实显示：

- “黑白高度极坐标”使用 BW 全景。
- “彩色背景”使用 RGB 全景。
- “融合背景”保留 RGB 色彩，并注入 55% 的 BW-RGB 亮度细节差。
- RGB/BW 尺寸不同时会先对齐；BW 需要 180 度对齐时复用现有投影工具。
- 融合结果按当前全景缓存，只有输入全景变化后才重新生成。

本轮验证：

- CUDA worker 使用最终模型检测已知无人机帧，类别为 `drone`，坐标
  `(190,1476)`、置信度 `0.806`，与历史基准 `(191,1476)` 对齐。
- 已知鸟类帧能够输出 `bird` 类别候选。
- 106 条实际候选关联回放通过：61 次新建、45 次合并；同源类别冲突分开，
  严格对齐的 RGB/BW 类别冲突按置信度投票合并。
- 8192px 合成全景界面冒烟测试通过，三种背景截图哈希均不同，三模式切换、
  重绘和截图总耗时约 260ms；分类文字和列表布局目视检查通过。
- `pytest` 现有 14 项测试通过，`build_test.sh` 通过。
- 正式工程在 `/tmp/dmx_formal_compile_20260817` 无 `DMX_TEST_BUILD` 独立
  编译通过，没有覆盖 `build_linux/DMX`。

### 2.19 2026-08-18 固定电线杆抑制与硬负样本

日志 `/mnt/dmx4t/data/dmx_test/logs/2026-08-17/log_23-57-03.txt` 中的
`T-0001` 经复核不是无人机，而是固定电线杆横担：

- 连续命中 100 次，全部来自 RGB，方位固定在约 `161.7°`。
- 全景中心仅在 `x=29430..29437`、`y=2855..2857` 内轻微抖动。
- 对应候选清单均为 `direct_yolo_local`，人工查看首、中、末 ROI 后确认是
  同一根电线杆。

`dmx_test` 的直接 YOLO worker 已加入保守的静态杂波确认：

- 同源、同类别候选需保持在全景 `24px x 12px` 范围内。
- 64 位局部外观 dHash 距离不超过 12，框面积和长宽比需保持兼容。
- 至少稳定命中 5 次、持续 24 秒，并且至少 3 次在候选框下方检测到连续
  竖直支撑结构，才归入静态背景杂波。
- 已确认静态点仍继续保存 512 ROI 和 manifest 记录，但不再进入正常目标信号；
  日志写入 `STATIC_CLUTTER suppress/hold`，已有列表项和雷达点立即隐藏。
- 如果外观变化超过严格阈值，会先作为新候选上报，不直接套用旧静态结论，
  避免真实目标经过固定结构时被误压制。
- 单候选上下文计算离线实测约 1ms，不改变直接 YOLO 的逐帧调度方式。

该能力由 `run_dmx_test.sh` 的 `DMX_STATIC_CLUTTER=1` 开启，参数也只在测试
启动脚本中设置。正式启动链路未开启，正式检测行为不变。

已从本次人工确认的固定电线杆误检簇提取硬负样本：

```text
/home/sht/data/低小慢数据/yolo26s/result/jiao/pt/weights最终改版/hard_negatives_20260723_static_pole
```

- 从 101 张唯一误检 ROI 中按时间均匀抽取 32 张干净 `512x512` 图像。
- `images/` 与 `labels/` 一一对应，标签为空 YOLO txt，表示整图无目标。
- `manifest.csv` 保留原路径、帧号、全景位置、置信度和检测框；
  `review_contact_sheet.jpg` 仅用于人工检查，不能用于训练。
- 当前只完成提取和审核，尚未用这批数据重新训练模型。

本轮验证：101 张电线杆序列在第 5 次命中时首次确认静态，后续共压制 96 次；
前 4 次保留为未确认候选，另有 1 次强曝光变化按保护策略短暂上报。人工确认的
真实无人机 ROI 上下文得分均为 0，重复测试未被静态规则压制。Qt 冒烟测试确认
列表与雷达点同步隐藏；14 项 Python 测试、测试版构建和无测试宏正式版独立构建
均通过。

### 2.20 2026-08-18 设备与主控程序详细使用说明书

面向设备推广、现场交付和售后维护，已全面重写正式使用说明书：

- Markdown 源文件：`docs/dmx_software_user_manual.md`。
- A4 PDF：`docs/dmx_software_user_manual.pdf`，37 页，带独立封面和目录。
- 文档编号 `DMX-UM-001`，版本 `V2.0`。

说明书按当前正式 `DMX` 的实际代码和配置编写，覆盖设备组成、安全和急停、
安装接线、设备网/SMB/串口部署、启动检查、主界面、转台、标准运行、全景与
ROI、设备端采集、本机 AB 录制、本机无损全景保存、日志与目录、检测边界、
目标雷达、停机、故障排查、配置、日常维护、交付验收和故障上报模板。

文档明确区分正式 `DMX` 与不控制真实转台的 `dmx_test`，也明确“目标雷达”
是光学全景极坐标显示，不提供物理测距，算法输出必须人工复核。当前正式版只
向目标雷达提供 BW 背景；测试版已有但尚未移植的 RGB/融合背景和三分类功能
没有被写成正式交付能力。

PDF 已用 Pandoc + XeLaTeX 导出，中文字体、封面、目录、表格密集页、故障页
和末页均已抽查。正式对外发布前仍需由交付方补充制造商、售后联系方式、实际
硬件型号/供电/防护等级以及产品和接线实拍图，并完成安全、电气和商务审核。

## 3. 当前待办与风险

### 3.1 新目标雷达界面继续完善

优先级：高。

待办：

- 在真实外场数据下验证新目标雷达页面布局、目标点位置、刻度和正北校准操作。
- 彩色和融合背景已在 `dmx_test` 实现，仍需用 7 月 23 日完整回放目视检查
  RGB/BW 配准和长时间切换流畅度。
- 目标雷达窗口目前仍保留测试 mock 数据构造函数 `buildMockTargets()`，实际集成时要确认清空逻辑是否覆盖充分。
- ROI 点击弹窗需要从“ROI 大图”升级为“对应全图查看”，见 `2.6`。
- 目标列表 7 个缩略图在不同分辨率下的尺寸需要实机检查。
- 目标标注文字可能在目标密集时互相遮挡，后续需要做避让或只在选中/悬停时显示详细标注。

### 3.2 彩色与黑白互补预处理

优先级：高。

当前状态：

- 配置层已有 `detect.stream`，支持 `BW`、`RGB`、`BOTH`。
- 当前推荐仍以 BW 为主。
- 当前主链路为 BW + v1 天空 mask + 传统点目标识别；YOLO 保留但默认停用。

待办：

- 制定 RGB/BW 互补策略：
  - BW 用于高灵敏小目标召回。
  - RGB 用于排除云边、鸟、亮点噪声或补充颜色/纹理特征。
  - 两路检测结果按方位角、panoY、时间窗口合并。
- 评估是否先在 BW 传统候选后取 RGB 同位置 patch 做复核。
- 评估 BOTH 模式下 CPU 压力和误报变化。
- 增加跨流目标融合字段，例如 `streamMask`、`rgbScore`、`bwScore`。

### 3.3 传统筛选未召回时的应对

优先级：高。

当前已有措施：

- 当前默认不启用 YOLO，主召回来自传统点扩散/纹理筛选。
- 传统检测使用 `wrj.jpg` 中心目标高通模板、中心-环带暗点响应、形状紧致度和线状惩罚压制误检。
- `hybrid` 模式代码仍保留；重新启用 YOLO 后会按 `detectYolo.supplementInterval` 做低频天空补检。

待优化：

- 传统阈值只在 2026-07-14 首个 AB 样本上做过离线验证，仍需更多含目标/无目标样本评估。
- 传统候选目前没有在主程序里做跨帧时序确认；离线脚本已验证该策略有效，后续可接入实时链路。
- YOLO 补检窗口选择目前仍依赖 diff 响应，极低对比度目标可能仍漏。
- 可增加按 horizon 上方高度带的均匀巡检窗口。
- 可根据历史目标轨迹在邻近方位做局部补检。
- 可在扫描线附近优先补检，降低无效窗口数量。

### 3.4 YOLO 模型效果验证

优先级：高。

当前模型：

```text
/home/sht/data/低小慢数据/yolo26s/result/jiao/pt/weights/best.onnx
```

当前配置：

```text
detectYolo.enabled = false
detectYolo.mode = hybrid
detectYolo.inputSize = 640
detectYolo.confirmThreshold = 0.03
detectYolo.supplementThreshold = 0.08
detectYolo.iouThreshold = 0.45
```

注意：

- 2026-07-16 按用户要求，正式程序先停用 YOLOv26s，默认只跑传统点目标识别。
- 2026-07-15 已将 `dmx_config.json` 中 `detectYolo.modelPath` 更新为新权重 `jiao/pt/weights/best.onnx`。
- 新 ONNX 输出形状为 `(1, 6, 8400)`，前 4 行为 box，后 2 行为 `drone/bird` 类别分数。
- 离线抽样和滑窗验证发现新模型类别分数尺度很低，约需要 `0.0003` 量级阈值才有候选。
- 当前正式程序 `AppConfig` 会将 YOLO 阈值限制在 `0.01` 以上；如果直接使用当前正式阈值，新模型可能没有输出。正式接入前需要重新校准阈值或确认 ONNX 导出是否缺少置信度/后处理。

待办：

- 用 `/home/sht/data/低小慢数据/yolo26s/result/lu` 相关数据做离线验证。
- 统计传统候选、YOLO 确认、YOLO 补检各自的召回和误报。
- 确认类别顺序 `["drone", "bird"]` 与训练 `data.yaml` 一致。
- 检查 ONNX 输出格式是否在所有导出版本中稳定。
- 补一份阈值调参记录，避免外场临时盲调。

### 3.5 检测信号字段不足

优先级：中。

当前 `VideoThread::candidateDetected` 信号：

```cpp
void candidateDetected(const QString &stream,
                       double angle,
                       int panoX,
                       int panoY,
                       double score,
                       const QString &cropPath);
```

问题：

- manifest 中有 `frameX/frameY`，但信号没有传给 `MainWindow`。
- `MainWindow::onCandidateDetected()` 构造 `TargetRecord` 时 `frameX/frameY` 当前填 0。
- 信号也没有传 `detector/className/yoloScore/source/fullImagePath`。

后续建议：

- 扩展信号或新增结构体信号，直接传完整目标记录。
- 至少补充 `frameX/frameY`、`detector`、`className`、`yoloScore`。
- 若要实现全图查看，需要补充可打开的全图路径或可反查的 source 信息。

### 3.6 主界面和转台控制遗留优化

优先级：中。

旧交接中曾定位的问题：

- 转台控制参数切换时，UI 线程中可能存在同步等待或 sleep，导致主雷达图短暂停顿。
- 主雷达 `RadarWidget` 旧逻辑可能存在角度整数化或刷新节流不足问题。

当前需要重新确认：

- 现有 `turntablecontroldialog.cpp`、`turntabledriver.cpp` 是否已经完全异步化。
- 外场调整 `6s/圈 -> 8s/圈` 时主界面雷达是否仍卡顿。
- 主雷达图和新目标雷达图的扫描角是否都足够平滑。

### 3.7 文档持续补齐

优先级：中。

已完成：

- 软件使用说明书。
- 目标显示策略文档。

待补：

- YOLOv26s 接入与阈值调参专项文档。
- RGB/BW 互补预处理专项文档。
- 外场操作简表：启动、校准正北、录制、检测、保存、问题排查。
- 数据目录规范和备份规范。

### 3.8 采集基准数据保存与回放

优先级：高。

当前状态：

- 8001 原始 UDP 文本日志已能写入 `/mnt/dmx4t/data/raw_log`。
- `/mnt/dmx4t` 本地 4T 盘已挂载并可写。
- `/mnt/dmx_share` 当前挂载为设备 SMB：`//192.168.4.1/data`。
- 20260723 图像已完整保存在 `/mnt/dmx_share/raw/20260723`，并已完成日志路径逐条存在性校验。
- 已完成独立 `dmx_test` 构建、固定 8 秒每圈回放发送器和桌面一键入口，见 `2.13`。

剩余风险：

- 当前基准图像仍位于设备 SMB 共享盘；离线断开设备后无法回放。
- 如果后续要求完全脱离设备，应把约 55 GB 的 `/mnt/dmx_share/raw/20260723` 按原层级复制到本地 4T 盘，再把 `DMX_SHARE_MOUNT` 指向该镜像根目录。

## 4. 关键文件速查

### 4.1 工程与配置

- `DMX.pro`：主工程 qmake 文件。
- `DMX_test.pro`：从同一源代码构建独立 `DMX_test` 可执行程序。
- `build_linux.sh`：Linux 构建脚本。
- `build_test.sh`：Linux 测试回放版构建脚本。
- `run_dmx_test.sh`：测试版环境隔离、本地回放发送器和 GUI 启动总入口。
- `dmx_config.json`：运行配置。
- `appconfig.h/.cpp`：配置加载、默认值、环境变量覆盖。
- `resources.qrc`：资源文件。

### 4.2 主界面

- `mainwindow.h/.cpp`：主窗口、工具栏、页面切换、设备控制、候选目标接收。
- `aivideowidget.h/.cpp`：视频/图像显示控件。
- `panoramawidget.h/.cpp`：全景显示控件。
- `radarwidget.h/.cpp`：旧主界面雷达控件。

### 4.3 图像接收、缓存、保存

- `videothread.h/.cpp`：视频接收、全景写入、检测推理、候选输出。
- `panoramacache.h/.cpp`：全景缓存。
- `panoramasaver.h/.cpp`：全景保存。
- `rawrecorder.h/.cpp`：录制输出。

### 4.4 转台与设备

- `turntablecontroldialog.h/.cpp`：转台控制界面。
- `turntabledriver.h/.cpp`：转台串口驱动。
- `udpprotocol.h/.cpp`：设备 UDP 控制。

### 4.5 新目标雷达

- `radar_ui/targetradarwindow.h/.cpp`：目标雷达页面总控件。
- `radar_ui/targetradarwidget.h/.cpp`：雷达图绘制、目标层、扫描线、刻度、正北校准。
- `radar_ui/targetlistwidget.h/.cpp`：左侧 ROI 列表和替换展示。
- `radar_ui/targetinfopanel.h/.cpp`：目标信息面板。
- `radar_ui/targetpreviewpanel.h/.cpp`：目标预览面板。
- `radar_ui/imageviewerdialog.h/.cpp`：可缩放图像查看弹窗。
- `radar_ui/polarpanoramaprojector.h/.cpp`：全景图到极坐标背景投影。
- `radar_ui/targetrecord.h`：目标记录结构。

### 4.6 文档

- `docs/dmx_software_user_manual.md`：软件使用说明书。
- `docs/target_display_strategy.md`：目标显示策略算法文档。
- `docs/dmx_test_replay_20260723.md`：20260723 基准数据分析和 dmx_test 操作说明。
- `TASK_HANDOFF.md`：当前交接文档。

## 5. 关键实现说明

### 5.1 检测链路入口

主要入口：

- `VideoWorker::detectCandidates(...)`
- `VideoWorker::run()`
- `MainWindow::onCandidateDetected(...)`

检测流判断：

- `detect.stream = BW` 时只处理 BW。
- `detect.stream = RGB` 时只处理 RGB。
- `detect.stream = BOTH` 时两路都处理。

BW tile 对齐：

- BW 流在 tile index 上做了半圈偏移处理。
- 主界面传给新雷达的 `m_uiThumbBw` 已经应用 BW 的 180 度角度偏移，因此 `MainWindow::updateTargetRadarBackground()` 里不应再二次旋转。

### 5.2 天空 mask 与地平线

传统背景建模仍按 tile 独立进行：

- 根据角度计算 tile index。
- 每个 tile 收集前 `backgroundFrames` 帧。
- 背景 ready 后仍更新 `horizonProfile` 作为兼容信息。
- 正式检测 mask 使用全景级 `buildTemporalSkyMaskPreparedSmallV1(...)`：
  - 每个 tile 只把同角度观测缩小后填入 1/4 全景样本。
  - 全景样本集齐 `backgroundFrames` 次后统一生成 `maskSmall`。
  - 每个样本先生成 `clear_sky`、`cloud_sky` 和高置信天空种子。
  - 使用前几次同角度观测的时序一致性辅助确认。
  - 保留含可靠天空锚点的大连通块，支持多块天空。
  - 剪掉孤立碎片和过细连通通道。
  - 最终 `detect_sky = clear_sky ∪ cloud_sky`。
- 在线检测时按 `tileIndex` 从全景级 `maskSmall` 裁出当前 tile 的 mask slice 并放大使用。

当前策略：

- 传统检测只在 v1 天空区域执行。
- 检测前会对天空 mask 轻微腐蚀，降低天空边缘树叶、建筑和云边误检权重。
- YOLO 若重新启用，补检也要求窗口天空覆盖率达标。
- 地面、草坪、树线、建筑边缘原则上不进入检测区域。

### 5.3 传统候选筛选

主要步骤：

1. 灰度化。
2. 中值滤波。
3. 对 v1 天空 mask 做轻微内缩，形成检测 mask。
4. 计算局部背景 `GaussianBlur(gray, sigma=9)`，用 `localBg - gray` 得到暗点响应。
5. 对原图做 CLAHE、轻锐化，再用 BlackHat 增强暗小目标。
6. 取暗点响应和 BlackHat 响应最大值，只保留检测 mask 内响应。
7. 在检测 mask 内计算 `mean + thresholdK * std` 阈值，并带低阈值下限和高阈值上限。
8. 二值化、3x3 闭运算、连通域筛选面积/尺寸/长宽比/紧致度。
9. 对每个候选计算中心-环带点扩散特征，要求中心暗核明显、不是线状结构。
10. 如果 `detect.referenceTemplatePath` 可用，则与 `wrj.jpg` 中心高通模板做相关性筛选。
11. 按响应、暗点对比、中心-环带响应、模板相关性和紧致度综合打分，排序后 NMS。

### 5.4 YOLO 混合检测

当前 YOLO 加载：

- `ensureYoloNet(...)` 使用 OpenCV DNN 读取 ONNX。
- 模型路径来自 `detectYolo.modelPath`。

当前推理：

- `runYoloOnBgr(...)` 将 crop resize 到 `detectYolo.inputSize`。
- 使用 `blobFromImage(..., 1/255, ..., swapRB=true)`。
- 输出经 NMS 后按置信度排序。

当前默认：

- `detectYolo.enabled = false`。
- 主程序不会加载或调用 ONNX。
- `detector` 字段为 `traditional_v1`。

`hybrid` 模式代码仍保留，重新启用 YOLO 后：

- 传统候选送 YOLO，用 `confirmThreshold`。
- YOLO 检出后把目标中心更新到 YOLO box 中心。
- 补检窗口送 YOLO，用 `supplementThreshold`。
- 传统和 YOLO 输出统一进入 `finalKept`。

### 5.5 ROI 保存与 manifest

保存逻辑在 `VideoWorker::detectCandidates(...)` 末段。

每个最终候选：

- 计算 `panoX/panoY`。
- 计算 `angle`。
- 调用 `fixedCropRgb32(...)` 以目标中心裁剪。
- 写 JPG。
- 追加 `manifest.jsonl`。
- emit `candidateDetected(...)`。

注意：

- manifest 是追溯检测细节的主数据。
- 新雷达窗口当前接收的是信号里的部分字段，不是 manifest 全量字段。

### 5.6 新目标雷达目标合并

目标合并在 `TargetRadarWindow::addOrUpdateTarget(...)`：

- 先按保留时间清理旧目标。
- 新目标与已有目标按方位角差和 panoY 差匹配。
- 当前方位角合并阈值为 2 度。
- 纵向容差为 `max(64, fullHeight / 32)`。
- 命中后保留原 id，增加 hits，更新 confidence/score/time/state。

状态分类：

- 超过保留时间 2/3：`stale`。
- hits >= 3 且 confidence >= 0.45：`confirmed`。
- hits >= 2 或 confidence >= 0.55：`tracking`。
- 其他：`new`。

### 5.7 ROI 列表替换策略

实现位置：

- `TargetRadarWindow::visibleTargetIndexes()`
- `TargetRadarWindow::targetDisplayPriority(...)`

综合优先级：

- `confirmed` 权重最高。
- `tracking` 次之。
- `new` 正常显示。
- `stale` 降权。
- confidence 越高越优先。
- freshness 越高越优先。
- hits 越多越优先。
- score 越高越优先。
- 当前选中目标优先保留。

### 5.8 正北显示校准

实现位置：

- `TargetRadarWidget::setNorthOffsetDeg(...)`
- `TargetRadarWidget::rawToDisplayAngle(...)`
- `TargetRadarWidget::displayToRawAngle(...)`
- `TargetRadarWindow::applyPendingNorthOffset(...)`
- `TargetRadarWindow::resetNorthOffset(...)`

显示层统一旋转：

- 雷达背景全景先按 `northOffsetDeg` 做水平循环平移，再投影成极坐标。
- 目标点绘制使用显示角。
- 扫描线绘制使用显示角。
- 鼠标点击返回原始设备角。

禁止事项：

- 不要把 `northOffsetDeg` 写入 `dmx_config.json`。
- 不要用显示角覆盖 `TargetRecord.azimuthDeg`。
- 不要用显示角写入 manifest。

## 6. 配置重点

当前 `dmx_config.json` 中最关键参数：

```text
paths.dataRoot = /mnt/dmx4t/data
paths.recordRoot = /mnt/dmx4t/data/recordings
paths.rawLogRoot = /mnt/dmx4t/data/raw_log

panorama.fullWidth = 65536
panorama.fullHeight = 4096
panorama.thumbWidth = 8192
panorama.thumbHeight = 240

detect.enabled = true
detect.stream = BW
detect.saveRoot = /mnt/dmx4t/data/candidates
detect.cropSize = 256
detect.backgroundFrames = 3
detect.skyMargin = 16
detect.skyShrinkPixels = 32
detect.maxCandidatesPerFrame = 3
detect.minArea = 4
detect.maxArea = 400
detect.thresholdK = 3.5
detect.minContrast = 25
detect.nmsRadius = 96
detect.backgroundUpdateAlpha = 0.02
detect.backgroundUpdateInterval = 4
detect.backgroundProtectRadius = 128

detectYolo.enabled = true
detectYolo.modelPath = /home/sht/data/低小慢数据/yolo26s/result/jiao/pt/weights/best.onnx
detectYolo.mode = hybrid
detectYolo.classNames = ["drone", "bird"]
detectYolo.inputSize = 640
detectYolo.confirmThreshold = 0.03
detectYolo.supplementThreshold = 0.08
detectYolo.supplementInterval = 8
detectYolo.supplementMaxWindows = 6

detectUi.radarHoldMs = 20000
detectUi.maxRadarTargets = 20
```

环境变量也可覆盖部分 YOLO 配置：

- `DMX_RAW_LOG_ROOT`
- `DMX_SHARE_MOUNT`
- `DMX_PATH_PORT`
- `DMX_REPLAY_MODE`
- `DMX_YOLO_ENABLED`
- `DMX_YOLO_MODEL`
- `DMX_YOLO_MODE`
- `DMX_YOLO_INPUT_SIZE`
- `DMX_YOLO_SUPPLEMENT_INTERVAL`
- `DMX_YOLO_SUPPLEMENT_MAX_WINDOWS`

## 7. 数据与输出路径

YOLO 相关数据：

```text
/home/sht/data/低小慢数据/yolo26s/result/jiao/pt/weights
```

当前 ONNX 权重：

```text
/home/sht/data/低小慢数据/yolo26s/result/jiao/pt/weights/best.onnx
```

疑似目标输出：

```text
/mnt/dmx4t/data/candidates/YYYYMMDD/HH/
```

每小时目录包含：

- 目标 ROI JPG。
- `manifest.jsonl`。

录制输出：

```text
/mnt/dmx4t/data/recordings
```

原始 UDP 文本日志：

```text
/mnt/dmx4t/data/raw_log/YYYYMMDD/HH/udp_8001_YYYYMMDD_HH_启动时间_pid.jsonl
```

本地模拟基准图像镜像根目录：

```text
/mnt/dmx4t/data/replay_sources/baseline_20260723
```

镜像目录内部应保留设备 `/data` 下的相对层级，例如：

```text
/mnt/dmx4t/data/replay_sources/baseline_20260723/raw/20260723/RGB
/mnt/dmx4t/data/replay_sources/baseline_20260723/raw/20260723/BW
```

当前 20260723 测试版直接读取：

```text
/mnt/dmx_share/raw/20260723/RGB
/mnt/dmx_share/raw/20260723/BW
```

`dmx_test` 独立输出：

```text
/mnt/dmx4t/data/dmx_test/logs
/mnt/dmx4t/data/dmx_test/raw_log
/mnt/dmx4t/data/dmx_test/candidates
/mnt/dmx4t/data/dmx_test/runs
```

## 8. 验证记录

### 2026-07-30

验证内容：

- 对比实时角度日志、`turntable.direction=left` 配置和回放帧号映射。
- 核对 RGB/BW 像素变换和 `PanoramaCache` 角度块写入逻辑。
- 重新构建测试版和实时版。
- 使用 `QT_QPA_PLATFORM=offscreen` 对测试版执行 13 秒本地端到端回放。
- 重跑回放 Python 测试、AppConfig Qt 测试和 PanoramaCache Qt 测试。

验证结果：

- 日志确认回放映射为 `index=0,1,2 -> angle=0,337.5,315`，方向为左转，像素处理为 `CCW90+mirrorH`。
- 发送端短测发出 52 条 UDP；RGB/BW 各稳定解码 `2 frames/s`，队列通常为 0，无读图或解码失败。
- 主雷达由 2Hz 跳变改为约 33Hz 连续扫描，固定速度为 `45°/s`。
- `bash build_test.sh` 通过，生成 `build_linux_test/DMX_test`。
- `bash build_linux.sh` 通过，生成 `build_linux/DMX`。
- 回放 Python 测试 `4 passed`。
- AppConfig Qt 测试 `6 passed`。
- PanoramaCache Qt 测试 `5 passed`。
- 第二轮测试版编译通过，正式 `build_linux/DMX` 未重新构建。
- 回放启动日志确认 `skyShrink=48px`；第三圈建模后检测日志确认实际使用 `shrink=48px`。
- 36 秒回放发送 142 条路径 UDP，无读图或解码错误；首次 mask 生成耗时 `6535ms`，期间 BW 队列峰值 14，并记录 `drop=13`。
- `bash -n run_dmx_test.sh`、回放 Python 测试 4 项、AppConfig Qt 测试 6 项和 PanoramaCache Qt 测试 5 项均通过。
- 用户查看 `48px` 全景叠加图后将测试参数提高到 `64px`；隔离回放导出确认 `manifest.json` 中 `shrinkPixels=64`、最终覆盖率 15.22%，正式程序仍未重建。

### 2026-07-29

验证内容：

- 解析 `/mnt/dmx4t/data/raw_log/20260723` 下全部 8 个 JSONL 文件。
- 校验 19,422 条报文格式、BW/RGB 配对、帧序号连续性、重复和缺失情况。
- 逐条映射并检查 `/mnt/dmx_share/raw/20260723` 下图像文件。
- 实测固定 8 秒每圈发送节奏。
- 构建独立 `/home/sht/work/DMX_qt/build_linux_test/DMX_test`。
- 使用本地隔离端口对 `DMX_test` 做短程端到端回放。
- 创建 `/home/sht/桌面/dmx_test.desktop` 并设置可执行权限。

验证结果：

- 日志解析错误 0，BW/RGB 各 9,711 帧，序号 `0..9710` 连续。
- 图像路径存在 19,422/19,422。
- Python 单元测试 `4 passed`。
- AppConfig Qt 单元测试 `6 passed`。
- 发送实测：组周期约 `500.0ms`，BW-RGB 间隔约 `102.0ms`。
- `bash build_test.sh` 编译通过。
- `bash build_linux.sh` 编译通过，实时主程序默认端口仍为 `8001`。
- 端到端短测收到全部 24/24 条报文；测试程序每路稳定解码 `2 frames/s`，无 `NOT_READY`、`DECODE_FAIL` 或读取失败。
- 测试程序绑定隔离端口，日志确认 `Bind 18011 ok`；正式桌面配置使用 `18001`，未占用实时 `8001`。

### 2026-07-23

验证内容：

- 修复并挂载本地 4T NTFS 数据盘 `/mnt/dmx4t`：
  - 原因：`/dev/sda1` 被 NTFS dirty 标记阻止挂载。
  - 操作：`ntfsfix -d /dev/sda1` 后重置 systemd mount/automount 状态并挂载。
  - 当前：`/mnt/dmx4t` 为 `/dev/sda1 ntfs3`，容量约 `3.7T`，可用约 `1.5T`。
  - `/srv/dmx` loop 镜像也已恢复挂载。
- 当前设备共享盘 `/mnt/dmx_share` 已挂载：
  - 来源：`//192.168.4.1/data`。
  - 主程序仍按 `paths.shareMount=/mnt/dmx_share` 读取设备发来的 `/data/...` 图像路径。
  - 但 PC 端写入 `/mnt/dmx_share` 被服务端拒绝，见 `3.8`。
- 新增 8001 原始 UDP 文本日志：
  - 修改：`appconfig.h/.cpp`、`dmx_config.json`、`videothread.h/.cpp`。
  - 新配置：`paths.rawLogRoot=/mnt/dmx4t/data/raw_log`。
  - 日志文件按 `YYYYMMDD/HH/udp_8001_YYYYMMDD_HH_启动时间_pid.jsonl` 保存。
  - 每条 UDP 记录 `rxMs/rxIso/sender/len/text`，并立即 `flush()`。
- 已创建本地回放基准图像镜像目录：
  - `/mnt/dmx4t/data/replay_sources/baseline_20260723/raw/20260723/RGB`
  - `/mnt/dmx4t/data/replay_sources/baseline_20260723/raw/20260723/BW`
- 追加 YOLO 硬确认诊断日志：
  - 修改：`videothread.cpp`。
  - 日志标签：`YOLODBG`。
  - 诊断目的：区分 YOLO 无框、无 drone、分数低于确认阈值、bird 分数压制、无效框和整圈 NMS 等拒绝原因。
- 修正 YOLO raw 输出解析与低阈值配置：
  - 修改：`videothread.cpp`、`appconfig.cpp`、`dmx_config.json`。
  - `1x6x8400` 按 raw YOLO 两类输出解析。
  - 当前测试确认阈值为 `0.0001`。
- 增加 YOLO 空结果传统兜底与更详细诊断：
  - 修改：`videothread.cpp`、`appconfig.h/.cpp`、`dmx_config.json`。
  - 当前 `detectYolo.fallbackTraditionalOnEmpty=true`。
  - 新 `YOLODBG` 字段记录模型输出形状、raw 最大分、阈值前候选数和 NMS 后候选数。
- 增加天空 mask 检测边缘内收：
  - 修改：`videothread.cpp`、`appconfig.h/.cpp`、`dmx_config.json`。
  - 当前 `detect.skyShrinkPixels=32`。
  - 用于减少树冠、房屋和地物边缘被划为天空后的误检。

验证命令：

```bash
python3 -m json.tool dmx_config.json >/dev/null
bash build_linux.sh
```

结果：

- JSON 校验通过。
- 编译通过，输出 `/home/sht/work/DMX_qt/build_linux/DMX`。
- YOLO 诊断日志修改后再次执行 `bash build_linux.sh`，编译通过。
- YOLO raw 输出解析和低阈值配置修改后再次执行 JSON 校验与 `bash build_linux.sh`，均通过。
- YOLO 空结果传统兜底与详细诊断修改后再次执行 JSON 校验与 `bash build_linux.sh`，均通过。
- 天空 mask 边缘内收配置修改后再次执行 JSON 校验与 `bash build_linux.sh`，均通过。
- 使用 `QT_QPA_PLATFORM=offscreen ./build_linux/DMX` 短启动后，向本机 `127.0.0.1:8001` 发送模拟 UDP，确认 raw log JSONL 正常生成并包含原始 `text`。
- 验证用 raw log 文件已删除，避免污染正式采集基准日志。

### 2026-07-15

验证内容：

- 使用 2026-07-14 17 点外场 BW 录制数据验证“新 YOLO 权重 + 天空区域提取 + 增强预处理 + 512 滑窗 YOLO 推理”的分析流程。
- 输入目录：`/mnt/dmx4t/data/recordings/20260714/bw/17`。
- 选取首个完整 AB 半全景：
  - `BW_20260714_170002_2366-A.jpg`
  - `BW_20260714_170002_2366-B.jpg`
- A/B 半幅按当前流程分别处理，未拼接成一张图后再处理。
- 新权重目录：`/home/sht/data/低小慢数据/yolo26s/result/jiao/pt/weights`。
- 本次使用：`/home/sht/data/低小慢数据/yolo26s/result/jiao/pt/weights/best.onnx`。
- 旧权重 `/home/sht/data/低小慢数据/yolo26s/result/lu/best.onnx` 已不再用于本次分析，`dmx_config.json` 也已更新到新 ONNX。
- 按 `detect.backgroundFrames = 3`，分别使用同一半幅 A/B 序列的前 3 张建立背景。
- 按用户确认的新逻辑，不再做地平线分割，不输出地平线 profile、天空 mask 中间图或 CSV。
- 当前分析流程：
  - 灰度化。
  - `medianBlur`，核大小 3。
  - 3 帧背景平均。
  - 在背景图上按亮度、低纹理、低梯度和上半部位置生成天空候选。
  - 保留多个互不连通的天空候选块，不强制合并为一个连通域。
  - 填充天空块内部小孔洞。
  - 对边缘高纹理/高梯度区域做清理和轻微内缩，减少树叶、建筑物混入。
  - 同时输出天空区域叠加在原图上的效果图。
  - 对原图天空区域做增强预处理：CLAHE 直方图均衡化、非锐化锐化、TopHat/BlackHat 小目标响应增强，非天空区域用天空灰度中值填充。
  - 针对严重曝光区域降低 CLAHE 和形态学增强强度，饱和区域不再强行制造细节。
  - 在每个天空连通块外接框上做 `512x512` 滑窗推理。
  - 滑窗步长 `256`，即 50% 重叠。
  - 每个连通块最后一列/最后一行强制贴边补齐覆盖。
  - 天空块小于 `512x512` 时，以天空块中心生成一个 `512x512` 窗口。
  - 窗口超出图像边界时，使用边界复制方式填充到 `512x512`。
  - 对靠近天空区域边缘的检测框做距离惩罚，中心越靠近边缘，adjusted score 越低。
  - 对低纹理严重曝光区域做额外分数惩罚。
  - 新模型输出分数尺度很低，本次离线分析使用 `raw >= 0.0003` 入候选，`adjusted >= 0.0003` 排序。
  - 每个半幅只保留 adjusted score 排名前 12 的候选用于可视化。
  - 将最终识别框直接标注回原始半全景图。

输出目录：

```text
/mnt/dmx4t/DMX_yangben/20260714/_analysis
```

主要结果：

- 已删除上一轮中间文件和旧检测结果图。
- 当轮目录只保留天空区域图和新权重检测结果图；后续已按用户要求删除并改为传统点目标检测结果。
- A 半幅滑窗 169 个，raw 弱候选 28 个，显示 adjusted 排名前 12 个。
- B 半幅滑窗 528 个，raw 弱候选 90 个，显示 adjusted 排名前 12 个。
- 当前显示出的候选类别均为 `drone`。
- 天空区域总览图：`/mnt/dmx4t/DMX_yangben/20260714/_analysis/00_AB_sky_regions_on_original.jpg`。
- A 半幅天空图：`/mnt/dmx4t/DMX_yangben/20260714/_analysis/BW_20260714_170002_2366-A_sky_regions_on_original.jpg`。
- B 半幅天空图：`/mnt/dmx4t/DMX_yangben/20260714/_analysis/BW_20260714_170002_2366-B_sky_regions_on_original.jpg`。
- 检测总览图：`/mnt/dmx4t/DMX_yangben/20260714/_analysis/00_AB_jiao_lowconf_edgeweighted_512scan_on_original.jpg`。
- A 半幅检测图：`/mnt/dmx4t/DMX_yangben/20260714/_analysis/BW_20260714_170002_2366-A_jiao_lowconf_edgeweighted_512scan_on_original.jpg`。
- B 半幅检测图：`/mnt/dmx4t/DMX_yangben/20260714/_analysis/BW_20260714_170002_2366-B_jiao_lowconf_edgeweighted_512scan_on_original.jpg`。
- 按用户要求追加 A 图全 raw 候选导出：
  - 使用 `raw >= 0.00005`，不做边缘/曝光降权，不做 Top-K 截断，只做全图 NMS 去重。
  - A 半幅得到 93 个 raw 候选。
  - 全图标注：`/mnt/dmx4t/DMX_yangben/20260714/_analysis/BW_20260714_170002_2366-A_ALL_RAW_CANDIDATES_raw5e-05_on_original.jpg`。
  - 候选 CSV：`/mnt/dmx4t/DMX_yangben/20260714/_analysis/BW_20260714_170002_2366-A_ALL_RAW_CANDIDATES_raw5e-05.csv`。
  - 候选裁剪页：
    - `/mnt/dmx4t/DMX_yangben/20260714/_analysis/BW_20260714_170002_2366-A_ALL_RAW_CANDIDATE_CROPS_raw5e-05_page01.jpg`
    - `/mnt/dmx4t/DMX_yangben/20260714/_analysis/BW_20260714_170002_2366-A_ALL_RAW_CANDIDATE_CROPS_raw5e-05_page02.jpg`

说明：

- 本次没有写入候选保存目录，也没有修改程序正式检测链路代码。
- 本次为离线分析可视化；若要作为正式逻辑，需要把天空区域提取和滑窗推理策略接入 `VideoWorker::detectCandidates()`。
- 新模型在当前 OpenCV DNN 解析下输出分数极低，常规 `0.03/0.08` 阈值没有候选；正式接入前必须确认 ONNX 导出和置信度尺度。
- 当前结果仍是候选排序，不应直接视为最终低误报检测结果。AB 图实际只有一个目标，后续需要结合人工标注位置做 Top-1/Top-K 命中率评估。
- A 图全 raw 候选目视看仍以天空/建筑/树缘线状伪框为主，真实目标是否进入候选需用户按裁剪页编号人工确认。

追加传统算法验证：

- 用户确认 YOLO 候选未命中真实无人机，要求暂时不调用 yolo26s/ONNX，只用传统识别算法搜索远距离点状无人机。
- 参考样本：`/mnt/dmx4t/DMX_yangben/20260714/_analysis/wrj.jpg`。
  - `wrj.jpg` 尺寸约 `710x545`。
  - 无人机位于图像正中附近，表现为紧凑暗点，略有横向展开，但不是长线。
- 已删除当轮 YOLO 候选、天空 mask 等旧输出，仅保留 `wrj.jpg` 和最新传统算法结果。
- 传统算法要点：
  - 从 `wrj.jpg` 中心裁剪学习紧凑暗点纹理和高通模板。
  - 对 A/B 半幅仍按前三张建立背景和天空区域约束，只在天空候选区域内找点状目标。
  - 灰度预处理使用中值滤波、CLAHE 和局部背景差分。
  - 使用 Gaussian local background minus image、BlackHat 响应和中心-环带暗点扩散响应强化远距离点目标。
  - 对水平线响应做惩罚，过滤电线、云层/蓝天交界线等线状候选。
  - 按面积、宽高、长宽比、紧致度、暗点对比度、中心-环带响应、模板相关性综合打分。
  - 保留多个互不连通天空块，不强制只取一块天空。
  - 输出候选框直接标注在原始半全景图上，并输出候选裁剪页和 CSV。
- 最新输出目录仍为：`/mnt/dmx4t/DMX_yangben/20260714/_analysis`。
- 最新结果文件：
  - `wrj_learned_center_target.jpg`
  - `00_AB_traditional_point_candidates_on_original.jpg`
  - `BW_20260714_170002_2366-A_traditional_point_candidates_on_original.jpg`
  - `BW_20260714_170002_2366-A_traditional_point_candidates.csv`
  - `BW_20260714_170002_2366-A_traditional_point_candidate_crops_page01.jpg`
  - `BW_20260714_170002_2366-A_wrj_template_match_location.jpg`
  - `BW_20260714_170002_2366-A_traditional_candidate1_vs_wrj_zoom.jpg`
  - `BW_20260714_170002_2366-B_traditional_point_candidates_on_original.jpg`
  - `BW_20260714_170002_2366-B_traditional_point_candidates.csv`
  - `BW_20260714_170002_2366-B_traditional_point_candidate_crops_page01.jpg`
  - `BW_20260714_170002_2366-B_traditional_point_candidate_crops_page02.jpg`
  - `BW_20260714_170002_2366-B_traditional_point_candidate_crops_page03.jpg`
- 传统算法候选数量：
  - A 半幅 65 个候选。
  - B 半幅 168 个候选。
- A 半幅真实目标结论：
  - A 图传统候选 #1 基本就是 `wrj.jpg` 中心真实无人机目标。
  - 候选 #1 中心：`x=18455, y=1558`。
  - 候选 #1 框：`x1=18445, y1=1547, x2=18466, y2=1564`。
  - 候选 #1 分数：`238.4555`，模板相关性 `0.5827`。
  - 使用 `wrj.jpg` 整图高通模板在 A 图中核对，最佳匹配中心为 `x=18459, y=1560`，与候选 #1 基本重合。
  - 已补充局部对照图 `BW_20260714_170002_2366-A_traditional_candidate1_vs_wrj_zoom.jpg`，左侧为 `wrj.jpg` 中心参考目标，右侧为 A 图候选 #1，二者均呈中心暗核和轻微横向展开形态。
- 2026-07-16 追加误检压制验证：
  - 在传统候选 CSV 上追加强候选后过滤，不重新调用 YOLO/ONNX。
  - 已新增离线工具：`tools/reduce_false_positives_20260714.py`。
  - 工具输入：
    - 当前传统候选 CSV：`*_traditional_point_candidates.csv`
    - 原始 AB 半全景图：`/mnt/dmx4t/data/recordings/20260714/bw/17/BW_20260714_170002_2366-A/B.jpg`
    - 参考纹理图：`/mnt/dmx4t/DMX_yangben/20260714/_analysis/wrj.jpg`
  - 工具策略：
    - 强候选单帧可直接保留，避免把当前 A 图真实目标误删。
    - 中等候选必须在邻近帧中通过局部模板和点目标特征确认，否则剔除。
    - 弱候选按模板相关性、中心-环带响应、对比度、长宽比、面积、紧致度给出剔除原因。
  - 当前强过滤条件：
    - `score >= 150`
    - `template_corr >= 0.25`
    - `center_ring >= 35`
    - `contrast >= 10`
    - `0.7 <= aspect <= 2.2`
    - `60 <= area <= 180`
    - `compactness >= 0.55`
  - 中等候选条件：
    - `score >= 80`
    - `template_corr >= 0.15`
    - `center_ring >= 18`
    - `contrast >= 5`
    - `0.6 <= aspect <= 2.2`
    - `40 <= area <= 240`
    - `compactness >= 0.45`
  - 当前运行命令：
    - `python3 tools/reduce_false_positives_20260714.py`
  - 本次使用前 7 个 AB 对做中等候选时序复核。
  - A 半幅从 65 个候选压到 1 个，保留候选 #1，即真实无人机位置。
  - B 半幅从 168 个候选压到 0 个。
  - 总候选 233 个，最终保留 1 个。
  - 输出：
    - `00_AB_traditional_strong_filtered_overview.jpg`
    - `BW_20260714_170002_2366-A_traditional_strong_filtered_on_original.jpg`
    - `BW_20260714_170002_2366-A_traditional_strong_filtered.csv`
    - `BW_20260714_170002_2366-B_traditional_strong_filtered_on_original.jpg`
    - `BW_20260714_170002_2366-B_traditional_strong_filtered.csv`
    - `00_AB_traditional_fp_reduction_final_overview.jpg`
    - `BW_20260714_170002_2366-A_traditional_fp_reduction_final_on_original.jpg`
    - `BW_20260714_170002_2366-B_traditional_fp_reduction_final_on_original.jpg`
    - `traditional_fp_reduction_accepted_crops.jpg`
    - `traditional_fp_reduction_decisions.csv`
    - `traditional_fp_reduction_temporal_hits.csv`
  - `traditional_fp_reduction_decisions.csv` 会记录每个候选的 `accept/reject`、剔除原因和时序核对结果，便于继续调阈值。
  - 当前阈值在这组 AB 图有效，但不应直接当作最终全场景固定阈值；后续需要用更多含目标/无目标样本做召回率和误检率评估。
- 2026-07-16 按用户要求生成三张 AB 全景展示图：
  - 新增脚本：`tools/make_20260714_three_views.py`。
  - 输出目录：`/mnt/dmx4t/DMX_yangben/20260714/_analysis/traditional`。
  - A/B 仍分别处理，只在最终展示阶段把 A 放上方、B 放下方生成 AB 预览图。
  - 三张主展示图：
    - `01_AB_sky_mask_on_original.jpg`：天空区域 mask 叠加在原图上的展示。
    - `02_AB_yolov26s_filtered_on_original.jpg`：YOLOv26s 新权重滑窗推理、天空约束、边缘降权和 NMS 筛选后的框。
    - `03_AB_traditional_filtered_on_original.jpg`：当前传统点目标算法去重筛选后的框。
  - 同时保存 A/B 原始分辨率分图：
    - `01_A_sky_mask_on_original_full.jpg`
    - `01_B_sky_mask_on_original_full.jpg`
    - `02_A_yolov26s_filtered_on_original_full.jpg`
    - `02_B_yolov26s_filtered_on_original_full.jpg`
    - `03_A_traditional_filtered_on_original_full.jpg`
    - `03_B_traditional_filtered_on_original_full.jpg`
  - 天空 mask 覆盖率：
    - A：`0.080515`
    - B：`0.215910`
  - YOLOv26s 筛选结果：
    - A：天空窗口 225 个，raw 候选 1603 个，最终保留 12 个。
    - B：天空窗口 483 个，raw 候选 554 个，最终保留 12 个。
    - 候选明细：`02_yolov26s_filtered_candidates.csv`。
  - 传统算法筛选结果：
    - 最终保留 1 个候选，位于 A 图 `x=18455, y=1558`。
    - 候选明细：`03_traditional_filtered_candidates.csv`。
  - 第一版天空 mask 曾因阈值过松导致 A 图覆盖率约 `0.97`，已改为更严格的亮度、梯度、纹理条件后重跑；当前 A 图不再整图泛蓝。
- 2026-07-16 针对 A 图反光树叶误识别为天空的问题，追加前三个完整 AB 全景对的时序天空 mask 验证：
  - 新增脚本：`tools/make_20260714_temporal_sky_mask.py`。
  - 输出目录：`/mnt/dmx4t/DMX_yangben/20260714/_analysis/sky—mask`。
  - 使用前三个完整 AB 对：
    - `BW_20260714_170002_2366-A/B.jpg`
    - `BW_20260714_170011_2382-A/B.jpg`
    - `BW_20260714_170020_2398-A/B.jpg`
  - A/B 分别处理，只在最终预览图中上下排列展示。
  - 新 mask 逻辑：
    - 高置信天空种子。
    - 在候选天空中按连通域保留含天空种子的区域，支持多块天空。
    - 加入梯度、局部纹理、Laplacian 高频约束，压制反光树叶和建筑边缘。
    - 使用前三帧时序一致性作为辅助，要求 `support >= 2` 且时间方差不过高。
    - 对已确认天空连通块内部的小孔洞做小面积填补，避免真实无人机这类高频暗点把 sky mask 打穿。
    - 输出 `sure_sky` 和 `maybe_sky` 双层 mask；后续检测建议只使用 `sure_sky`，`maybe_sky` 仅用于人工复核或低权重区域。
  - 主结果图：
    - `first_AB_sure_maybe_sky_mask_on_original.jpg`：青色为 `sure_sky`，黄色为 `maybe_sky`。
    - `first_AB_sure_sky_mask_on_original.jpg`：只显示推荐用于检测的 `sure_sky`。
  - A/B 原始分辨率分图：
    - `first_A_sure_maybe_sky_on_original_full.jpg`
    - `first_A_sure_sky_on_original_full.jpg`
    - `first_B_sure_maybe_sky_on_original_full.jpg`
    - `first_B_sure_sky_on_original_full.jpg`
  - mask 文件：
    - `first_A_sure_sky_mask.png`
    - `first_A_maybe_sky_mask.png`
    - `first_A_temporal_support.png`
    - `first_B_sure_sky_mask.png`
    - `first_B_maybe_sky_mask.png`
    - `first_B_temporal_support.png`
  - 统计：
    - A：`sure_ratio = 0.063177`，`maybe_ratio = 0.001858`。
    - B：`sure_ratio = 0.197066`，`maybe_ratio = 0.016798`。
  - 验证：A 图真实目标坐标 `x=18455, y=1558` 位于 `first_A_sure_sky_mask.png` 内，像素值为 `255`。
- 2026-07-16 针对云层天空零散问题，追加通用 v1 天空 mask：
  - 用户强调不能按 A/B 特判；v1 对每个半全景都同时执行 `clear_sky` 和 `cloud_sky` 两条通用分支。
  - 输出目录：`/mnt/dmx4t/DMX_yangben/20260714/_analysis/sky—mask/v1`。
  - 脚本仍为：`tools/make_20260714_temporal_sky_mask.py`。
  - v1 逻辑：
    - `clear_sky`：严格低梯度、低纹理、高置信天空种子。
    - `cloud_sky`：允许大尺度云层灰度起伏，但要求局部高频边缘密度低、连通块与可靠天空锚点有足够接触，避免孤立反光树叶进入。
    - `detect_sky = clear_sky ∪ cloud_sky`，后续检测建议使用该 mask。
    - `maybe` 仅作为边界复核，不建议直接参与检测。
  - 主结果图：
    - `first_AB_v1_clear_cloud_maybe_on_original.jpg`：分层显示，cyan 为 `clear_sky`，blue 为 `cloud_sky`，yellow 为 `maybe`。
    - `first_AB_v1_detect_sky_mask_on_original.jpg`：最终用于检测的 `detect_sky` 叠加图。
  - A/B 原始分辨率分图：
    - `first_A_v1_clear_cloud_maybe_on_original_full.jpg`
    - `first_A_v1_detect_sky_on_original_full.jpg`
    - `first_B_v1_clear_cloud_maybe_on_original_full.jpg`
    - `first_B_v1_detect_sky_on_original_full.jpg`
  - mask 文件：
    - `first_A_v1_detect_sky_mask.png`
    - `first_A_v1_clear_sky_mask.png`
    - `first_A_v1_cloud_sky_mask.png`
    - `first_B_v1_detect_sky_mask.png`
    - `first_B_v1_clear_sky_mask.png`
    - `first_B_v1_cloud_sky_mask.png`
  - 按用户要求，最终 `detect_sky` 追加连通域碎片剪枝：
    - 天空区域不保留孤立零散小块。
    - 保留面积足够大的多块天空，允许被树/建筑分割后互不连通。
    - 按连通域面积、相对最大天空块比例、宽高、填充率和边界复杂度过滤碎片。
    - 区域增长前和最终合成后都追加细通道掐断，使用形态开运算切断过窄桥接，避免天空区域通过不符合逻辑的细缝蔓延到反光树叶或建筑边缘。
  - 当前 v1 统计：
    - A：`detect_sky_ratio = 0.064846`，其中 `clear_ratio = 0.063146`，`cloud_ratio = 0.001670`，`maybe_ratio = 0.003380`，最终连通块数量 1。
    - B：`detect_sky_ratio = 0.212375`，其中 `clear_ratio = 0.198874`，`cloud_ratio = 0.013447`，`maybe_ratio = 0.016734`，最终连通块数量 3。
  - 验证：A 图真实目标坐标 `x=18455, y=1558` 位于 `first_A_v1_detect_sky_mask.png` 内，像素值为 `255`。
  - 按用户要求另导出只包含 A/B 两张最终 `detect_sky` 原图叠加图的目录：
    - `/mnt/dmx4t/DMX_yangben/20260714/_analysis/sky—mask/v1/ab_only/A_detect_sky_mask_on_original.jpg`
    - `/mnt/dmx4t/DMX_yangben/20260714/_analysis/sky—mask/v1/ab_only/B_detect_sky_mask_on_original.jpg`
- 2026-07-16 按用户要求将当前满意的 v1 天空 mask 和传统点目标识别集成到 DMX 主程序：
  - 修改 `VideoWorker::updateBackgroundTile(...)`，每个 tile 前 `detect.backgroundFrames` 帧同时用于背景和 `buildTemporalSkyMaskV1(...)`。
  - `DetectBackgroundTile` 新增 `initialFrames`，用于保存建模期帧。
  - `VideoWorker::detectCandidates(...)` 默认使用 `traditional_v1`：
    - v1 天空 mask 轻微内缩后作为检测 mask。
    - CLAHE + 轻锐化 + BlackHat 暗点增强。
    - 局部背景差分提取点扩散响应。
    - 按面积、长宽比、紧致度、中心-环带响应、模板相关性综合筛选。
  - 新增 `detect.referenceTemplatePath` 配置和环境变量 `DMX_DETECT_REFERENCE_TEMPLATE`。
  - `dmx_config.json` 当前设置：
    - `detect.referenceTemplatePath = /mnt/dmx4t/DMX_yangben/20260714/_analysis/wrj.jpg`
    - `detectYolo.enabled = false`
  - YOLOv26s 代码路径仍保留，后续可通过配置重新启用。
  - manifest 中 `skyMaskMode` 改为 `temporal_sky_v1`。
  - 编译验证：
    - 命令：`bash build_linux.sh`
    - 结果：通过。
    - 输出：`/home/sht/work/DMX_qt/build_linux/DMX`。
- 当前限制：
  - 正式链路暂未接入离线脚本中的“中等候选跨帧时序确认”，实时误检率还需外场连续数据验证。
  - 传统阈值主要按 2026-07-14 首个 AB 样本调过，后续需要更多样本验证召回和误报。
- 2026-07-16 追加 16/17/18 点 BW 批数据的 512 训练窗口预览：
  - 新增脚本：`tools/generate_20260714_traditional_512_preview.py`。
  - 输入根目录：`/mnt/dmx4t/data/recordings/20260714/bw`。
  - 默认处理小时目录：`16 17 18`。
  - 逻辑：
    - 每个小时、每个 A/B 半幅取前三张建立 v1 天空 mask。
    - 在天空 mask 内运行当前传统点目标识别。
    - 候选中心裁剪 `512x512`，越界用黑边填充。
    - 每张源图最多保存 1 个候选；同一小时同一 A/B 半幅内按中心坐标做全局去重，默认半径 `384`，避免负样本重复爆炸。
  - 预览输出目录：`/mnt/dmx4t/DMX_yangben/20260714/traditional_512_preview`。
  - 当前已生成 10 张预览窗口：
    - 干净训练窗口：`traditional_512_preview/images/*.jpg`
    - 预览总览：`traditional_512_preview/preview_contact_sheet.jpg`
    - 候选明细：`traditional_512_preview/manifest.csv`
    - 参考模板：`traditional_512_preview/reference_template_used.jpg`
  - `wrj.jpg` 曾被清理到回收站，本次已从 `/mnt/dmx4t/.Trash-1000/files/wrj.jpg` 复制回配置路径：
    - `/mnt/dmx4t/DMX_yangben/20260714/_analysis/wrj.jpg`
- 2026-07-16 按用户要求继续完成 16/17/18 点全部 BW 数据的 512 训练候选窗口导出：
  - 使用脚本：`tools/generate_20260714_traditional_512_preview.py`。
  - 执行命令：
    - `python3 tools/generate_20260714_traditional_512_preview.py --out-dir /mnt/dmx4t/DMX_yangben/20260714/traditional_512_dataset --max-crops 0 --global-dedup-radius 512 --contact-sheet-limit 30 --progress-every 20`
  - 处理输入：
    - `/mnt/dmx4t/data/recordings/20260714/bw/16`
    - `/mnt/dmx4t/data/recordings/20260714/bw/17`
    - `/mnt/dmx4t/data/recordings/20260714/bw/18`
  - 全量结果：
    - 源图总数：`879`。
    - 保存 `512x512` 候选窗口：`97`。
    - 所有输出图已检查尺寸，均为 `512x512`。
    - 未生成每张源图的 debug overlay，避免无用调试数据膨胀。
  - 输出目录：`/mnt/dmx4t/DMX_yangben/20260714/traditional_512_dataset`。
  - 核心文件：
    - `images/*.jpg`：干净训练窗口，共 `97` 张。
    - `manifest.csv`：候选来源、中心坐标、bbox、score、contrast、template_corr 等字段。
    - `run_summary.json`：运行参数和分组统计。
    - `preview_contact_sheet.jpg`：仅前 `30` 张的轻量预览。
  - 分组统计：
    - processed：`16A=134`，`16B=134`，`17A=263`，`17B=261`，`18A=44`，`18B=43`。
    - saved：`16A=11`，`16B=9`，`17A=14`，`17B=42`，`18A=19`，`18B=2`。
  - 控制负样本数量策略：
    - 每张源图最多保存一个候选。
    - 同一小时同一 A/B 半幅内按候选中心做全局去重，半径 `512`。
    - 不保存 per-image debug overlay。
    - contact sheet 只抽前 `30` 张。
- 2026-07-20 使用新增训 `weights2` 做“传统候选 + YOLOv26s 二次过滤/融合”单个完整 AB 全景验证：
  - 新模型目录：`/home/sht/data/低小慢数据/yolo26s/result/jiao/pt/weights2`。
  - 本次使用 ONNX：`/home/sht/data/低小慢数据/yolo26s/result/jiao/pt/weights2/best.onnx`。
  - 输入样例仍为首个完整 AB 半全景：
    - `/mnt/dmx4t/data/recordings/20260714/bw/17/BW_20260714_170002_2366-A.jpg`
    - `/mnt/dmx4t/data/recordings/20260714/bw/17/BW_20260714_170002_2366-B.jpg`
  - 新增脚本：`tools/run_20260714_trad_yolo26_weights2.py`。
  - 输出目录：`/mnt/dmx4t/DMX_yangben/20260714/_analysis/tradition+yolo26`。
  - 处理逻辑：
    - 先用前三个完整 AB 对分别为 A/B 建立 v1 天空 mask。
    - 传统阶段放宽阈值做高召回候选，每个半幅最多保留 `160` 个传统候选。
    - 对每个传统候选中心裁 `512x512`，输入 `weights2/best.onnx` 打分。
    - 输出两套最终框图：
      - `01_yolo_filter`：YOLO drone 分数硬过滤，最终 `5` 个框。
      - `02_fusion_score`：传统分数和 YOLO 分数融合，YOLO 权重 `0.72`，最终 `16` 个框。
  - 关键输出：
    - `01_yolo_filter/BW_20260714_170002_2366_AB_final_boxes.jpg`
    - `02_fusion_score/BW_20260714_170002_2366_AB_final_boxes.jpg`
    - `01_yolo_filter/final_boxes.csv`
    - `02_fusion_score/final_boxes.csv`
    - `all_traditional_yolo_scored_candidates.csv`
  - 本次只输出最终框图和 CSV，没有输出大批 debug 中间图。
- 2026-07-20 追加实时优化版本，目标为完整 AB 全景在线核心链路压到 `6s` 内：
  - 新增/更新脚本：`tools/run_20260714_realtime_trad_yolo26.py`。
  - 最终输出目录：`/mnt/dmx4t/DMX_yangben/20260714/_analysis/tradition+yolo26/realtime_6s`。
  - 报告目录：`/mnt/dmx4t/DMX_yangben/20260714/_analysis/tradition+yolo26/realtime_report`。
  - LaTeX/PDF：
    - `realtime_optimization_report.tex`
    - `realtime_optimization_report.pdf`
  - 关键实时逻辑：
    - 天空 mask 不再每圈重复生成，在线阶段复用 v1 mask。
    - 保留 CLAHE、轻锐化、局部背景差分和 black-hat 的点目标响应，避免弱小无人机从候选池中丢失。
    - cheap 候选加入下部弱惩罚，降低树叶、建筑、地平线边缘误检权重，但不硬删除低空候选。
    - 传统特征短名单使用空间 NMS，默认 `feature_limit=120`、半径 `56`。
    - 送 YOLO 的候选改为空间覆盖优先，每个半景 `8` 个，半径 `256`，避免局部高分区域占满名额。
    - YOLO 框中心权重半径从 `160` 放宽到 `256`，解决真实目标在 512 窗口内但不在传统候选中心时被压成 0 分的问题。
    - YOLO 输出解析从逐行 Python 循环改为 NumPy 向量化，解析耗时约从 `0.23s` 降到 `0.007s`。
  - 最终计时（样例 `BW_20260714_170002_2366-A/B.jpg`）：
    - 读图与 mask：`1.398s`。
    - 传统候选合计：`3.220s`。
    - YOLOv26s 过滤合计：`1.059s`。
    - 在线核心合计：`5.676s`，满足 `6s/圈`。
    - 全分辨率诊断图写盘：`1.925s`，不计入实时核心路径。
  - 最终检测结果：
    - 最终仅输出 `1` 个框，位于 A 图真实目标附近。
    - YOLO 框：`(18434,1548,18476,1562)`。
    - drone 分数：`0.0837`，bird 分数：`0.00018`。
  - 注意事项：
    - OpenCV DNN 对当前 `weights2/best.onnx` 的 batch size 大于 1 会触发模型内部 reshape 错误，因此当前仍按单窗口循环 forward。
    - 当前结论基于一张完整 AB 全景验证，后续应在 16/17/18 连续数据上做批量回归，重点检查低空目标是否被下部弱惩罚影响。
    - 实时系统集成时不要在在线线程写全分辨率 debug 图；需要诊断图时建议异步低频保存。
- 2026-07-20 将实时优化检测链路集成进 DMX 主程序：
  - 修改文件：
    - `videothread.cpp/.h`
    - `panoramacache.cpp/.h`
    - `appconfig.cpp/.h`
    - `dmx_config.json`
    - `mainwindow.cpp/.h`
    - `radar_ui/targetrecord.h`
    - `radar_ui/targetlistwidget.cpp`
  - 实时检测调度（上一版，已由下一条“tile 候选入池 + 整圈汇总确认”修正）：
    - 新增 `PanoramaCache::snapshotFullBw()` / `snapshotFullRgb()`。
    - `VideoWorker` 不再对每个 tile 直接跑检测，而是在内存全景完成一圈后调用 `maybeDetectPanoramaCandidates()`，从无损全景缓存取全景快照跑一次完整检测。
    - 这样 `confirmMaxCandidates=8` 是每圈/每张全景的 YOLO 确认窗口上限，不会变成每个 tile 8 个窗口。
  - 检测策略：
    - `detect.cropSize` 改为 `512`，匹配离线验证的 512 窗口。
    - `detectYolo.enabled=true`，`detectYolo.mode=confirm`。
    - `detectYolo.modelPath` 改为 `/home/sht/data/低小慢数据/yolo26s/result/jiao/pt/weights2/best.onnx`。
    - 传统候选改为 cheap pool + 空间限流：
      - `featureLimit=120`
      - `featureNmsRadius=56`
      - `confirmMaxCandidates=8`
      - `confirmNmsRadius=256`
      - `centerWeightRadius=256`
    - YOLO 确认阶段按中心加权后的 drone 分数做硬过滤，并要求 drone 分数高于 bird 分数。
  - 目标框链路：
    - `DetectCandidateLocal` 增加 frame bbox、ROI bbox、YOLO bird/drone 分数字段。
    - `candidateDetected(...)` 信号增加 `roiBoxX1/Y1/X2/Y2`。
    - `TargetRecord` 增加 `hasRoiBox` 和 ROI 内框坐标。
    - 左侧 `TargetListWidget` 的疑似目标缩略图会叠加红色目标框，并显示框坐标。
    - `manifest.jsonl` 追加 `frameBox*` 与 `roiBox*` 字段，便于追溯。
  - 验证：
    - `bash build_linux.sh` 编译通过。
    - 输出程序仍为 `/home/sht/work/DMX_qt/build_linux/DMX`。
- 2026-07-20 追加实时调度修正，解决“整圈结束后才开始处理会多出一圈延迟”的问题：
  - `VideoWorker` 检测入口改为：
    - 每个 tile 写入 `PanoramaCache` 后立即调用 `collectTileCandidates()`。
    - tile 阶段只做天空 mask/传统点目标候选/纹理特征，候选仅进入内存中的本圈候选池，不写 debug 图、不跑 YOLO。
    - `PanoramaCache::state()` 判断当前圈最后一个 tile 写完后，`maybeFinalizeRoundCandidates()` 触发 `finalizeRoundCandidates()`。
    - 整圈汇总阶段对候选池做全景坐标空间 NMS，再最多取 `detectYolo.confirmMaxCandidates=8` 个 512 窗口跑 YOLOv26s `weights2` 硬过滤。
  - 性能目的：
    - 传统候选耗时分摊到采集过程中。
    - 整圈完成后只剩少量 512x512 窗口裁剪和 YOLO forward，避免整张 65536x4096 全景快照和整图 OpenCV 处理阻塞。
  - 坐标/显示：
    - tile 候选会映射到全景坐标，整圈 NMS 使用 360 度横向环绕距离。
    - 最终保存截图仍为 512x512，左侧疑似目标列表继续显示 ROI 内红框。
    - `manifest.jsonl` 继续写入 `frameBox*`、`panoBox*`、`roiBox*`、`round`、`tileIndex` 等追溯字段。
  - 当前限制：
    - `detectYolo.mode=confirm` 是主路径；`sky/hybrid` 的低频天空补检没有作为实时主链路启用。
    - 如果 UI 丢帧导致实际未覆盖完整一圈，当前仍按已处理的 `segments` 帧触发汇总，建议实测时关注日志里的 `pool/yolo_in/saved/elapsed`。
  - 验证：
    - `bash build_linux.sh` 编译通过。
    - 输出程序仍为 `/home/sht/work/DMX_qt/build_linux/DMX`。
- 2026-07-20 追加天空 mask 建模修正：
  - 用户确认不应每个 tile 各自生成天空 mask，而应根据前几张完整全景生成一张全景级 mask，后续按角度/tile 切片使用。
  - 当前实现：
    - 新增 `DetectSkyMaskPanoramaStateLocal`，BW/RGB 各维护一份全景级 sky mask 状态。
    - `collectTileCandidates()` 中每个 tile 只把当前灰度图缩到 1/4 宽高后填入对应全景样本列。
    - 每个 tile 位置累积 `detect.backgroundFrames` 次观测后，使用已有 v1 天空分割逻辑在 1/4 全景上生成 `maskSmall`。
    - 后续检测时按 `tileIndex` 从 `maskSmall` 中裁出当前 tile 的 mask slice，再 `INTER_NEAREST` 放大到当前 tile 尺寸。
    - per-tile `DetectBackgroundTile` 仍用于传统背景初始化/慢速更新，但不再生成或持有自己的 `skyMask`。
  - 实时意义：
    - 天空 mask 分割只在初始化阶段做一次全景级建模。
    - 在线主链路每个 tile 只做 mask 切片/缩放，不再重复跑天空分割。
    - 全景级 mask 能统一做连通域、小碎块和窄通道处理，减少 tile 边界不连续。
  - 验证：
    - `bash build_linux.sh` 编译通过。
    - `python3 -m json.tool dmx_config.json >/dev/null` 校验通过。

### 2026-07-30 dmx_test 直接 YOLO 与 RGB/BW 互补

- 仅在 `DMX_test.pro` 中定义 `DMX_TEST_BUILD`，新增
  `DirectYoloManager` 和常驻 Python ONNX Runtime GPU worker；正式
  `DMX.pro` 不包含 manager，正式二进制未更新。
- 模型：
  `/home/sht/data/低小慢数据/yolo26s/result/jiao/pt/weights2/best.onnx`。
- 项目内 `.venv` 已安装 `onnxruntime-gpu[cuda,cudnn]==1.26.0` 和
  `opencv-python-headless==4.13.0.92`，CUDA provider 已在 RTX 5060 Ti
  上验证。
- 新主链路不依赖传统召回：
  - RGB/BW 每帧各跑一次全图 640 YOLO。
  - mask 内使用 640x640 原分辨率窗口、512 步长、15% 天空覆盖率做
    小目标补检。
  - 正常每帧预算 240ms，排队超过 450ms 时局部预算降为 120ms；
    未完成窗口按 `(stream, tileIndex)` 轮转。
  - 单流上报阈值 0.12，跨流低阈值 0.05，bird 比值为 1.10。
  - RGB/BW 通过已经补偿 180 度的全景坐标异步互证，时间差
    2.5-5.5s，横向 384px、纵向 256px。
  - 目标尺寸只检查 ONNX 框是否合法，不参与核心置信度或融合权重。
- BW 传统算法仍独立运行，但在测试版中：
  - 线性降采样 2 倍，降低连通域和纹理特征耗时。
  - 传统候选继续保存到原 manifest，并记录 `DETECT_AUX ui=0`。
  - 不再直接进入雷达，避免未确认传统误报污染最终结果。
- 20260723 固定回放预载确定的 RGB/BW 几何 mask，避免第三圈在线建模
  各阻塞约 7.5s：
  `/mnt/dmx4t/data/dmx_test/analysis/sky_mask_runtime_baseline_20260723`。
- 关键验证：
  - CUDA 640 推理 P50 4.27ms、P95 4.80ms。
  - 4096 共享盘图像解码、旋转镜像约 85-115ms。
  - 最密集分段有 42 个窗口，240ms 内约处理 27 个，后续轮转补齐。
  - 已知帧 `BW_20260723_153353_984.jpg` 检出 `(191,1475)`，原候选为
    `(191,1476)`。
  - 35 秒端到端日志
    `/mnt/dmx4t/data/dmx_test/logs/2026-07-30/log_18-46-52.txt`
    中 RGB/BW 均为 2fps，无 `SEQ gap`，传统输出均为 `ui=0`。
  - `build_test.sh` 编译通过。
  - 正式目标在 `/tmp/dmx_formal_compile` 无 `DMX_TEST_BUILD` 全量编译
    通过，未覆盖 `build_linux/DMX`。
- 当前阈值是第一版外场测试参数，下一轮应按人工确认的真阳性和误报样本
  调整 0.12/0.05，而不是先改目标尺寸规则。

### 2026-07-30 dmx_test 处理速率复核与疑似目标事件合并

- 最近一次完整运行日志：
  `/mnt/dmx4t/data/dmx_test/logs/2026-07-30/log_18-51-49.txt`。
- 速率结论：
  - RGB/BW 输入各为 2fps，即每流每 500ms 一帧、每 8 秒形成一圈。
  - 图像处理 `FPS` 日志共 1751 个采样，平均处理耗时 298ms，
    P50 280ms、P95 409ms，单次最大值 594ms。
  - 直接 YOLO 共 253 个性能采样：总耗时平均 140.3ms、P50 128.8ms、
    P95 238.1ms、最大 241.5ms；排队最大 218.5ms，未触发 450ms
    追赶阈值。
  - 本次处理 3502 帧，无 `SEQ gap`、无丢帧或失败。因此识别处理能够
    跟上输入和 8 秒全景刷新周期；偶发单帧超过 500ms 没有形成持续积压。
- 新目标雷达列表在 `DMX_TEST_BUILD` 下增加事件级去重，不属于轨迹预测：
  - 同一源帧的多个独立 YOLO 框绝不互相合并，允许多个目标同时出现。
  - 同源跨圈候选按 8 秒周期、时间残差、全景角度、高度、目标框面积/
    长宽比和 ROI 感知哈希联合关联；目标尺寸只用于形态一致性检查，
    不作为识别置信度核心权重。
  - RGB/BW 支持两种合并：同一采集帧对且时间差不超过 1.2 秒，或相邻
    半圈 2.5-6.5 秒；同时检查角度、高度和框形态。
  - 合并后沿用稳定目标编号并累加命中次数，来源显示为 `彩色+黑白`。
  - 目标关联记忆保留 120 秒，但列表和雷达只显示最近
    `2 * detectRadarHoldMs` 的事件，避免历史点长期占用界面。
  - 每次判定写入 `TARGET_ASSOC`，记录 `new/merge`、模式、时间差、
    角度差、高度差、形态比和哈希距离，便于下一轮审计。
- 使用本次 `direct_yolo_manifest.jsonl` 的 47 条检测做离线冒烟回放：
  33 个新事件、14 次合并；连续 9 圈出现的同一无人机归入一个编号，
  同一帧的两只鸟保持两个编号，已知 RGB/BW 对合并为 `RGB+BW`。
- `build_linux_test/DMX_test` 编译通过；实际样本关联冒烟测试通过。
  正式版在 `/tmp/dmx_formal_compile` 编译通过，事件合并逻辑由
  `DMX_TEST_BUILD` 隔离，未覆盖正式 `build_linux/DMX`。

### 2026-07-14

验证内容：

- 新目标雷达 ROI 列表。
- 图像查看弹窗。
- 目标图层显示策略。
- 正北显示校准。
- 外圈角度刻度。

命令：

```bash
bash build_linux.sh
```

结果：

- 编译通过。
- 生成 `/home/sht/work/DMX_qt/build_linux/DMX`。

遗留：

- 还需要在真实外场数据和实际屏幕分辨率下做 UI 操作验证。
- 还需要用真实检测数据验证 YOLO 阈值和 RGB/BW 互补策略。

## 9. 下一步推荐推进顺序

1. 补齐 ROI 点击查看“对应全图”的数据链路。
2. 用真实外场数据验证新目标雷达页面，包括正北校准、刻度、目标点、7 个 ROI 替换。
3. 对 `direct_yolo_manifest.jsonl` 做人工真阳性/误报标注，输出
   YOLOv26s 0.12/0.05 阈值评估表。
4. 用户确认 dmx_test 效果后，再把直接 YOLO 和 RGB/BW 互补链路移植到
   正式 DMX；移植前不要改正式启动脚本。
5. 用完整回放检查彩色/融合背景的 RGB/BW 配准和长时间切换流畅度。
6. 复查转台调速时主界面卡顿问题，必要时异步化串口命令流程。
7. 更新软件使用说明书和目标显示策略文档，把实测结论写进去。

## 10. 接手注意事项

- 当前仓库不是干净状态，已有大量修改和未跟踪文件，不要执行破坏性 git 操作。
- 目标识别逻辑的基础要求不能丢：
  - 先做图像预处理。
  - 前三张建立背景。
  - 先分割地平线/天空 mask。
  - dmx_test 的直接 YOLO 必须独立于传统召回。
  - 传统分支作为辅助诊断，不应把未确认结果直接送入雷达。
  - RGB/BW 按全景角度异步互补，不能按相同帧号直接配对。
  - 目标尺寸不能作为核心置信度或过滤权重。
- 用户明确要求正北校准不保存，每次程序关闭后恢复原样。
- ROI 保存必须保证目标在图像中央。
- 新目标雷达显示层是独立图层，不应和背景投影逻辑混在一起。
- 算法原始方位角和显示校准角必须分开。
- 每次做完功能后更新本交接文档。

### 2.20 2026-08-26 weights5部署与取消电线杆运行时抑制

- `dmx_test` 一键启动模型更新为
  `/home/sht/data/低小慢数据/yolo26s/result/jiao/pt/weights5/weights/best.onnx`。
- ONNX SHA-256 为
  `e68c15744109f1ec52952001994dc59bddecbfe6df72c9a2932911fbfe8b094f`。
- `run_dmx_test.sh` 增加模型可读性预检；weights5缺失时直接终止，不允许
  静默使用旧权重或无模型运行。
- 启动脚本显式设置 `DMX_STATIC_CLUTTER=0`，不再向worker传递
  `--static-clutter`，旧电线杆位置/外观/竖直支撑规则不参与目标过滤。
- `DMX_TRADITIONAL_DIAGNOSTIC_ONLY=1` 保持不变，传统候选只用于诊断；正常
  检测候选由weights5直接YOLO产生，RGB/BW互证和事件融合逻辑保持不变。
- 独立worker加载和单帧推理冒烟测试通过：CUDA、`1x3x512x512`、三类、
  end-to-end解码，启动回报 `staticClutter=false`。
