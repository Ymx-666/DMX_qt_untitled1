# 全景图缓存机制（重构版）

## 类图

```mermaid
classDiagram
    class MainWindow {
        -PanoramaWidget* panoramaView
        -PanoramaWidget* thermalPanoramaView
        -AIVideoWidget* colorRoiView
        -AIVideoWidget* thermalRoiView
        -QSharedPointer~PanoramaCache~ m_panoCache
        -QTimer* m_renderTimer
        -QAtomicInteger~int~ m_renderBusy
        +onRenderTick()
        +onPanoramaClicked(angle)
        +onSaveFullPanoramaClicked()
    }

    class VideoThread {
        -int m_type
        -VideoWorker* m_worker
        -QSharedPointer~PanoramaCache~ m_cache
    }

    class VideoWorker {
        -int m_type
        -QSharedPointer~PanoramaCache~ m_cache
        +processPendingDatagrams()
        +handlePathInternal(...)
    }

    class PanoramaCache {
        +pushRgbFrame(frameRgb32)
        +pushBwFrame(frameBw8)
        +snapshotThumbRgb()
        +snapshotThumbBw()
        +extractFullRgbSliceByAngle(angle)
        +extractFullBwSliceByAngle(angle, allow180Fallback)
        +state(BlockId)
        +setFreezeWrites(bool)
        +withFullRgbLocked(func)
        +withFullBwLocked(func)
    }

    MainWindow --> PanoramaCache
    VideoThread --> PanoramaCache
    VideoWorker --> PanoramaCache
    VideoThread --> VideoWorker
```

## 内存布局图

```mermaid
flowchart TB
    subgraph FULL["无损全景 (循环覆盖)"]
        FR["Full RGB: QImage 65536x4096 RGB32"]
        FB["Full BW: QImage 65536x4096 Indexed8"]
    end

    subgraph THUMB["缩略全景 (循环覆盖)"]
        TR["Thumb RGB: QImage 8192x240 RGB32"]
        TBW["Thumb BW: QImage 8192x240 Indexed8"]
    end

    W["写指针 writeIndex (0..segments-1)\n第一帧 -> 0号块(0°基准)\n写满 segments 后回到 0 覆盖"]
    R["读取：UI 定时器仅读取缩略块\nROI/保存读取无损块"]

    W --> FR
    W --> FB
    W --> TR
    W --> TBW
    R --> TR
    R --> TBW
    R -.-> FR
    R -.-> FB
```

## 接口说明

- 写入（子线程）
  - `pushRgbFrame(QImage frameRgb32)`：以第一帧为 0°，后续按到达顺序写入，写满回绕覆盖；同时更新缩略图块
  - `pushBwFrame(QImage frameBw8)`：同上（Indexed8 灰度）
- 读取（主线程）
  - `snapshotThumbRgb()/snapshotThumbBw()`：返回缩略图快照（用于 UI 刷新）
  - `extractFullRgbSliceByAngle(angle)`：按角度在无损全景中取一个 slice（ROI 来源）
  - `extractFullBwSliceByAngle(angle, allow180Fallback)`：BW 取 slice（可选 180° 兜底）
- 状态
  - `state(BlockId)`：写指针、有效帧数、是否写满一圈、丢帧计数等
- 保存
  - `saveFullPanoramaBmp(rgbPath, bwPath, err)`：分段读锁写盘，写入期间不停止转台与UI刷新（不同分段可能存在微小时间差）
