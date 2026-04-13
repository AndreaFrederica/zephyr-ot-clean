import argparse
import threading
import time

import serial


def reader_loop(ser: serial.Serial, stop_event: threading.Event) -> None:
    while not stop_event.is_set():
        try:
            line = ser.readline()
        except serial.SerialException:
            break

        if not line:
            continue

        try:
            text = line.decode(errors="ignore").rstrip()
        except Exception:
            text = repr(line)

        if text:
            print(f"[SCREEN RX] {text}")


def send_line(ser: serial.Serial, text: str, line_end: str) -> None:
    payload = (text.strip() + line_end).encode()
    ser.write(payload)
    ser.flush()
    print(f"[SCREEN TX] {text.strip()}")


def print_help() -> None:
    print("可用命令:")
    print("  get <id>          -> 发送 GET,<id>")
    print("  lock <id> <0|1>   -> 发送 LOCK,<id>,<0|1>")
    print("  raw <text>        -> 原样发送一行文本")
    print("  help              -> 显示帮助")
    print("  q / quit / exit   -> 退出")


def main() -> None:
    parser = argparse.ArgumentParser(description="Parking Lock 串口屏模拟器")
    parser.add_argument("--port", default="COM16")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--timeout", type=float, default=0.2)
    parser.add_argument("--line-end", default="\\r\\n", choices=["\\r\\n", "\\n", "\\r"])
    parser.add_argument("--listen-only", action="store_true")
    args = parser.parse_args()

    line_end = args.line_end.encode("utf-8").decode("unicode_escape")

    with serial.Serial(args.port, args.baud, timeout=args.timeout) as ser:
        print(f"[INFO] 已连接 {args.port} @ {args.baud}")
        stop_event = threading.Event()
        reader = threading.Thread(target=reader_loop, args=(ser, stop_event), daemon=True)
        reader.start()

        # 给主机一点时间发送 GATEWAY_READY
        time.sleep(0.4)

        if args.listen_only:
            print("[INFO] 监听模式中，按 Ctrl+C 退出")
            try:
                while True:
                    time.sleep(0.5)
            except KeyboardInterrupt:
                pass
            finally:
                stop_event.set()
                reader.join(timeout=0.5)
            return

        print_help()
        while True:
            cmd = input("screen> ").strip()
            if not cmd:
                continue

            low = cmd.lower()
            if low in {"q", "quit", "exit"}:
                break
            if low == "help":
                print_help()
                continue

            parts = cmd.split()
            if parts[0].lower() == "get" and len(parts) == 2:
                send_line(ser, f"GET,{parts[1]}", line_end)
                continue

            if parts[0].lower() == "lock" and len(parts) == 3:
                send_line(ser, f"LOCK,{parts[1]},{parts[2]}", line_end)
                continue

            if parts[0].lower() == "raw" and len(parts) >= 2:
                send_line(ser, cmd[4:], line_end)
                continue

            print("[WARN] 命令格式不正确，输入 help 查看用法")

        stop_event.set()
        reader.join(timeout=0.5)
        print("[INFO] 退出")


if __name__ == "__main__":
    main()
