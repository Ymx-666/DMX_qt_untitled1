import socket
import struct
import time
import os
import cv2
import numpy as np
import glob
import threading

# ================= 统一配置区 =================
# [控制信令平面]
LISTEN_IP = "127.0.0.1"     # 模拟器本机监听 IP
LISTEN_PORT = 5001          # 监听 Qt 发来的指令
REPLY_PORT = 5002           # 回传状态给 Qt 的端口

# [数据传输平面]
TARGET_IP = "127.0.0.1"     # Qt 接收端的 IP
PORT_PATH = 8001            # 文件路径信令端口
PORT_BW = 8002              # 黑白二进制裸流端口
PORT_COLOR = 8003           # 彩色二进制裸流端口
CHUNK_SIZE = 8000           # MTU 防丢包切片大小

# [挂载与路径]
LINUX_MOUNT_DIR = "/mnt/radar_data/test/20161018/1"
FAKE_WIN_DIR = r"\\192.168.4.1\data\test\output_slices"
# ==========================================

# ================= 核心工具类 =================
class RadarStreamSlicer:
    def __init__(self, target_h=4096, target_w=2048):
        self.target_h = target_h
        self.target_w = target_w
        self.pixel_buffer = None

    def push_and_slice(self, raw_image):
        if raw_image is None or raw_image.size == 0: return []
        h, w = raw_image.shape[:2]
        
        # 竖图右旋对齐高度
        if h == 6510 and w == 4096:
            raw_image = cv2.rotate(raw_image, cv2.ROTATE_90_CLOCKWISE)

        # 压入缓存区
        if self.pixel_buffer is None:
            self.pixel_buffer = raw_image
        else:
            self.pixel_buffer = np.concatenate((self.pixel_buffer, raw_image), axis=1)
            
        valid_frames = []
        # 按列切割
        while self.pixel_buffer.shape[1] >= self.target_w:
            frame = self.pixel_buffer[:, :self.target_w]
            valid_frames.append(frame.copy())
            self.pixel_buffer = self.pixel_buffer[:, self.target_w:]
            
        return valid_frames

def send_udp_blocks(sock, img_data, total_size, timestamp, image_index, img_type, target_port):
    """底层 UDP 封包与分片发送函数"""
    offset = 0
    block_index = 0
    while offset < total_size:
        current_block_size = min(CHUNK_SIZE, total_size - offset)
        chunk_data = img_data[offset : offset + current_block_size]
        
        # C++ 结构体字节对齐: <H H I I I H H
        header = struct.pack('<H H I I I H H', 
                             0xFFFF, img_type, timestamp, image_index, total_size, block_index, current_block_size)
        
        packet = header + chunk_data
        sock.sendto(packet, (TARGET_IP, target_port))
        
        offset += current_block_size
        block_index += 1
        time.sleep(0.001) # 微小延时防网卡缓冲区溢出


# ================= 线程 1：信令控制平面 =================
def thread_control_plane():
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind((LISTEN_IP, LISTEN_PORT))
    
    print(f"🎧 [信令线程] 已启动！监听控制指令 (UDP: {LISTEN_PORT})...")

    while True:
        try:
            data, addr = sock.recvfrom(1024)
            cmd = data.decode('utf-8').strip()
            client_ip = addr[0] 
            
            print(f"📥 [收到指令] <- {cmd} (来自 {client_ip}:{addr[1]})")

            # 构造返回值并打回
            if cmd.endswith(";"):
                reply_str = f"{cmd}OK;"
            else:
                reply_str = f"{cmd};OK;"
            
            sock.sendto(reply_str.encode('utf-8'), (client_ip, REPLY_PORT))
            print(f"📤 [回传状态] -> {reply_str} (发往 {client_ip}:{REPLY_PORT})")

        except Exception as e:
            print(f"❌ [信令线程] 发生错误: {e}")


# ================= 线程 2：数据流平面 =================
def thread_data_pipeline():
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    slicer = RadarStreamSlicer()
    
    raw_files = sorted(glob.glob(os.path.join(LINUX_MOUNT_DIR, "*.[pj][pn]*")))
    if not raw_files:
        print(f"❌ [数据线程] 挂载目录中没有找到图片: {LINUX_MOUNT_DIR}")
        return
        
    print(f"🚀 [数据线程] 已启动！3 端口全双工雷达流准备就绪...")
    
    image_index = 0 

    while True:
        for raw_path in raw_files:
            img = cv2.imread(raw_path, cv2.IMREAD_GRAYSCALE)
            if img is None: continue

            frames = slicer.push_and_slice(img)
            
            for frame in frames:
                success, buffer = cv2.imencode('.jpg', frame, [cv2.IMWRITE_JPEG_QUALITY, 85])
                if not success: continue
                
                img_data = buffer.tobytes()
                total_size = len(img_data)
                timestamp = int(time.time())

                # 动作 1：8001 端口路径推流
                filename = f"frame_{image_index:05d}.jpg"
                win_path = f"{FAKE_WIN_DIR}\\{filename}"
                sock.sendto(f"RGB; {win_path}".encode('utf-8'), (TARGET_IP, PORT_PATH))
                sock.sendto(f"BW; {win_path}".encode('utf-8'), (TARGET_IP, PORT_PATH))

                # 动作 2：8003 端口彩色裸流
                send_udp_blocks(sock, img_data, total_size, timestamp, image_index, 1, PORT_COLOR)

                # 动作 3：8002 端口黑白裸流
                send_udp_blocks(sock, img_data, total_size, timestamp, image_index, 2, PORT_BW)

                # 打印进度
                current_lap = (image_index // 32) + 1  
                slice_pos = (image_index % 32) + 1     
                bar = "█" * slice_pos + "░" * (32 - slice_pos)
                print(f"🔄 [雷达推流] 第 {current_lap:03d} 圈 |{bar}| {slice_pos:02d}/32 | 全局帧 {image_index}")

                image_index += 1
                time.sleep(0.15) 

        print("\n🔁 当前挂载数据已触底，像素缓存池无缝衔接，启动下一轮旋转...\n")


# ================= 主程序入口 =================
if __name__ == "__main__":
    print("==================================================")
    print("🤖 全真雷达硬件节点模拟器 初始化中...")
    print("==================================================\n")

    # 创建为守护线程（daemon=True），确保主程序收到 Ctrl+C 时子线程会跟随安全退出
    t_control = threading.Thread(target=thread_control_plane, daemon=True)
    t_data = threading.Thread(target=thread_data_pipeline, daemon=True)

    # 启动双线程并行执行
    t_control.start()
    t_data.start()

    # 主线程进入休眠，维持程序运行状态，捕捉 Ctrl+C 中断
    try:
        while True:
            time.sleep(1)
    except KeyboardInterrupt:
        print("\n🛑 收到 KeyboardInterrupt，模拟器安全停止。")
