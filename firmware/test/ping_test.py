import time

import serial


PORT = "COM9"  # TODO 4：改为你的开发板端口。
BAUDRATE = 115200


def send_ping(port):
    # TODO 5：
    # 向串口发送 PING，并带上换行符。
    # 提示：port.write(b"...")
    # b 前缀表示字节数据。
    port.write(b"PING\n")
    # raise NotImplementedError("请完成 PING 发送代码")


def wait_for_reply(port, timeout_seconds=2.0):
    deadline = time.monotonic() + timeout_seconds
    pending = bytearray()

    while time.monotonic() < deadline:
        chunk = port.readline(256)

        if not chunk:
            continue

        pending.extend(chunk)

        while b"\n" in pending:
            raw_line, _, remainder = pending.partition(b"\n")
            pending = bytearray(remainder)

            line = raw_line.decode(
                "utf-8", errors="replace"
            ).strip()

            if line == "PONG" or line.startswith("ERR "):
                return line

            if line:
                print(f"[设备日志] {line}")

        if len(pending) > 4096:
            raise RuntimeError("接收到过长且没有换行的数据")

    return None


def main():
    success_count = 0
    total_count = 20

    with serial.Serial(
        PORT,
        BAUDRATE,
        timeout=0.1,
        write_timeout=1.0,
    ) as port:
        # 给设备启动和串口连接留出时间。
        time.sleep(2)
        port.reset_input_buffer()

        for index in range(total_count):
            send_ping(port)
            reply = wait_for_reply(port)

            if reply == "PONG":
                success_count += 1
                print(f"[{index + 1}] 成功：PONG")
            elif reply is None:
                print(f"[{index + 1}] 超时")
            else:
                print(f"[{index + 1}] 设备返回错误：{reply}")

            time.sleep(0.2)

    print(f"结果：{success_count}/{total_count} 次成功")


if __name__ == "__main__":
    try:
        main()
    except serial.SerialException as exc:
        print(f"串口错误：{exc}")
    except KeyboardInterrupt:
        print("测试已停止")