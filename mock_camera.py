import socket
import time
import os

# ================= 配置区 =================
TARGET_IP = "127.0.0.1"   
PORT_PATH = 8001          

# 本机实际存在的测试图片目录
LOCAL_LINUX_DIR = "/home/ymx/untitled1/data" 
# ==========================================

def simulate_slow_camera():
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    
    try:
        files = sorted([f for f in os.listdir(LOCAL_LINUX_DIR) if f.lower().endswith(('.jpg', '.png'))])
    except Exception as e:
        print(f"❌ 读取本地目录失败: {e}")
        return

    if not files:
        print("❌ 目录中没有找到图片！")
        return

    print(f"✅ 成功加载 {len(files)} 张素材。")
    print("🚀 开始慢速推流，当前速率：1帧 / 秒 (1 FPS)...")

    mid = len(files) // 2
    color_files = files[:mid]
    bw_files = files[mid:]
    
    color_idx = 0
    bw_idx = 0

    while True:
        # ==========================================
        # 1. 模拟彩色相机 (RGB) 推流
        # ==========================================
        if color_idx < len(color_files):
            win_color_path = f"\\\\192.168.4.1\\data\\raw\\20260130\\rgb\\{color_files[color_idx]}"
            udp_rgb = f"RGB; {win_color_path}"
            
            sock.sendto(udp_rgb.encode('utf-8'), (TARGET_IP, PORT_PATH))
            print(f"🔴 [1 FPS 慢速推流] 彩色: {udp_rgb}")
            color_idx += 1

        # 给 Qt 一点处理和渲染的时间（微小间隔防止粘包）
        time.sleep(0.05)

        # ==========================================
        # 2. 模拟黑白相机 (BW) 推流
        # ==========================================
        if bw_idx < len(bw_files):
            win_bw_path = f"\\\\192.168.4.1\\data\\raw\\20260130\\bw\\{bw_files[bw_idx]}"
            udp_bw = f"BW; {win_bw_path}"
            
            sock.sendto(udp_bw.encode('utf-8'), (TARGET_IP, PORT_PATH))
            print(f"⚪ [1 FPS 慢速推流] 黑白: {udp_bw}")
            bw_idx += 1

        # ==========================================
        # 【核心修改点】：雷打不动的 1 秒间隔
        # ==========================================
        time.sleep(1.0) 
        
        # 循环播放
        if color_idx >= len(color_files) and bw_idx >= len(bw_files):
            print("🔄 扫描完毕，重新开始...")
            color_idx = 0
            bw_idx = 0

if __name__ == "__main__":
    simulate_slow_camera()
