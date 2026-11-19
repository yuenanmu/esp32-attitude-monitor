import json  # json.dumps()函数：将 Python 对象转换为 JSON 字符串。
import threading #启动串口线程，Lock 用于保护共享状态，Event 用于线程间的停止信号。
import time  # time.time()函数：获取当前时间戳（秒）。

# 
from http.server import BaseHTTPRequestHandler #  处理请求
from http.server import ThreadingHTTPServer # 监听HTTP请求
from pathlib import Path # 路径处理, 创建路径对象

import serial # serial.Serial() 函数：创建串口对象。打开COM 端口。

from attitude_test import PORT, BAUDRATE, read_line, request_batch

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
    "device_status": None,
    "control": None,
}

ACTIVE_PHASES = {"queued", "running"}


def control_exchange(port, pending, command, prefix):
    port.write((command + "\n").encode("ascii"))
    deadline = time.monotonic() + 2.0
    while True:
        line = read_line(port, pending, deadline)
        if line.startswith((prefix, "ERR ")):
            return line


def update_control(**changes):
    with state_lock:
        state["control"] = {**state["control"], **changes}


def service_control(port, pending, next_status_at, control_deadline):
    # 只有串口线程调用此函数。每次完整读取一个命令的响应。
    with state_lock:
        control = state["control"]

    now = time.monotonic()
    if control and control["phase"] == "queued":
        update_control(phase="running")
        calibrate = control["action"] == "calibrate"
        command = "CALIBRATE_GYRO" if calibrate else f"SET_RATE {control['rate_hz']}"
        prefix = "CALIBRATION_REQUESTED " if calibrate else "QUEUED "
        reply = control_exchange(port, pending, command, prefix)
        if reply.startswith("ERR "):
            update_control(phase="failed", detail=reply)
        else:
            update_control(device_id=int(reply.removeprefix(prefix)))
            control_deadline = time.monotonic() + 15.0
        # 下一轮立即读取状态；先让采集循环继续处理一批。
        return 0.0, control_deadline

    running = control and control["phase"] == "running"
    if running and now >= control_deadline:
        update_control(phase="unknown", detail="等待设备完成超时，请检查设备状态。")
        running = False

    if now < next_status_at:
        return next_status_at, control_deadline

    reply = control_exchange(port, pending, "GET_STATUS", "STATUS ")
    if reply.startswith("ERR "):
        with state_lock:
            state["device_status"] = None
        return time.monotonic() + 1.0, control_deadline

    _, mode, rate_hz, request_id, pending_flag, result = reply.split()
    if mode not in {"RUNNING", "CALIBRATING"} or pending_flag not in {"0", "1"}:
        raise ValueError(f"无效的设备状态：{reply}")
    device_status = {
        "mode": mode,
        "rate_hz": int(rate_hz),
        "request_id": int(request_id),
        "pending": pending_flag == "1",
        "result": int(result),
    }
    with state_lock:
        state["device_status"] = device_status
        # pending 在校准开始时就会清零，因此还要检查 mode。
        if (running and device_status["request_id"] == control["device_id"]
                and mode == "RUNNING" and not device_status["pending"]):
            if device_status["result"] != 0:
                phase, detail = "failed", f"设备结果码：{result}"
            elif control["action"] == "set_rate" and device_status["rate_hz"] != control["rate_hz"]:
                phase, detail = "failed", "设备报告的采样率与目标不一致。"
            else:
                phase, detail = "succeeded", None
            state["control"] = {**control, "phase": phase, "detail": detail}
            running = False
    return time.monotonic() + (0.25 if running else 1.0), control_deadline


def serial_worker():
    after_seq = 0
    pending = bytearray() # 保存未处理的数据
    next_status_at = control_deadline = 0.0

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

                    next_status_at, control_deadline = service_control(
                        port, pending, next_status_at, control_deadline
                    )
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
            state["device_status"] = None
            control = state["control"]
            if control and control["phase"] in ACTIVE_PHASES:
                queued = control["phase"] == "queued"
                state["control"] = {
                    **control,
                    "phase": "failed" if queued else "unknown",
                    "detail": state["last_error"] or "桥接已停止。",
                }

# 状态处理
class StateHandler(BaseHTTPRequestHandler):
    def do_GET(self): # 处理GET请求

        # 下面是在处理静态文件
        # self.path 是浏览器请求的路径，可能带查询参数。
        path = self.path.split("?", 1)[0] # 例如 "/web/style.css?v=2"：遇到？，就截断。只切分一次。获取第一个参数。
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
        self.send_json(200, snapshot)

    def send_json(self, status, data):
        body = json.dumps(
            data,
            ensure_ascii=False,
            allow_nan=False,
        ).encode("utf-8")

        self.send_response(status)
        self.send_header(
            "Content-Type",
            "application/json; charset=utf-8",
        )
        self.send_header("Cache-Control", "no-store")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()

        self.wfile.write(body)

    def do_POST(self):
        if self.path != "/api/control":
            self.send_json(404, {"error": "接口不存在。"})
            return
        if self.headers.get("Content-Type", "").split(";", 1)[0].strip().lower() != "application/json":
            self.send_json(415, {"error": "请求必须使用 JSON。"})
            return
        try:
            length = int(self.headers.get("Content-Length", "0"))
            if not 0 < length <= 1024:
                raise ValueError()
            data = json.loads(self.rfile.read(length).decode("utf-8"))
        except ValueError:
            self.send_json(400, {"error": "请求体无效。"})
            return
        action = data.get("action") if isinstance(data, dict) else None
        rate_hz = data.get("rate_hz") if action == "set_rate" else None
        if (action not in ("calibrate", "set_rate")
                or action == "set_rate" and (type(rate_hz) is not int or rate_hz not in (100, 200))):
            self.send_json(400, {"error": "请选择校准，或设置 100 / 200 Hz。"})
            return

        with state_lock:
            control = state["control"]
            device = state["device_status"]
            if not state["connected"]:
                status, response = 503, {"error": "开发板未连接。"}
            elif ((control and control["phase"] in ACTIVE_PHASES)
                    or (device and (device["pending"] or device["mode"] == "CALIBRATING"))):
                status, response = 409, {"error": "开发板正在处理操作，请稍后再试。"}
            else:
                control_id = control["id"] + 1 if control else 1
                state["control"] = {
                    "id": control_id, "action": action, "rate_hz": rate_hz,
                    "phase": "queued", "device_id": None, "detail": None,
                }
                status, response = 202, {"id": control_id, "phase": "queued"}
        self.send_json(status, response)

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
