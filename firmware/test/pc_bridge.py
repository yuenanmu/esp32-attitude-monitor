import json  # json.dumps()函数：将 Python 对象转换为 JSON 字符串。
import threading #启动串口线程，Lock 用于保护共享状态，Event 用于线程间的停止信号。
import time  # time.time()函数：获取当前时间戳（秒）。

# 
from http.server import BaseHTTPRequestHandler #  处理请求
from http.server import ThreadingHTTPServer # 监听HTTP请求
from pathlib import Path # 路径处理, 创建路径对象

import serial # serial.Serial() 函数：创建串口对象。打开COM 端口。

from attitude_test import PORT, BAUDRATE, request_batch

# 静态文件目录
WEB_DIR = Path(__file__).with_name("web")
WEB_FILES = {
    "/": ("index.html", "text/html; charset=utf-8"),
    "/web/style.css": ("style.css", "text/css; charset=utf-8"),
    "/web/dashboard.js": ("dashboard.js", "text/javascript; charset=utf-8"),
    "/web/student-live.js": ("student-live.js", "text/javascript; charset=utf-8"),
    "/web/pose.js": ("pose.js", "text/javascript; charset=utf-8"),
    "/web/vendor/three.module.min.js": (
        "vendor/three.module.min.js", "text/javascript; charset=utf-8"
    ),
}

# 状态锁和停止信号
state_lock = threading.Lock()
stop_event = threading.Event()

# 串口写state,HTTP请求读state
state = {
    "connected": False,
    "latest": None, # 保存最新数据，就是record 姿态数据
    "received_count": 0,
    "missing_count": 0,
    "last_received_at": None, # 保存最新数据接收时间
    "last_error": None,
}

def serial_worker():
    after_seq = 0
    pending = bytearray() # 保存未处理的数据

    try:
        with serial.Serial(  # 创建串口对象。打开COM 端口。
            PORT,
            BAUDRATE,
            timeout=0.1,
            write_timeout=1.0,
        ) as port:
            if stop_event.wait(2.0):
                return

            port.reset_input_buffer()

            with open(
                "attitude_bridge.jsonl", # 数据存储文件
                "a", # 追加模式，保存数据。ps:没有则创建新文件。
                encoding="utf-8",
            ) as output:
                while not stop_event.is_set():
                    header, records = request_batch( # 请求批次。records 为字典列表
                        port, pending, after_seq
                    )

                    # 完整批次已由 request_batch 验证。
                    # 获取的records数据保存到 output 中，没有打印
                    for record in records:
                        output.write(
                            json.dumps(
                                record,
                                ensure_ascii=False,
                                allow_nan=False,
                            ) + "\n"
                        )

                    output.flush()

                    # TODO 8.1
                    # 计算本次缺失数。
                    # gap 为 1 时：
                    # max(0, oldest - after_seq - 1)
                    missing = 0
                    if len(records) > 0 :
                        oldest = records[0]["seq"]
                        missing = max(0, oldest - after_seq - 1)


                    # 锁内更新内存状态。
                    with state_lock:
                        state["connected"] = True
                        state["last_error"] = None
                        state["received_count"] += len(records)
                        state["missing_count"] += missing
                        if len(records) > 0:
                            state["latest"] = records[-1].copy()
                            state["last_received_at"] = time.time()

                    # TODO 8.3
                    # 有记录时推进 after_seq。
                    # 与你现有 attitude_test.py 相同。
                    if len(records) > 0:
                        after_seq = records[-1]["seq"]

                    if len(records) < 32:
                        stop_event.wait(0.2) # 先停0.2秒

    except (
        serial.SerialException,
        TimeoutError,
        ValueError,
        RuntimeError,
        OSError,
    ) as exc:
        with state_lock: # 锁内只更新内存状态。
            state["connected"] = False # 更新连接状态
            state["last_error"] = str(exc)

    finally:
        with state_lock: # 锁内只更新内存状态。
            state["connected"] = False

# 状态处理
class StateHandler(BaseHTTPRequestHandler):
    def do_GET(self): # 处理GET请求

        # 下面是在处理静态文件
        # self.path 是浏览器请求的路径，可能带查询参数。
        path = self.path.split("?", 1)[0] # 例如 "/web/style.css?v=2"：只取第一个 "?" 前面的 "/web/style.css"。
        if path in WEB_FILES:
            filename, content_type = WEB_FILES[path]
            try:
                # WEB_DIR 是磁盘上的 web 目录；这里的 `/` 是 Path 的路径拼接。
                body = (WEB_DIR / filename).read_bytes() # 响应体是静态文件内容，是画面
            except FileNotFoundError:
                self.send_error(404)
                return
            self.send_response(200)
            self.send_header("Content-Type", content_type)
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Cache-Control", "no-cache")
            self.end_headers()
            self.wfile.write(body) # wfile 是写回客户端的输出流。把文件的字节发给请求它的浏览器。
            return

        if path != "/api/state":
            self.send_error(404)
            return

        # 创建状态快照
        with state_lock:
            snapshot = state.copy()
            # 这个是什么深/浅拷贝？ 浅拷贝：
            # 深拷贝： 
            if snapshot["latest"] is not None:
                snapshot["latest"] = (
                    snapshot["latest"].copy()
                )
        # 把 record->state->snapshot->body,发送融合数据给浏览器
        body = json.dumps(
            snapshot,
            ensure_ascii=False,
            allow_nan=False,
        ).encode("utf-8")

        self.send_response(200) # 返回状态码200
        self.send_header(
            "Content-Type",
            "application/json; charset=utf-8",
        )
        self.send_header("Cache-Control", "no-store")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()

        self.wfile.write(body)

def main():
    # 创建服务器并绑定、监听本机 8000 端口，不是一个线程
    server = ThreadingHTTPServer( 
        ("127.0.0.1", 8000),
        StateHandler,
    )
    server.daemon_threads = True # 设置线程为守护线程,守护线程会在主线程结束后自动结束

    worker = threading.Thread( # 启动串口线程
        target=serial_worker,
        name="serial-worker",
        daemon=True,
    )
    worker.start() # 启动串口线程

    print("姿态监控：http://127.0.0.1:8000/")
    print("状态接口：http://127.0.0.1:8000/api/state")

    try:
        server.serve_forever() # 启动HTTP服务
    except KeyboardInterrupt:
        pass
    finally:
        stop_event.set()
        server.server_close()
        worker.join(timeout=4.0)


if __name__ == "__main__":
    main()
