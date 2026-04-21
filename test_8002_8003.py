import socket
import struct
import time
import os

# ================= 配置区 =================
TARGET_IP = "127.0.0.1"
PORT_BW = 8002      # 协议规定的黑白裸流端口
PORT_COLOR = 8003   # 协议规定的彩色裸流端口
CHUNK_SIZE = 8000   # 协议规定的单包最大载荷 (防 UDP 丢包)

# 本机实际存在的测试图片目录 (借用这些图片转成二进制流发送)
LOCAL_LINUX_DIR = "/home/ymx/untitled1/data" 
# ==========================================

def send_binary_stream():
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
    print("🚀 开始模拟 8002/8003 端口的高并发二进制裸流...")

    mid = len(files) // 2
    color_files = files[:mid]
    bw_files = files[mid:]
    
    image_index = 0 # 模拟全景图转动的全局序号

    while True:
        # 取出一张图发给彩色(8003)，取另一张图发给黑白(8002)
        color_img_path = os.path.join(LOCAL_LINUX_DIR, color_files[image_index % len(color_files)])
        bw_img_path = os.path.join(LOCAL_LINUX_DIR, bw_files[image_index % len(bw_files)])

        # ==========================================
        # 核心逻辑：读取文件并切片发包
        # ==========================================
        send_single_image(sock, color_img_path, PORT_COLOR, image_index, 1) # 1代表彩色图
        send_single_image(sock, bw_img_path, PORT_BW, image_index, 2)       # 2代表黑白图
        
        image_index += 1
        time.sleep(0.1) # 模拟相机出图的帧率间隔

def send_single_image(sock, file_path, target_port, img_index, img_type):
    # 1. 以二进制(rb)模式读取整张 JPG
    with open(file_path, 'rb') as f:
        img_data = f.read()
    
    total_size = len(img_data)
    timestamp = int(time.time())
    offset = 0
    block_index = 0
    
    # 2. 把这张大图切碎成多个 UDP 包发送
    while offset < total_size:
        # 计算当前块的大小 (最后一块可能不足 8000)
        current_block_size = min(CHUNK_SIZE, total_size - offset)
        chunk_data = img_data[offset : offset + current_block_size]
        
        # 3. 严格按照 C++ 端的结构体构造 20 字节的二进制包头
        # 格式: <H H I I I H H (小端序)
        # 对应: 0xFFFF, 图像类型, 时间戳, 图像序号, 总大小, 块序号, 块大小
        header = struct.pack('<H H I I I H H', 
                             0xFFFF, 
                             img_type, 
                             timestamp, 
                             img_index, 
                             total_size, 
                             block_index, 
                             current_block_size)
        
        # 4. 拼接报头和图像真实数据，发往指定端口
        packet = header + chunk_data
        sock.sendto(packet, (TARGET_IP, target_port))
        
        offset += current_block_size
        block_index += 1
        
        # 微小延时防止网卡缓冲溢出导致 UDP 丢包
        time.sleep(0.001) 
    
    port_name = "彩色(8003)" if target_port == 8003 else "黑白(8002)"
    print(f"📦 [{port_name}] 已发送图序号 {img_index} | 大小: {total_size} 字节 | 分片: {block_index} 个")

if __name__ == "__main__":
    send_binary_stream()
