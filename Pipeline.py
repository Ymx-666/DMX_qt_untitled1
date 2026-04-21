import socket
import struct
import time
import os
import cv2
import numpy as np
import glob

# ================= 配置区 =================
TARGET_IP = "127.0.0.1"
PORT_PATH = 8001    # 文件路径信令端口
PORT_BW = 8002      # 黑白二进制裸流端口
PORT_COLOR = 8003   # 彩色二进制裸流端口
CHUNK_SIZE = 8000   # MTU 防丢包切片大小

# Linux 挂载目录
LINUX_MOUNT_DIR = "/mnt/radar_data/test/20161018/1"

# 发给 8001 端口的虚拟/挂载前缀路径 (由于是裸流推图，并不实际存盘，这里发虚拟路径骗过 UI 协议)
FAKE_WIN_DIR = r"\\192.168.4.1\data\test\output_slices"
# ==========================================

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
        # 含义: 0xFFFF, 图像类型(1彩/2黑白), 时间戳, 图像序号, 总大小, 块序号, 块大小
        header = struct.pack('<H H I I I H H', 
                             0xFFFF, img_type, timestamp, image_index, total_size, block_index, current_block_size)
        
        packet = header + chunk_data
        sock.sendto(packet, (TARGET_IP, target_port))
        
        offset += current_block_size
        block_index += 1
        time.sleep(0.001) # 微小延时防网卡缓冲区溢出


def run_infinite_pipeline():
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    slicer = RadarStreamSlicer()
    
    raw_files = sorted(glob.glob(os.path.join(LINUX_MOUNT_DIR, "*.[pj][pn]*")))
    if not raw_files:
        print(f"❌ 挂载目录中没有找到图片: {LINUX_MOUNT_DIR}")
        return
        
    print(f"✅ 准备就绪。启动 3 端口全双工雷达模拟流 (无限循环)...")
    
    # 全局帧序号
    image_index = 0 

    try:
        # 第一层循环：无限循环播放
        while True:
            # 第二层循环：遍历本地切片数据
            for raw_path in raw_files:
                img = cv2.imread(raw_path, cv2.IMREAD_GRAYSCALE)
                if img is None: continue

                # 拿到切片好的 2048x4096 标准帧
                frames = slicer.push_and_slice(img)
                
                for frame in frames:
                    # 1. 内存极速转码 (压缩成 JPG 减小网络压力)
                    success, buffer = cv2.imencode('.jpg', frame, [cv2.IMWRITE_JPEG_QUALITY, 85])
                    if not success: continue
                    
                    img_data = buffer.tobytes()
                    total_size = len(img_data)
                    timestamp = int(time.time())

                    # ==================================================
                    # 动作 1：向 8001 端口发送路径信令 (兼容文件共享模式协议)
                    # ==================================================
                    filename = f"frame_{image_index:05d}.jpg"
                    win_path = f"{FAKE_WIN_DIR}\\{filename}"
                    sock.sendto(f"RGB; {win_path}".encode('utf-8'), (TARGET_IP, PORT_PATH))
                    sock.sendto(f"BW; {win_path}".encode('utf-8'), (TARGET_IP, PORT_PATH))

                    # ==================================================
                    # 动作 2：向 8003 端口喷射“彩色”裸流 (Type = 1)
                    # ==================================================
                    send_udp_blocks(sock, img_data, total_size, timestamp, image_index, 1, PORT_COLOR)

                    # ==================================================
                    # 动作 3：向 8002 端口喷射“黑白”裸流 (Type = 2)
                    # ==================================================
                    send_udp_blocks(sock, img_data, total_size, timestamp, image_index, 2, PORT_BW)

                    # --- 打印进度大屏 ---
                    current_lap = (image_index // 32) + 1  # 当前是第几圈
                    slice_pos = (image_index % 32) + 1     # 当前在全景图中的第几块 (1-32)
                    
                    bar = "█" * slice_pos + "░" * (32 - slice_pos)
                    print(f"🔄 雷达第 {current_lap:03d} 圈 |{bar}| {slice_pos:02d}/32 | 全局帧 {image_index}")

                    image_index += 1
                    
                    # 出帧率控制 (当前: 0.15秒出一帧，即约 5 秒扫完一整张全景图)
                    # 留出时间给 Qt UI 解码渲染，若 UI 卡顿可将此数值调大
                    time.sleep(0.15) 

            print("\n🔁 当前挂载数据已触底，像素缓存池无缝衔接，启动下一轮旋转...\n")
            
    except KeyboardInterrupt:
        print("\n🛑 收到中断信号，雷达推流已停止。")

if __name__ == "__main__":
    run_infinite_pipeline()
