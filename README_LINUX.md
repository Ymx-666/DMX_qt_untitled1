# Linux 构建与运行说明

## 1. 依赖安装

```bash
sudo apt install -y \
    qtbase5-dev qtbase5-dev-tools qttools5-dev qttools5-dev-tools \
    qtmultimedia5-dev libqt5multimediawidgets5 \
    libqt5serialport5-dev libqt5svg5-dev \
    libavcodec-dev libavformat-dev libavutil-dev \
    libswscale-dev libavdevice-dev libswresample-dev \
    libopencv-dev pkg-config cifs-utils
```

## 2. 编译

在 Qt 源码目录（含 `untitled1.pro` 那个目录）执行：

```bash
./build_linux.sh
```

产出：`build_linux/untitled1`

## 3. 挂载设备共享盘（首次或重启后）

```bash
./mount_share.sh                          # 默认 192.168.4.1 -> /mnt/dmx_share
./mount_share.sh 192.168.1.100 /mnt/foo   # 自定义 IP 和挂载点
```

## 4. 运行

```bash
./build_linux/untitled1
```

## 5. 路径配置

所有写盘路径都可通过环境变量覆盖：

| 用途 | 默认路径 | 环境变量 |
|---|---|---|
| 保存全图 | `~/dmx_data/saves` | `DMX_SAVE_ROOT` |
| 录制 raw | `~/dmx_data/recordings` | `DMX_REC_ROOT` |
| 日志文件 | `~/dmx_data/logs/<YYYY-MM-DD>/log_<HH-MM-SS>.txt` | `DMX_LOG_ROOT` |
| 共享盘挂载点 | `/mnt/dmx_share` | `DMX_SHARE_MOUNT` |

日志按当天日期建二级子目录，每次启动程序新建一个 `log_HH-MM-SS.txt` 文件，每条日志立即 flush 落盘，崩溃时也能保留全部历史。

示例：

```bash
DMX_SAVE_ROOT=/data/saves \
DMX_REC_ROOT=/data/recordings \
DMX_SHARE_MOUNT=/mnt/myshare \
    ./build_linux/untitled1
```

启动后 UI 日志框最上方会显示当前生效的路径（淡黄色 `BUILD` 行），便于确认配置正确。

## 6. 跨平台说明

源码内通过 `#ifdef Q_OS_WIN` / `#else` 区分 Windows 和 Linux：

- Windows 上 `\\192.168.4.1\data\*` UNC 路径直接访问设备共享盘
- Linux 上读取 `/mnt/dmx_share/*`（需事先 SMB 挂载）

如果不想在 Linux 上挂载共享盘（例如仅做离线测试），可让 `DMX_SHARE_MOUNT` 指向本地一个目录，把测试数据放进去。
