import argparse
import json
import threading
import time
from urllib import request

import serial


def post_line(base_url: str, line: str, timeout: float) -> None:
    url = f"{base_url.rstrip('/')}/v1/ingest/line"
    payload = json.dumps({"line": line}).encode("utf-8")
    req = request.Request(url, data=payload, method="POST")
    req.add_header("Content-Type", "application/json")
    with request.urlopen(req, timeout=timeout) as resp:
        if resp.status >= 300:
            raise RuntimeError(f"HTTP {resp.status}")


def reader_loop(
    ser: serial.Serial,
    base_url: str,
    http_timeout: float,
    stop_event: threading.Event,
) -> None:
    while not stop_event.is_set():
        try:
            line = ser.readline()
        except serial.SerialException:
            break

        if not line:
            continue

        text = line.decode(errors="ignore").strip()
        if not text:
            continue

        print(f"[SER RX] {text}")
        try:
            post_line(base_url, text, timeout=http_timeout)
            print("[HTTP OK]")
        except Exception as exc:
            print(f"[HTTP ERR] {exc}")


def main() -> None:
    parser = argparse.ArgumentParser(description="Bridge serial lines to cloud HTTP ingest API")
    parser.add_argument("--port", default="COM16")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--serial-timeout", type=float, default=0.2)
    parser.add_argument("--http-timeout", type=float, default=2.0)
    parser.add_argument("--base-url", default="http://127.0.0.1:8000")
    args = parser.parse_args()

    with serial.Serial(args.port, args.baud, timeout=args.serial_timeout) as ser:
        print(f"[INFO] Serial connected: {args.port} @ {args.baud}")
        print(f"[INFO] Cloud ingest: {args.base_url}/v1/ingest/line")
        stop_event = threading.Event()
        t = threading.Thread(
            target=reader_loop,
            args=(ser, args.base_url, args.http_timeout, stop_event),
            daemon=True,
        )
        t.start()

        try:
            while True:
                time.sleep(0.5)
        except KeyboardInterrupt:
            pass
        finally:
            stop_event.set()
            t.join(timeout=0.5)


if __name__ == "__main__":
    main()
