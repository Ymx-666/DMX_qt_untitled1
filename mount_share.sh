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

# 优先尝试 SMB 协议版本顺序: v3.0 -> v2.1 -> v2.0 -> v1.0
for VERS in 3.0 2.1 2.0 1.0; do
    echo "尝试挂载 //$DEVICE_IP/data -> $MOUNT_POINT (SMB vers=$VERS) ..."
    if sudo mount -t cifs "//$DEVICE_IP/data" "$MOUNT_POINT" \
        -o "guest,vers=$VERS,uid=$(id -u),gid=$(id -g),file_mode=0644,dir_mode=0755" 2>/dev/null; then
        echo "挂载成功（SMB vers=$VERS）。前 10 项内容："
        ls "$MOUNT_POINT" 2>&1 | head -10
        exit 0
    fi
done

echo ""
echo "自动挂载失败。可能原因："
echo "  1. 共享需要账号密码 → sudo mount -t cifs //$DEVICE_IP/data $MOUNT_POINT -o username=USER"
echo "  2. 设备禁用了所有 SMB 协议版本"
echo "  3. 防火墙阻断"
echo ""
echo "诊断: smbclient -L //$DEVICE_IP -N"
exit 1
