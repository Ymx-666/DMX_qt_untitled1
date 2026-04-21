import socket

# 绑定本机 IP 监听 5001 端口
LISTEN_IP = "127.0.0.1"
LISTEN_PORT = 5001
REPLY_PORT = 5002

def simulate_hardware_device():
    # 创建 UDP Socket
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind((LISTEN_IP, LISTEN_PORT))
    
    print("==================================================")
    print(f"🤖 [硬件模拟器] 已启动！")
    print(f"🎧 正在监听控制指令 (UDP 端口: {LISTEN_PORT})...")
    print("==================================================\n")

    while True:
        try:
            # 1. 接收 Qt 发来的指令
            data, addr = sock.recvfrom(1024)
            cmd = data.decode('utf-8').strip()
            
            # Qt 发包所在的 IP (用于回传)
            client_ip = addr[0] 
            print(f"📥 [收到指令] <- {cmd} (来自主控端 {client_ip}:{addr[1]})")

            # 2. 构造协议要求的返回值 (例如: TG_OPEN_DEVICE;OK;)
            if cmd.endswith(";"):
                reply_str = f"{cmd}OK;"
            else:
                reply_str = f"{cmd};OK;"
            
            # 3. 将返回值打回给 Qt 的 5002 端口
            sock.sendto(reply_str.encode('utf-8'), (client_ip, REPLY_PORT))
            print(f"📤 [回传状态] -> {reply_str} (发往 {client_ip}:{REPLY_PORT})\n")

        except Exception as e:
            print(f"❌ 发生错误: {e}")

if __name__ == "__main__":
    simulate_hardware_device()
