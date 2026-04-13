import argparse
import threading
import time
from dataclasses import dataclass

import serial

try:
    from textual.app import App, ComposeResult
    from textual.containers import Container
    from textual.widgets import Button, Footer, Header, Input, RichLog, Static
except ImportError as exc:  # pragma: no cover
    raise SystemExit(
        "缺少 textual 依赖，请先安装后再运行:\n"
        "  pixi run python -m pip install textual\n"
    ) from exc


@dataclass
class SerialConfig:
    port: str
    baud: int
    timeout: float
    line_end: str


@dataclass
class NodeSnapshot:
    temp: str = "-"
    humi: str = "-"
    flame_digital: str = "-"
    flame_analog: str = "-"
    lock: str = "-"
    reason: str = "-"
    uid: str = "-"


def parse_report_summary(line: str) -> str | None:
    line = line.strip()
    if not line.startswith("REPORT,"):
        return None
    parts = line.split(",")
    if len(parts) < 4:
        return None
    node = parts[1]
    reason = parts[2]
    kv = {}
    for item in parts[3:]:
        if "=" in item:
            k, v = item.split("=", 1)
            kv[k] = v
    return (
        f"Node {node} | {reason} | "
        f"T={kv.get('T', '-')}, H={kv.get('H', '-')}, "
        f"FD={kv.get('FD', '-')}, FA={kv.get('FA', '-')}, L={kv.get('L', '-')}"
    )


def parse_report(line: str) -> tuple[int, NodeSnapshot] | None:
    line = line.strip()
    if not line.startswith("REPORT,"):
        return None

    parts = line.split(",")
    if len(parts) < 4:
        return None

    try:
        node = int(parts[1])
    except ValueError:
        return None

    kv = {}
    for item in parts[3:]:
        if "=" in item:
            k, v = item.split("=", 1)
            kv[k] = v

    snap = NodeSnapshot(
        temp=kv.get("T", "-"),
        humi=kv.get("H", "-"),
        flame_digital=kv.get("FD", "-"),
        flame_analog=kv.get("FA", "-"),
        lock=kv.get("L", "-"),
        reason=parts[2],
        uid=kv.get("UID", "-"),
    )
    return node, snap


