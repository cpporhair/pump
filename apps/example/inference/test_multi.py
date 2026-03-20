#!/usr/bin/env python3
"""Automated multi-turn test — sends several Chinese prompts to verify
no UTF-8 decode errors across multiple exchanges."""

import codecs
import socket
import struct
import sys
import traceback


def send_prompt(sock, prompt):
    payload = prompt.encode("utf-8")
    total_len = len(payload) + 4
    sock.sendall(struct.pack("<I", total_len) + payload)


def recv_frame(sock):
    hdr = b""
    while len(hdr) < 4:
        chunk = sock.recv(4 - len(hdr))
        if not chunk:
            raise ConnectionError("connection closed")
        hdr += chunk
    total_len = struct.unpack("<I", hdr)[0]
    payload_len = total_len - 4
    if payload_len <= 0 or payload_len > 1_000_000:
        raise ValueError(f"bad frame: total_len={total_len}")

    payload = b""
    while len(payload) < payload_len:
        chunk = sock.recv(payload_len - len(payload))
        if not chunk:
            raise ConnectionError("connection closed")
        payload += chunk
    return payload


def recv_tokens(sock):
    decoder = codecs.getincrementaldecoder("utf-8")("replace")
    tokens = []
    while True:
        payload = recv_frame(sock)
        flags = payload[0]
        if flags == 0:
            text = decoder.decode(payload[1:], final=False)
            if text:
                tokens.append(text)
        elif flags == 1:
            tail = decoder.decode(b"", final=True)
            if tail:
                tokens.append(tail)
            break
        elif flags == 2:
            msg = payload[1:].decode("utf-8", errors="replace")
            raise RuntimeError(f"server error: {msg}")
        else:
            raise RuntimeError(f"unknown flags: {flags}")
    return "".join(tokens)


PROMPTS = [
    "你好，请用中文回答：1+1等于几？",
    "用中文解释一下什么是递归",
    "写一首关于春天的五言绝句",
    "Python和C++有什么区别？用中文回答",
    "请用中文讲一个笑话",
]


def main():
    host = "127.0.0.1"
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8080

    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.connect((host, port))
    print(f"Connected to {host}:{port}\n")

    for i, prompt in enumerate(PROMPTS):
        print(f"--- Q{i+1}: {prompt}")
        try:
            send_prompt(sock, prompt)
            reply = recv_tokens(sock)
            print(f"--- A{i+1}: {reply[:200]}{'...' if len(reply) > 200 else ''}")
            print()
        except Exception:
            traceback.print_exc()
            sock.close()
            sys.exit(1)

    print(f"\nAll {len(PROMPTS)} exchanges completed successfully!")
    sock.close()


if __name__ == "__main__":
    main()
