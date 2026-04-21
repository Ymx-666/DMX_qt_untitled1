# 该代码作用：监听 UDP 8001 端口，并将接收到的所有数据及其来源实时保存到本地文件 udp_log.txt 中
import socket
from datetime import datetime

# 配置信息
LOG_FILE = "udp_log.txt"
LISTEN_IP = "0.0.0.0"
LISTEN_PORT = 8001

# 创建 UDP 套接字
sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
sock.bind((LISTEN_IP, LISTEN_PORT))

print(f"正在 {LISTEN_PORT} 端口监听并存储数据... (按 Ctrl+C 停止)")

try:
    # 使用 'a' 模式打开文件（append，追加模式）
    # encoding='utf-8' 确保中文内容不乱码，errors='ignore' 防止非文本数据导致崩溃
    with open(LOG_FILE, "a", encoding="utf-8") as f:
        while True:
            # 接收数据
            data, addr = sock.recvfrom(4096)  # 缓冲区增加到 4096 字节
            
            # 获取当前时间
            timestamp = datetime.now().strftime("%Y-%m-%d %H:%M:%S")
            
            # 尝试解码数据
            content = data.decode(errors='ignore').strip()
            
            # 格式化日志内容
            log_entry = f"[{timestamp}] 来自 {addr} 的数据: {content}\n"
            
            # 打印到屏幕并写入文件
            print(log_entry, end="")
            f.write(log_entry)
            
            # 实时刷新缓冲区，确保程序异常崩溃时数据已写入磁盘
            f.flush()

except KeyboardInterrupt:
    print("\n监听已停止，数据已保存。")
finally:
    sock.close()
