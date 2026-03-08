#!/usr/bin/env python3
"""
DPDK echo 测试客户端

用法:
  1. 启动 DPDK server 并配置 TAP 网口:
     sudo ./dpdk udp server
     sudo ip addr add 10.0.0.1/24 dev dtap0
     sudo ip link set dtap0 up

  2. 运行本脚本:
     python3 test.py udp              # 测试 UDP echo
     python3 test.py udp 10.0.0.1     # 指定 server IP
     python3 test.py udp 10.0.0.1 10  # 发 10 条消息

  KCP 测试类似:
     sudo ./dpdk kcp server
     sudo ip addr add 10.0.0.1/24 dev dtap0 && sudo ip link set dtap0 up
     python3 test.py kcp
"""

import socket
import struct
import sys
import time


def test_udp(server_ip, server_port, count):
    print(f"UDP echo test -> {server_ip}:{server_port}, {count} messages")
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.settimeout(2.0)

    ok = 0
    fail = 0
    for i in range(count):
        data = struct.pack("<i", i)
        sock.sendto(data, (server_ip, server_port))
        try:
            resp, addr = sock.recvfrom(1024)
            v = struct.unpack("<i", resp)[0]
            if v == i:
                print(f"  [{i}] sent {i}, got {v} ✓")
                ok += 1
            else:
                print(f"  [{i}] sent {i}, got {v} ✗ (mismatch)")
                fail += 1
        except socket.timeout:
            print(f"  [{i}] sent {i}, timeout ✗")
            fail += 1

    sock.close()
    print(f"\nResult: {ok}/{count} passed, {fail}/{count} failed")
    return fail == 0


def test_kcp(server_ip, server_port, count):
    """KCP 底层走 UDP，握手协议: 5 字节 SYN/ACK 包"""
    print(f"KCP echo test -> {server_ip}:{server_port}, {count} messages")
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.settimeout(2.0)

    # KCP handshake: SYN (type=0xF0, conv=1)
    conv = 1
    syn = struct.pack("<BI", 0xF0, conv)
    sock.sendto(syn, (server_ip, server_port))

    try:
        resp, _ = sock.recvfrom(1024)
        ack_type, ack_conv = struct.unpack("<BI", resp[:5])
        if ack_type != 0xF1 or ack_conv != conv:
            print(f"Handshake failed: type=0x{ack_type:02X} conv={ack_conv}")
            sock.close()
            return False
        print(f"  Connected, conv={conv}")
    except socket.timeout:
        print("  Handshake timeout")
        sock.close()
        return False

    # KCP data exchange requires the full KCP protocol (ikcp)
    # which is non-trivial to implement in Python.
    # Just verify the handshake succeeded.
    print(f"\nKCP handshake OK. Full data test requires KCP protocol implementation.")
    print(f"Use the C++ client (./dpdk kcp client) for full KCP echo test.")

    sock.close()
    return True


def main():
    if len(sys.argv) < 2:
        print("Usage: test.py <udp|kcp> [server_ip] [count]")
        print("  server_ip  default: 10.0.0.1")
        print("  count      default: 5")
        return

    mode = sys.argv[1]
    server_ip = sys.argv[2] if len(sys.argv) > 2 else "10.0.0.1"
    count = int(sys.argv[3]) if len(sys.argv) > 3 else 5

    ports = {"udp": 19200, "kcp": 19300}
    if mode not in ports:
        print(f"Unknown mode: {mode}")
        return

    if mode == "udp":
        test_udp(server_ip, ports[mode], count)
    elif mode == "kcp":
        test_kcp(server_ip, ports[mode], count)


if __name__ == "__main__":
    main()