class ScreenSimApp(App):
    ONLINE_TIMEOUT_S = 15.0
    CSS = """
    #status {
        height: 3;
        border: solid green;
        padding: 0 1;
    }
    #log {
        height: 8;
        border: solid cyan;
    }
    #dashboard {
        height: 1fr;
        border: solid blue;
        padding: 1 2;
    }
    #cmd {
        border: solid yellow;
    }
    #actions {
        height: 5;
        border: solid magenta;
    }
    #node {
        width: 14;
    }
    .op-btn {
        margin: 0 1;
    }
    #node_quick {
        height: 9;
        layout: grid;
        grid-size: 2;
        grid-gutter: 0 1;
    }
    .node-btn {
        width: 1fr;
    }
    """

    BINDINGS = [
        ("ctrl+c", "quit", "退出"),
    ]

    def __init__(self, config: SerialConfig):
        super().__init__()
        self.config = config
        self.ser: serial.Serial | None = None
        self.stop_event = threading.Event()
        self.reader_thread: threading.Thread | None = None
        self.last_report = "尚未收到 REPORT"
        self.nodes: dict[int, NodeSnapshot] = {}
        self.node_last_seen: dict[int, float] = {}
        self.selected_node = 1

    def compose(self) -> ComposeResult:
        yield Header(show_clock=True)
        with Container():
            yield Static("", id="status")
            with Container(id="actions"):
                yield Input(value="1", placeholder="节点ID", id="node")
                yield Button("GET", id="btn_get", classes="op-btn", variant="primary")
                yield Button("LOCK", id="btn_lock", classes="op-btn", variant="warning")
                yield Button("UNLOCK", id="btn_unlock", classes="op-btn", variant="success")
                yield Button("PING", id="btn_ping", classes="op-btn")
            with Container(id="node_quick"):
                yield Button("N1 LOCK", id="btn_n1_lock", classes="node-btn")
                yield Button("N1 UNLOCK", id="btn_n1_unlock", classes="node-btn")
                yield Button("N2 LOCK", id="btn_n2_lock", classes="node-btn")
                yield Button("N2 UNLOCK", id="btn_n2_unlock", classes="node-btn")
                yield Button("N3 LOCK", id="btn_n3_lock", classes="node-btn")
                yield Button("N3 UNLOCK", id="btn_n3_unlock", classes="node-btn")
                yield Button("N4 LOCK", id="btn_n4_lock", classes="node-btn")
                yield Button("N4 UNLOCK", id="btn_n4_unlock", classes="node-btn")
            yield Static("", id="dashboard")
            yield RichLog(id="log", wrap=True, highlight=True, markup=True, max_lines=120)
            yield Input(
                placeholder="输入: /get 1 | /lock 1 1 | /raw GET,1 | /help",
                id="cmd",
            )
        yield Footer()

    def on_mount(self) -> None:
        self._set_status(f"连接中: {self.config.port} @ {self.config.baud}")
        self._open_serial()
        self.query_one("#cmd", Input).focus()
        self._refresh_dashboard()
        self._refresh_quick_visibility()
        self.set_interval(1.0, self._refresh_quick_visibility)
        self._log("[bold green]已启动 TUI 串口屏模拟器[/]")
        self._log("看板显示节点状态，日志只保留关键事件。输入 /help 查看命令。")

    def on_unmount(self) -> None:
        self._shutdown_serial()

    def _open_serial(self) -> None:
        try:
            self.ser = serial.Serial(
                self.config.port,
                self.config.baud,
                timeout=self.config.timeout,
            )
        except Exception as exc:
            self._log(f"[bold red]串口打开失败:[/] {exc}")
            self._set_status(f"串口失败: {self.config.port}")
            return

        self._set_status(f"已连接: {self.config.port} @ {self.config.baud} | {self.last_report}")
        self.reader_thread = threading.Thread(target=self._reader_loop, daemon=True)
        self.reader_thread.start()

    def _shutdown_serial(self) -> None:
        self.stop_event.set()
        if self.reader_thread is not None:
            self.reader_thread.join(timeout=0.5)
        if self.ser is not None and self.ser.is_open:
            try:
                self.ser.close()
            except Exception:
                pass

    def _log(self, text: str) -> None:
        self.query_one("#log", RichLog).write(text)

    def _set_status(self, text: str) -> None:
        self.query_one("#status", Static).update(text)

    def _reader_loop(self) -> None:
        assert self.ser is not None
        while not self.stop_event.is_set():
            try:
                line = self.ser.readline()
            except serial.SerialException as exc:
                self.call_from_thread(self._log, f"[bold red]串口异常:[/] {exc}")
                break

            if not line:
                continue

            text = line.decode(errors="ignore").rstrip()
            if not text:
                continue

            parsed = parse_report(text)
            if parsed is not None:
                node, snap = parsed
                self.nodes[node] = snap
                self.node_last_seen[node] = time.monotonic()
                # Auto-follow latest node report, same behavior as real screen page jump.
                self.selected_node = node
                self.last_report = parse_report_summary(text) or self.last_report
                self.call_from_thread(self._refresh_dashboard)
                self.call_from_thread(self._sync_node_input)
                self.call_from_thread(self._refresh_quick_visibility)
                self.call_from_thread(
                    self._set_status,
                    f"已连接: {self.config.port} @ {self.config.baud} | {self.last_report}",
                )
                continue

            self.call_from_thread(self._log, f"[cyan][RX][/cyan] {text}")
            summary = parse_report_summary(text)
            if summary:
                self.last_report = summary
                self.call_from_thread(
                    self._set_status,
                    f"已连接: {self.config.port} @ {self.config.baud} | {self.last_report}",
                )

    def on_input_submitted(self, event: Input.Submitted) -> None:
        if event.input.id == "node":
            node = self._current_node()
            if node is None:
                self._log("[red]节点ID无效，请输入 1~4[/]")
            else:
                self.selected_node = node
                self._refresh_dashboard()
            return

        if event.input.id != "cmd":
            return

        raw = event.value.strip()
        event.input.value = ""
        if not raw:
            return

        if raw.lower() in {"q", "quit", "exit"}:
            self.exit()
            return

        if raw.lower() == "/help":
            self._log("[yellow]命令:[/] /get <id>, /lock <id> <0|1>, /raw <text>, quit")
            return

        send_text = self._translate_command(raw)
        if send_text is None:
            self._log("[red]命令格式错误，输入 /help 查看用法[/]")
            return

        self._send_line(send_text)

    def on_button_pressed(self, event: Button.Pressed) -> None:
        quick_map = {
            "btn_n1_lock": (1, 1),
            "btn_n1_unlock": (1, 0),
            "btn_n2_lock": (2, 1),
            "btn_n2_unlock": (2, 0),
            "btn_n3_lock": (3, 1),
            "btn_n3_unlock": (3, 0),
            "btn_n4_lock": (4, 1),
            "btn_n4_unlock": (4, 0),
        }
        if event.button.id in quick_map:
            node, value = quick_map[event.button.id]
            self.selected_node = node
            self.query_one("#node", Input).value = str(node)
            self._refresh_dashboard()
            self._send_line(f"LOCK,{node},{value}")
            return

        node = self._current_node()
        if node is None:
            self._log("[red]节点ID无效，请输入 1~4 之类数字[/]")
            return
        self.selected_node = node
        self._refresh_dashboard()

        if event.button.id == "btn_get":
            self._send_line(f"GET,{node}")
            return
        if event.button.id == "btn_lock":
            self._send_line(f"LOCK,{node},1")
            return
        if event.button.id == "btn_unlock":
            self._send_line(f"LOCK,{node},0")
            return
        if event.button.id == "btn_ping":
            self._send_line(f"REQ,{node},PING")
            return

    def _current_node(self) -> int | None:
        raw = self.query_one("#node", Input).value.strip()
        if not raw.isdigit():
            return None
        node = int(raw)
        if node <= 0 or node > 4:
            return None
        return node

    def _refresh_dashboard(self) -> None:
        node = self.selected_node
        snap = self.nodes.get(node, NodeSnapshot())

        lock_text = "LOCKED" if snap.lock == "1" else "UNLOCKED" if snap.lock == "0" else "-"
        fire_text = "ALARM" if snap.flame_digital == "0" else "NORMAL" if snap.flame_digital == "1" else "-"

        body = (
            f"[节点 {node} 实时状态]\n\n"
            f"温度: {snap.temp} C\n"
            f"湿度: {snap.humi} %\n"
            f"火焰数字: {snap.flame_digital} ({fire_text})\n"
            f"火焰模拟: {snap.flame_analog} mV\n"
            f"锁状态: {snap.lock} ({lock_text})\n"
            f"上报原因: {snap.reason}\n"
            f"UID: {snap.uid}\n"
        )
        self.query_one("#dashboard", Static).update(body)

    def _sync_node_input(self) -> None:
        node_input = self.query_one("#node", Input)
        node_input.value = str(self.selected_node)

    def _refresh_quick_visibility(self) -> None:
        now = time.monotonic()
        for node in (1, 2, 3, 4):
            online = (now - self.node_last_seen.get(node, 0.0)) <= self.ONLINE_TIMEOUT_S
            for suffix in ("lock", "unlock"):
                btn = self.query_one(f"#btn_n{node}_{suffix}", Button)
                btn.styles.display = "block" if online else "none"

    def _translate_command(self, cmd: str) -> str | None:
        parts = cmd.split()
        if parts[0].lower() == "/get" and len(parts) == 2:
            return f"GET,{parts[1]}"
        if parts[0].lower() == "/lock" and len(parts) == 3:
            return f"LOCK,{parts[1]},{parts[2]}"
        if parts[0].lower() == "/raw" and len(parts) >= 2:
            return cmd[5:]
        return None

    def _send_line(self, text: str) -> None:
        if self.ser is None or not self.ser.is_open:
            self._log("[red]串口未连接，无法发送[/]")
            return
        try:
            payload = (text + self.config.line_end).encode()
            self.ser.write(payload)
            self.ser.flush()
        except Exception as exc:
            self._log(f"[bold red]发送失败:[/] {exc}")
            return
        self._log(f"[green][TX][/green] {text}")


def main() -> None:
    parser = argparse.ArgumentParser(description="Parking Lock 串口屏 TUI 模拟器")
    parser.add_argument("--port", default="COM16")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--timeout", type=float, default=0.2)
    parser.add_argument("--line-end", default="\\r\\n", choices=["\\r\\n", "\\n", "\\r"])
    args = parser.parse_args()
    line_end = args.line_end.encode("utf-8").decode("unicode_escape")

    config = SerialConfig(
        port=args.port,
        baud=args.baud,
        timeout=args.timeout,
        line_end=line_end,
    )
    app = ScreenSimApp(config)
    app.run()


if __name__ == "__main__":
    main()
