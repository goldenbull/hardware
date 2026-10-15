#!/usr/bin/env python3
"""
kws-recorder 的上传服务：没有 TF 卡时，板子边录边把 PCM 通过 HTTP 传到这里。

只用 Python 3 标准库，直接跑：

    python3 tools/upload_server.py                 # 存到 ./kws-data，端口 8000
    python3 tools/upload_server.py --dir ~/kws --port 8000

启动时会打印本机局域网地址，把它填进 menuconfig → 录音存储 → 上传服务器地址。
浏览器打开同一个地址能看到已录的文件，可以直接试听 / 下载。

落盘的文件和 TF 卡上完全一样：

    open_001.wav  close_001.wav ...   16kHz/16bit/单声道，编号递增，永不覆盖
    sessions.csv                       每次录音一行，列和 TF 卡版本相同

协议（板子那头在 main/sink_http.c）：

    GET  /status                              -> 剩余空间 MB，兼做心跳
    POST /start?keyword=open                  -> 新建文件，返回文件名 open_003.wav
    POST /append?file=open_003.wav&offset=N   body 是裸 PCM，返回追加后的数据字节数
    POST /finish?file=...&keyword=...&...     记一行 sessions.csv

/append 带偏移量，所以可以放心重发：offset 比文件里已有的数据短，说明上一次其实
写进来了、只是回包丢了，截到 offset 再写一遍；offset 超过已有数据说明中间缺了一段，
回 409，板子那边就按失败处理。每次追加之后都回写 WAV 头，随时停掉服务，文件都能正常打开。
"""

import argparse
import csv
import html
import os
import re
import shutil
import socket
import struct
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, quote, urlsplit

HEADER_BYTES = 44
NAME_RE = re.compile(r"^([a-z]+)_(\d{3,})\.wav$")
PREFIX_RE = re.compile(r"^[a-z]+$")
# 一次追加的上限，板子每次只发 16KB，这里留足余量，防止误请求把内存吃光
MAX_APPEND_BYTES = 1 << 20

LOG_FIELDS = ["file", "keyword", "duration_s", "samples", "dropped_samples", "clipped_samples",
              "gain_db", "sample_rate", "wall_ms"]

# 板子只有一块，请求本来就是串行的，一把大锁足够
LOCK = threading.Lock()


def wav_header(rate, channels, bits, data_bytes):
    block_align = channels * bits // 8
    return struct.pack("<4sI4s4sIHHIIHH4sI",
                       b"RIFF", 36 + data_bytes, b"WAVE",
                       b"fmt ", 16, 1, channels, rate, rate * block_align, block_align, bits,
                       b"data", data_bytes)


def local_ips():
    """猜本机局域网地址：往外"连"一个 UDP 地址，看系统选哪块网卡，不会真的发包"""
    ips = []
    for target in ("8.8.8.8", "192.168.1.1", "10.0.0.1"):
        try:
            with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
                s.connect((target, 80))
                ip = s.getsockname()[0]
                if ip not in ips and not ip.startswith("127."):
                    ips.append(ip)
        except OSError:
            pass
    return ips


