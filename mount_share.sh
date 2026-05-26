#!/bin/bash
# 挂载 DMX 设备的 SMB 共享盘到本地挂载点。
# Usage: ./mount_share.sh [device_ip] [mount_point]

DEVICE_IP="${1:-192.168.4.1}"
MOUNT_POINT="${2:-/mnt/dmx_share}"

if ! command -v mount.cifs &>/dev/null; then
    echo "cifs-utils 未安装。请先执行："
    echo "  sudo apt install -y cifs-utils"
    exit 1
fi

if ! ping -c 1 -W 2 "$DEVICE_IP" &>/dev/null; then
    echo "无法 ping 通设备 $DEVICE_IP，检查网络后再试。"
    exit 1
fi

sudo mkdir -p "$MOUNT_POINT"

echo "挂载 //$DEVICE_IP/data -> $MOUNT_POINT ..."
if sudo mount -t cifs "//$DEVICE_IP/data" "$MOUNT_POINT" \
    -o "guest,vers=1.0,uid=$(id -u),gid=$(id -g),file_mode=0644,dir_mode=0755" 2>/dev/null; then
    echo "挂载成功。前 10 项内容："
    ls "$MOUNT_POINT" 2>&1 | head -10
    exit 0
fi

echo "guest + vers=1.0 失败，尝试 vers=2.0 ..."
if sudo mount -t cifs "//$DEVICE_IP/data" "$MOUNT_POINT" \
    -o "guest,vers=2.0,uid=$(id -u),gid=$(id -g),file_mode=0644,dir_mode=0755" 2>/dev/null; then
    echo "挂载成功（vers=2.0）。"
    ls "$MOUNT_POINT" 2>&1 | head -10
    exit 0
fi

echo ""
echo "自动挂载失败。可能原因："
echo "  1. 共享需要账号密码 → sudo mount -t cifs //$DEVICE_IP/data $MOUNT_POINT -o username=USER"
echo "  2. SMB 版本不匹配 → 试试 vers=3.0"
echo "  3. 设备未开启共享"
exit 1
