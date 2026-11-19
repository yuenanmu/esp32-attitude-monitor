import time

import serial

from attitude_test import PORT, BAUDRATE, read_line

def exchange(port, pending, command):
    port.write((command + "\n").encode("ascii"))
    deadline = time.monotonic() + 2.0

    # 处理固件的消息
    while True:
        line = read_line(port, pending, deadline)

        if line.startswith(("QUEUED ", "STATUS ", "ERR ")):
            return line

        if line == "PONG":
            return line

        print("[设备日志]", line)


def main():
    pending = bytearray()

    with serial.Serial(
        PORT,
        BAUDRATE,
        timeout=0.1,
        write_timeout=1.0,
    ) as port:
        while True:
            command = input("命令> ").strip()

            if command.lower() == "exit":
                break

            if command:
                print(exchange(port, pending, command))


if __name__ == "__main__":
    try:
        main()
    except (serial.SerialException,
            TimeoutError,
            ValueError) as exc:
        print("测试停止", exc)