class Handler(BaseHTTPRequestHandler):
    # HTTP/1.1 才能保持连接，板子每秒传一块，不用每次重新握手
    protocol_version = "HTTP/1.1"
    server_version = "kws-upload/1.0"
    data_dir = "."

    # ---- 工具 ----

    def reply(self, code, body="", content_type="text/plain; charset=utf-8"):
        data = body.encode("utf-8") if isinstance(body, str) else body
        self.send_response(code)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def read_body(self):
        length = int(self.headers.get("Content-Length") or 0)
        if length < 0 or length > MAX_APPEND_BYTES:
            raise ValueError(f"body 太大：{length}")
        data = self.rfile.read(length) if length else b""
        if len(data) != length:
            raise ValueError(f"body 不完整：{len(data)}/{length}")
        return data

    def file_path(self, query):
        name = query.get("file", [""])[0]
        if not NAME_RE.match(name):
            raise ValueError(f"文件名不合法：{name!r}")
        return name, os.path.join(self.data_dir, name)

    def log_message(self, fmt, *args):
        # /append 和 /status 太频繁，只打错误；其余在各自的处理函数里打
        pass

    def log_error(self, fmt, *args):
        print(f"[{time.strftime('%H:%M:%S')}] {self.client_address[0]} " + fmt % args, file=sys.stderr)

    def say(self, msg):
        print(f"[{time.strftime('%H:%M:%S')}] {self.client_address[0]} {msg}", flush=True)

    # ---- 路由 ----

    def do_GET(self):
        url = urlsplit(self.path)
        if url.path == "/status":
            free_mb = shutil.disk_usage(self.data_dir).free >> 20
            self.reply(200, str(free_mb))
        elif url.path == "/":
            self.reply(200, self.index_page(), "text/html; charset=utf-8")
        elif url.path.startswith("/files/"):
            self.send_wav(url.path[len("/files/"):])
        else:
            self.reply(404, "not found")

    def do_POST(self):
        url = urlsplit(self.path)
        query = parse_qs(url.query)
        try:
            body = self.read_body()
            if url.path == "/start":
                self.handle_start(query)
            elif url.path == "/append":
                self.handle_append(query, body)
            elif url.path == "/finish":
                self.handle_finish(query)
            else:
                self.reply(404, "not found")
        except (ValueError, KeyError) as e:
            self.log_error("请求有误 %s：%s", self.path, e)
            self.reply(400, str(e))
        except OSError as e:
            self.log_error("写文件失败 %s：%s", self.path, e)
            self.reply(500, str(e))

    # ---- 录音 ----

    def handle_start(self, query):
        prefix = query.get("keyword", [""])[0]
        if not PREFIX_RE.match(prefix):
            raise ValueError(f"keyword 不合法：{prefix!r}")
        rate = int(query.get("rate", ["16000"])[0])
        channels = int(query.get("channels", ["1"])[0])
        bits = int(query.get("bits", ["16"])[0])

        with LOCK:
            # 编号规则和 TF 卡版本一样：同前缀里最大的编号 +1，不区分大小写
            max_idx = 0
            for entry in os.listdir(self.data_dir):
                m = NAME_RE.match(entry.lower())
                if m and m.group(1) == prefix:
                    max_idx = max(max_idx, int(m.group(2)))
            name = f"{prefix}_{max_idx + 1:03d}.wav"
            # "x" 模式：文件已存在就失败，绝不覆盖
            with open(os.path.join(self.data_dir, name), "xb") as f:
                f.write(wav_header(rate, channels, bits, 0))

        self.say(f"开始录音 {name}")
        self.reply(200, name)

    def handle_append(self, query, body):
        name, path = self.file_path(query)
        offset = int(query["offset"][0])
        if offset < 0 or offset % 2 or len(body) % 2:
            raise ValueError(f"偏移或长度不是整数个采样：offset={offset} len={len(body)}")

        with LOCK:
            if not os.path.exists(path):
                self.reply(404, "no such file")
                return
            have = os.path.getsize(path) - HEADER_BYTES
            if offset > have:
                # 中间缺了一段，拼上去声音就错位了，让板子报错
                self.log_error("%s 数据不连续：已有 %d 字节，请求从 %d 开始", name, have, offset)
                self.reply(409, str(have))
                return
            if offset < have:
                self.say(f"{name} 重发：截回 {offset} 字节（原有 {have}）")
            with open(path, "r+b") as f:
                f.truncate(HEADER_BYTES + offset)
                f.seek(HEADER_BYTES + offset)
                f.write(body)
                total = offset + len(body)
                # 只改头里的两个长度字段，采样率等沿用 /start 时写的
                f.seek(4)
                f.write(struct.pack("<I", 36 + total))
                f.seek(40)
                f.write(struct.pack("<I", total))

        self.reply(200, str(total))

    def handle_finish(self, query):
        name, path = self.file_path(query)
        row = {k: query.get(k, [""])[0] for k in LOG_FIELDS}
        row["file"] = name

        with LOCK:
            log_path = os.path.join(self.data_dir, "sessions.csv")
            exists = os.path.exists(log_path)
            with open(log_path, "a", newline="") as f:
                w = csv.DictWriter(f, fieldnames=LOG_FIELDS)
                if not exists:
                    w.writeheader()
                w.writerow(row)

        size = os.path.getsize(path) - HEADER_BYTES if os.path.exists(path) else 0
        warn = ""
        if row["dropped_samples"] not in ("", "0"):
            warn += f"  丢帧 {row['dropped_samples']} 采样！"
        if query.get("complete", ["1"])[0] != "1":
            warn += "  上传不完整！"
        self.say(f"录音结束 {name}：{row['duration_s']} 秒，{size / 1024:.0f} KB，"
                 f"削顶 {row['clipped_samples']}{warn}")
        self.reply(200, "ok")

    # ---- 浏览 ----

    def send_wav(self, name):
        if not NAME_RE.match(name):
            self.reply(404, "not found")
            return
        path = os.path.join(self.data_dir, name)
        if not os.path.exists(path):
            self.reply(404, "not found")
            return
        with open(path, "rb") as f:
            data = f.read()
        self.reply(200, data, "audio/wav")

    def index_page(self):
        rows = []
        for entry in sorted(os.listdir(self.data_dir)):
            if not NAME_RE.match(entry):
                continue
            size = os.path.getsize(os.path.join(self.data_dir, entry))
            secs = max(size - HEADER_BYTES, 0) / 32000
            href = "/files/" + quote(entry)
            rows.append(f"<tr><td><a href='{href}'>{html.escape(entry)}</a></td>"
                        f"<td>{int(secs // 60)}:{int(secs % 60):02d}</td>"
                        f"<td><audio controls preload='none' src='{href}'></audio></td></tr>")
        free_gb = shutil.disk_usage(self.data_dir).free / 2**30
        return ("<!doctype html><meta charset='utf-8'><title>kws 录音</title>"
                "<style>body{font:14px sans-serif;margin:16px}td{padding:4px 12px}</style>"
                f"<h3>{html.escape(os.path.abspath(self.data_dir))}</h3>"
                f"<p>{len(rows)} 个文件，剩余 {free_gb:.1f} GB</p>"
                "<table>" + "".join(rows) + "</table>")


def main():
    ap = argparse.ArgumentParser(description="kws-recorder 录音上传服务")
    ap.add_argument("--dir", default="kws-data", help="录音存放目录（默认 ./kws-data）")
    ap.add_argument("--port", type=int, default=8000)
    ap.add_argument("--bind", default="0.0.0.0", help="监听地址（默认所有网卡）")
    args = ap.parse_args()

    os.makedirs(args.dir, exist_ok=True)
    Handler.data_dir = args.dir

    server = ThreadingHTTPServer((args.bind, args.port), Handler)
    print(f"录音存到 {os.path.abspath(args.dir)}")
    ips = local_ips()
    if ips:
        for ip in ips:
            print(f"menuconfig 里填：http://{ip}:{args.port}")
    else:
        print(f"没猜出本机局域网地址，用 ifconfig 查一下，端口 {args.port}")
    print("Ctrl+C 退出", flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print()


if __name__ == "__main__":
    main()
