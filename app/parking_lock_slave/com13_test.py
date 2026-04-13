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
        if line:
            try:
                print(f"[RX] {line.decode(errors='ignore').rstrip()}")
            except Exception:
                print(f"[RX] {line!r}")


def send_cmd(ser: serial.Serial, text: str, line_end: str) -> None:
    payload = (text.strip() + line_end).encode()
    ser.write(payload)
    ser.flush()
    print(f"[TX] {text.strip()}")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", default="COM13")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--node", type=int, default=1)
    parser.add_argument("--timeout", type=float, default=0.2)
    parser.add_argument("--line-end", default="\\r\\n", choices=["\\r\\n", "\\n", "\\r"])
    parser.add_argument("--listen", action="store_true")
    parser.add_argument("--listen-seconds", type=float, default=8.0)
    parser.add_argument("--broadcast", action="store_true")
    args = parser.parse_args()
    line_end = args.line_end.encode("utf-8").decode("unicode_escape")
    node = 0 if args.broadcast else args.node

    with serial.Serial(args.port, args.baud, timeout=args.timeout) as ser:
        stop_event = threading.Event()
        t = threading.Thread(target=reader_loop, args=(ser, stop_event), daemon=True)
        t.start()

        if args.listen:
            print("监听模式：请在倒计时内重启 MCU 观察 BOOT 帧")
            time.sleep(args.listen_seconds)
            stop_event.set()
            t.join(timeout=0.5)
            return

        time.sleep(0.8)
        send_cmd(ser, f"REQ,{node},PING", line_end)
        time.sleep(0.8)
        send_cmd(ser, f"REQ,{node},GET", line_end)
        time.sleep(0.8)
        send_cmd(ser, f"SET,{node},LOCK,1", line_end)
        time.sleep(0.8)
        send_cmd(ser, f"SET,{node},LOCK,0", line_end)

        print("输入命令后回车发送，输入 q 退出")
        while True:
            cmd = input("> ").strip()
            if cmd.lower() in {"q", "quit", "exit"}:
                break
            if cmd:
                send_cmd(ser, cmd, line_end)

        stop_event.set()
        t.join(timeout=0.5)


if __name__ == "__main__":
    main()
