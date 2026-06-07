# 录制后处理拼接脚本

脚本：`tools/stitch_recording_panoramas.py`

用途：把一次 RawRecorder 录制目录中的原始帧按每 16 帧拼成一张无损全景图。

依赖：

```bash
sudo apt install -y python3-opencv python3-numpy
```

输入目录必须是包含这些内容的 `REC2_...` 目录：

```text
REC2_20260607_120000/
  index.jsonl
  rgb/
  bw/
```

Linux 示例：

```bash
cd ~/work/DMX_qt
python3 tools/stitch_recording_panoramas.py /data/dmx/recordings/REC2_20260607_120000
```

默认输出：

```text
REC2_20260607_120000/panoramas/
  rgb_pano_0001.tiff
  rgb_pano_0001_preview.jpg
  bw_pano_0001.tiff
  bw_pano_0001_preview.jpg
  manifest.json
```

默认规则：

- 每 16 帧拼一张全景图。
- 每帧要求 4096x4096，输出全景图为 65536x4096。
- 输出 TIFF-LZW，无损压缩。
- 默认执行与实时全景相同的方向处理：旋转 90 度并镜像。
- 不足 16 帧的尾帧不会拼接，会写入 `manifest.json` 的 `droppedTailFrames`。

常用参数：

```bash
# 输出到指定目录
python3 tools/stitch_recording_panoramas.py REC2_xxx --out-dir /data/panos

# 不做旋转/镜像，直接横向拼原始帧
python3 tools/stitch_recording_panoramas.py REC2_xxx --no-orient

# 每组反向拼接，用于转台方向相反时离线修正
python3 tools/stitch_recording_panoramas.py REC2_xxx --reverse
```
