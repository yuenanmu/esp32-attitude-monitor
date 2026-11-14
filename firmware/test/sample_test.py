import time
import math

import serial
import json

PORT = "COM9"
BAUDRATE = 115200


def read_line(port, pending, deadline):
    while time.monotonic() < deadline:
        newline = pending.find(b"\n")

        if newline >= 0:
            raw = bytes(pending[:newline])
            del pending[:newline + 1]
            return raw.decode("utf-8", errors="strict").strip()

        chunk = port.read(256)

        if chunk:
            pending.extend(chunk)

        if len(pending) > 8192:
            raise ValueError("接收缓冲过长")

    raise TimeoutError("等待完整批次超时")


def parse_begin(line, expected_after):
    fields = line.split()

    # TODO 4.1
    # 确认共 7 项，第一项为 BEGIN。
    # 将其余字段转换为 int。
    # 检查返回的 after_seq 与 expected_after 相同。
    # 检查 count 在 0～32、gap 为 0 或 1。
    #
    # 返回字典，包含：
    # after、count、oldest、latest、overwrites、gap
    if len(fields) != 7 or fields[0] != "BEGIN":
        raise ValueError("格式错误")
    try:
        after_seq = int(fields[1])
        count = int(fields[2])
        oldest = int(fields[3])
        latest = int(fields[4])
        overwrites = int(fields[5])
        gap = int(fields[6])
    except ValueError:
        raise ValueError("格式错误")

    if after_seq != expected_after:
        raise ValueError("序号不匹配")

    if count < 0 or count > 32:
        raise ValueError("记录数超出范围")

    if gap not in (0, 1):
        raise ValueError("gap 值不正确")

    return {
        "after": after_seq,
        "count": count,
        "oldest": oldest,
        "latest": latest,
        "overwrites": overwrites,
        "gap": gap
    }
    


def parse_record(line):
    fields = line.split()

    if len(fields) != 9 or fields[0] != "DATA":
        raise ValueError("格式错误")
    try:
        seq = int(fields[1])
        t_us = int(fields[2])
        ax = float(fields[3])
        ay = float(fields[4])
        az = float(fields[5])
        gx = float(fields[6])
        gy = float(fields[7])
        gz = float(fields[8])
    except ValueError:
        raise ValueError("格式错误")
    
    if (not math.isfinite(ax) or not math.isfinite(ay) or not math.isfinite(az) or 
        not math.isfinite(gx) or not math.isfinite(gy) or not math.isfinite(gz)):
        raise ValueError("六轴值超出范围")

    return {
        "seq": seq,
        "t_us": t_us,
        "ax": ax,
        "ay": ay,
        "az": az,
        "gx": gx,
        "gy": gy,
        "gz": gz
    }


def validate_batch(header, records, end_line):
    # TODO 4.3
    # 验证：
    # 1. len(records) == header["count"]
    # 2. END 只有两项，序号正确
    # 3. 每条 seq > header["after"]
    # 4. 本批 seq、t_us 严格递增
    # 5. 非空批次中，记录位于 oldest～latest 范围内
    # 6. 非空批次且 gap 为 0：
    #    第一条 seq == header["after"] + 1
    #
    # END 预期序号：
    # 有记录用最后一条 seq，否则用 header["after"]。
    #
    # 不符合时 raise ValueError(...)
    if len(records) != header["count"]:
        raise ValueError("记录数不匹配")
    fields = end_line.split()
    if len(fields) != 2 or fields[0] != "END":
        raise ValueError("END 格式错误")
    try:
        end_seq = int(fields[1])
    except ValueError as exc:
        raise ValueError("END 值错误") from exc
    # 检查每条记录
    prev_seq = header["after"]
    prev_t_us = None
    for record in records:
        seq = record["seq"]
        timestamp = record["t_us"]

        if seq <= header["after"]:
            raise ValueError("记录序号不大于起始点 after")
        if seq <= prev_seq:
            raise ValueError("记录序号不大于前一条")
        if prev_t_us is not None and timestamp <= prev_t_us:
            raise ValueError("记录时间戳不大于前一条")
        prev_seq = seq
        prev_t_us = timestamp
        if not header["oldest"] <= seq <= header["latest"]:
            raise ValueError("记录序号不在 oldest～latest 范围内")
    expected_end = header["after"]
    if records:
        expected_end = records[-1]["seq"]

    if end_seq != expected_end:
        raise ValueError("END 序号不正确")

def request_batch(port, pending, after_seq, limit=32):
    command = f"GET_SAMPLES {after_seq} {limit}\n"
    port.write(command.encode("ascii"))

    deadline = time.monotonic() + 3.0
    header = None
    records = []

    while True:
        line = read_line(port, pending, deadline)

        if not line:
            continue

        if line.startswith("ERR "):
            raise RuntimeError(line)

        if line.startswith("BEGIN "):
            if header is not None:
                raise ValueError("批次尚未结束又收到 BEGIN")
            header = parse_begin(line, after_seq)

        elif line.startswith("DATA "):
            if header is None:
                raise ValueError("收到 DATA 前没有 BEGIN")

            records.append(parse_record(line))

            if len(records) > header["count"]:
                raise ValueError("记录数超过声明值")

        elif line.startswith("END "):
            if header is None:
                raise ValueError("收到 END 前没有 BEGIN")

            validate_batch(header, records, line)
            return header, records

        else:
            print("[设备日志]", line)


def main():
    after_seq = 0
    pending = bytearray()

    with serial.Serial(
        PORT,
        BAUDRATE,
        timeout=0.1,
        write_timeout=1.0,
    ) as port:
        time.sleep(2)
        port.reset_input_buffer()

        with open(
            "samples.jsonl", "a", encoding="utf-8"
        ) as output:
            while True:
                header, records = request_batch(
                    port, pending, after_seq
                )

                # request_batch() 返回前已经完成整批校验。
                if header["gap"]:
                    missing_count = header["oldest"] - after_seq - 1

                    print(
                        f"[数据缺口] 缺失序号 "
                        f"{after_seq + 1}～{header['oldest'] - 1}，"
                        f"共 {missing_count} 条"
                    )

                # 每条记录序列化为一行 JSON。
                for record in records:
                    line = json.dumps(record, ensure_ascii=False)
                    output.write(line + "\n")

                # 将 Python 文件缓冲区内容提交给操作系统。
                output.flush()

                # 整批写入成功后，更新下一次查询的起点。
                if records:
                    after_seq = records[-1]["seq"]

                print(
                    f"本批 {len(records)} 条，"
                    f"已保存到 {after_seq}"
                )

                # 满批时继续取，尽快清理积压。
                if len(records) < 32:
                    time.sleep(0.2)


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        print("已停止")
    except (serial.SerialException,
            TimeoutError,
            ValueError,
            RuntimeError) as exc:
        print("测试停止", exc)