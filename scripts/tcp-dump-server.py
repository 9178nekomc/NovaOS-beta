#!/usr/bin/env python3
# scripts/tcp-dump-server.py - 调试用：监听 8080，把收到的字节原样记录到文件
# 用法：python3 scripts/tcp-dump-server.py <logfile>
import socket
import sys
import threading
import time

logfile = sys.argv[1] if len(sys.argv) > 1 else "build/tcp-dump.log"
PORT = 8080

def log(msg):
    with open(logfile, "a") as f:
        f.write(time.strftime("[%H:%M:%S] ") + msg + "\n")

def handle(conn, addr):
    log(f"ACCEPT from {addr}")
    try:
        conn.settimeout(5)
        data = b""
        while True:
            try:
                chunk = conn.recv(4096)
            except socket.timeout:
                log("TIMEOUT waiting for data")
                break
            if not chunk:
                break
            data += chunk
            log(f"RECV {len(chunk)}B total={len(data)}B: {chunk.hex()}")
        log(f"CLOSE total={len(data)}B")
        if data:
            resp = b"HTTP/1.1 200 OK\r\nContent-Length: 0\r\nConnection: close\r\n\r\n"
            try:
                conn.sendall(resp)
                log(f"SENT {len(resp)}B response")
            except OSError as e:
                log(f"SEND FAILED: {e}")
    except OSError as e:
        log(f"ERROR: {e}")
    finally:
        conn.close()

with open(logfile, "a") as f:
    f.write(time.strftime("[%H:%M:%S] ") + "SERVER START 127.0.0.1:%d\n" % PORT)

srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
srv.bind(("127.0.0.1", PORT))
srv.listen(4)
log("LISTENING")
while True:
    conn, addr = srv.accept()
    threading.Thread(target=handle, args=(conn, addr), daemon=True).start()
