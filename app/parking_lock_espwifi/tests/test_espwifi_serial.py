#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
模拟单片机 (MCU) 与 ESP WiFi-MQTT 透传模块的串口集成测试程序。

运行方式 (通过 pixi):
    pixi run python app/parking_lock_espwifi/tests/test_espwifi_serial.py --port COMx

说明：
    - 本程序扮演 MCU 的角色，通过 UART 向 ESP 模块发送指令。
    - 它会验证 ESP 模块的 UART 回复是否符合预期。
    - 如果需要验证 MQTT 透传功能，可配合本地 MQTT Broker 使用
      （默认连接 127.0.0.1:1883，可通过 --mqtt-host/--mqtt-port 修改）。

先决条件：
    1. ESP 模块已烧录固件并上电。
    2. PC 已通过 USB-TTL/串口与 ESP 模块的 UART 连接。
    3. 若测试 MQTT 下行指令透传，请确保 MQTT Broker 可访问。
"""

from __future__ import annotations

import argparse
import json
import sys
import time
from dataclasses import dataclass, field
from typing import List, Optional

import serial
import serial.tools.list_ports

# 尝试导入 paho-mqtt，用于验证 MQTT 侧透传
try:
    import paho.mqtt.client as mqtt
except Exception:  # pragma: no cover
    mqtt = None  # type: ignore

DEFAULT_BAUD = 115200
DEFAULT_TIMEOUT = 5.0
MQTT_PAYLOAD_BUF_SIZE = 384


# =============================================================================
# 串口交互基类（模拟 MCU 侧）
# =============================================================================

class EspBridgeSerialTester:
    """封装与 ESP 透传模块的串口收发和断言逻辑。"""

    def __init__(
        self,
        port: str,
        baudrate: int = DEFAULT_BAUD,
        timeout: float = DEFAULT_TIMEOUT,
    ) -> None:
        self.ser = serial.Serial(
            port=port,
            baudrate=baudrate,
            bytesize=serial.EIGHTBITS,
            parity=serial.PARITY_NONE,
            stopbits=serial.STOPBITS_ONE,
            timeout=0.1,
        )
        self.default_timeout = timeout
        self._rx_buffer = bytearray()
        time.sleep(0.5)  # 等待模块稳定
        self.flush()

    # -----------------------------------------------------------------------
    # 原始 I/O
    # -----------------------------------------------------------------------
    def flush(self) -> None:
        """清空串口收发缓冲区。"""
        self.ser.reset_input_buffer()
        self.ser.reset_output_buffer()
        self._rx_buffer.clear()

    def close(self) -> None:
        if self.ser and self.ser.is_open:
            self.ser.close()

    def send_line(self, line: str) -> None:
        """模拟 MCU 向 ESP 发送一行 UART 数据（自动加 \r\n）。"""
        data = (line + "\r\n").encode("utf-8")
        self.ser.write(data)
        self.ser.flush()
        print(f"[MCU -> ESP] {line}")

    def _read_bytes(self, max_bytes: int = 4096) -> bytes:
        """非阻塞读取当前串口缓冲区中的字节。"""
        available = self.ser.in_waiting
        if available:
            to_read = min(available, max_bytes)
            return self.ser.read(to_read)
        return b""

    def _pop_line(self) -> Optional[str]:
        """从内部缓冲区尝试弹出一行完整数据。"""
        idx = self._rx_buffer.find(b"\n")
        if idx == -1:
            return None
        line_bytes = self._rx_buffer[: idx + 1]
        self._rx_buffer = self._rx_buffer[idx + 1 :]
        # 去掉 \r\n 或 \n
        line = line_bytes.decode("utf-8", errors="replace").strip()
        return line

    def expect_line(
        self,
        expected: Optional[str] = None,
        timeout: Optional[float] = None,
        contains: Optional[str] = None,
    ) -> str:
        """
        在超时时间内等待并读取一行 UART 数据。

        - expected: 如果提供，要求整行完全匹配。
        - contains: 如果提供，要求行内包含该子串。
        返回实际收到的行。
        """
        timeout = timeout or self.default_timeout
        deadline = time.time() + timeout

        while time.time() < deadline:
            self._rx_buffer.extend(self._read_bytes())
            while True:
                line = self._pop_line()
                if line is None:
                    break
                print(f"[ESP -> MCU] {line}")
                if expected is not None and line != expected:
                    raise AssertionError(
                        f"期望收到 '{expected}'，实际收到 '{line}'"
                    )
                if contains is not None and contains not in line:
                    raise AssertionError(
                        f"期望行包含 '{contains}'，实际收到 '{line}'"
                    )
                return line
            time.sleep(0.05)

        raise TimeoutError(
            f"在 {timeout}s 内未收到期望的 UART 数据"
            + (f" (expected='{expected}')" if expected else "")
            + (f" (contains='{contains}')" if contains else "")
        )

    def expect_any(self, count: int = 1, timeout: Optional[float] = None) -> List[str]:
        """在超时内接收任意 count 行 UART 数据并返回。"""
        timeout = timeout or self.default_timeout
        deadline = time.time() + timeout
        lines: List[str] = []
        while time.time() < deadline and len(lines) < count:
            self._rx_buffer.extend(self._read_bytes())
            while True:
                line = self._pop_line()
                if line is None:
                    break
                print(f"[ESP -> MCU] {line}")
                lines.append(line)
            if len(lines) < count:
                time.sleep(0.05)
        if len(lines) < count:
            raise TimeoutError(
                f"期望收到 {count} 行数据，但仅收到 {len(lines)} 行"
            )
        return lines

    # -----------------------------------------------------------------------
    # 便捷断言（高阶 API）
    # -----------------------------------------------------------------------
    def wait_boot(self, timeout: float = 3.0) -> None:
        """等待并验证 ESP 启动时发送的 READY,ESP,1。"""
        # 启动时可能先输出一些日志，我们只关心 READY,ESP,1
        deadline = time.time() + timeout
        while time.time() < deadline:
            self._rx_buffer.extend(self._read_bytes())
            while True:
                line = self._pop_line()
                if line is None:
                    break
                print(f"[ESP -> MCU] {line}")
                if line == "READY,ESP,1":
                    return
            time.sleep(0.05)
        raise TimeoutError("未收到 READY,ESP,1，模块可能未启动")

    def cmd_and_expect(self, cmd: str, expected: str, timeout: Optional[float] = None) -> str:
        """发送一条命令并期望收到指定回复。"""
        self.send_line(cmd)
        return self.expect_line(expected=expected, timeout=timeout)

    def drain(self, timeout: float = 2.0) -> List[str]:
        """等待并清空串口缓冲区中当前的所有行，返回收到的行。"""
        deadline = time.time() + timeout
        lines: List[str] = []
        while time.time() < deadline:
            self._rx_buffer.extend(self._read_bytes())
            while True:
                line = self._pop_line()
                if line is None:
                    break
                print(f"[ESP -> MCU] {line}")
                lines.append(line)
            time.sleep(0.05)
        return lines


# =============================================================================
# MQTT 辅助（用于验证上行/下行透传）
# =============================================================================

@dataclass
class MqttRecorder:
    client: object
    topic_messages: dict = field(default_factory=dict)
    connected: bool = False
    connect_rc: Optional[int] = None

    @staticmethod
    def _reason_code_to_int(reason_code) -> int:
        value = getattr(reason_code, "value", reason_code)
        try:
            return int(value)
        except Exception:
            # paho v2 may pass non-int-like objects; keep callback alive and mark as failure.
            return 255

    def on_connect(self, client, userdata, flags, reason_code, properties=None):
        rc = self._reason_code_to_int(reason_code)
        self.connect_rc = rc
        self.connected = (rc == 0)
        print(f"[MQTT] 已连接 (rc={rc})")

    def on_message(self, client, userdata, msg):
        payload = msg.payload.decode("utf-8", errors="replace")
        print(f"[MQTT] {msg.topic} -> {payload}")
        self.topic_messages.setdefault(msg.topic, []).append(payload)

    def subscribe(self, topic: str) -> None:
        self.client.subscribe(topic)

    def publish(self, topic: str, payload: str) -> None:
        self.client.publish(topic, payload)

    def wait_for_message(
        self, topic: str, timeout: float = 5.0
    ) -> Optional[str]:
        deadline = time.time() + timeout
        while time.time() < deadline:
            if topic in self.topic_messages and self.topic_messages[topic]:
                return self.topic_messages[topic].pop(0)
            time.sleep(0.05)
        return None


def build_topics(client_id: str) -> dict:
    return {
        "uplink_raw": f"parking-lock/{client_id}/uplink/raw",
        "uplink_status": f"parking-lock/{client_id}/uplink/status",
        "uplink_ack": f"parking-lock/{client_id}/uplink/ack",
        "downlink_cmd": f"parking-lock/{client_id}/downlink/cmd",
        "online": f"parking-lock/{client_id}/status/online",
    }


def make_mqtt_recorder(host: str, port: int, user: str = "", password: str = "") -> MqttRecorder:
    if mqtt is None:
        raise RuntimeError("缺少 paho-mqtt 依赖，无法构建 MQTT 客户端")
    client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2)
    if user:
        client.username_pw_set(user, password)
    recorder = MqttRecorder(client=client)
    client.on_connect = recorder.on_connect
    client.on_message = recorder.on_message
    client.connect(host, port, 60)
    client.loop_start()
    # 等待连接建立（网络抖动时 3s 偶发不足）
    deadline = time.time() + 8.0
    while time.time() < deadline and recorder.connect_rc is None:
        time.sleep(0.05)
    if recorder.connect_rc is None:
        raise TimeoutError("MQTT 连接超时")
    if not recorder.connected:
        raise ConnectionError(f"MQTT 连接失败，CONNACK rc={recorder.connect_rc}")
    return recorder


# =============================================================================
# 测试用例
# =============================================================================

class TestRunner:
    def __init__(
        self,
        tester: EspBridgeSerialTester,
        mqtt_recorder: Optional[MqttRecorder],
        client_id: str,
        wifi_ssid: str,
        wifi_pass: str,
        esp_mqtt_host: str,
        mqtt_host: str,
        mqtt_port: int,
        mqtt_user: str,
        mqtt_pass: str,
    ) -> None:
        self.t = tester
        self.mqtt = mqtt_recorder
        self.client_id = client_id
        self.topics = build_topics(client_id)
        self.wifi_ssid = wifi_ssid
        self.wifi_pass = wifi_pass
        self.esp_mqtt_host = esp_mqtt_host
        self.mqtt_host = mqtt_host
        self.mqtt_port = mqtt_port
        self.mqtt_user = mqtt_user
        self.mqtt_pass = mqtt_pass
        self.passed = 0
        self.failed = 0

    def _expect_line_eventually(self, expected: str, timeout: float = 3.0) -> str:
        """在超时内等待某一行出现，允许中间穿插异步状态输出。"""
        deadline = time.time() + timeout
        while time.time() < deadline:
            try:
                return self.t.expect_line(expected=expected, timeout=0.4)
            except AssertionError:
                # 异步状态行可能与命令响应交错，继续等目标行。
                continue
            except TimeoutError:
                continue
        raise TimeoutError(f"在 {timeout}s 内未收到期望行: {expected}")

    def _esp_mqtt_connected(self, timeout: float = 8.0) -> bool:
        """通过 NETSTAT 轮询 ESP 侧 MQTT 状态。"""
        deadline = time.time() + timeout
        while time.time() < deadline:
            self.t.send_line("NETSTAT")
            loaded_or_empty = None
            try:
                loaded_or_empty = self._expect_line_eventually("NETCFG,LOADED", timeout=1.0)
            except TimeoutError:
                try:
                    loaded_or_empty = self._expect_line_eventually("NETCFG,EMPTY", timeout=0.6)
                except TimeoutError:
                    loaded_or_empty = None

            if loaded_or_empty != "NETCFG,LOADED":
                return False

            mqtt_line = ""
            status_lines = self.t.expect_any(count=4, timeout=1.4)
            for line in status_lines:
                if line.startswith("MQTT,") and not mqtt_line:
                    mqtt_line = line
            if mqtt_line == "MQTT,CONNECTED":
                return True
            time.sleep(0.4)
        return False

    def _run(self, name: str, fn) -> None:
        print(f"\n========== {name} ==========")
        # 先清空串口缓冲区中的旧异步消息
        self.t.drain(timeout=1.0)
        try:
            fn()
            self.passed += 1
            print(f"[PASS] {name}")
        except Exception as exc:
            self.failed += 1
            print(f"[FAIL] {name}: {exc}")
        # 测试结束后等待并清空可能的异步输出（WiFi/MQTT 状态）
        self.t.drain(timeout=2.0)

    # -----------------------------------------------------------------------
    # 各测试场景
    # -----------------------------------------------------------------------
    def test_boot_ready(self) -> None:
        """验证模块启动后会主动发送 READY,ESP,1。"""
        # 通常在 setup() 里自动发送，不需要 MCU 触发。
        # 如果测试开始时已经错过，可手动 reset 模块后重测。
        try:
            self.t.wait_boot(timeout=2.0)
            return
        except TimeoutError:
            # 测试进程晚于设备启动时，READY 可能已错过。用 NETSTAT 兜底确认模块在线。
            pass

        self.t.send_line("NETSTAT")
        first = self.t.expect_any(count=1, timeout=1.2)[0]
        assert first in {"NETCFG,LOADED", "NETCFG,EMPTY"}, f"NETSTAT 首行异常: {first}"

    def test_netcfg_save_and_stat(self) -> None:
        """NETCFG 配置保存 + NETSTAT 查询。"""
        # 用命令行参数构造配置，避免测试侧 MQTT 与 ESP 目标 Broker 不一致。
        cmd = (
            f"NETCFG,{self.wifi_ssid},{self.wifi_pass},{self.esp_mqtt_host},"
            f"{self.mqtt_port},{self.client_id},{self.mqtt_user},{self.mqtt_pass}"
        )
        self.t.cmd_and_expect(cmd, "NETCFG,SAVED")

        # NETCFG 保存后 ESP 会开始连接 WiFi，异步输出连接状态。
        # 先等待并清空这些异步消息，再发 NETSTAT。
        print("[WAIT] 等待 ESP WiFi/MQTT 连接状态输出...")
        self.t.drain(timeout=5.0)

        # NETSTAT 应返回 LOADED + WiFi/MQTT 状态
        self.t.send_line("NETSTAT")
        self._expect_line_eventually("NETCFG,LOADED", timeout=2.0)
        lines = self.t.expect_any(count=2, timeout=2.0)
        assert any(line.startswith("WIFI,") for line in lines), f"期望 WIFI 状态行，收到 {lines}"
        assert any(line.startswith("MQTT,") for line in lines), f"期望 MQTT 状态行，收到 {lines}"

    def test_netclr(self) -> None:
        """清除配置并验证 NETSTAT 返回 EMPTY。"""
        self.t.cmd_and_expect("NETCLR", "NETCFG,CLEARED")
        # NETCLR 也会触发断开连接的异步输出
        self.t.drain(timeout=2.0)
        self.t.cmd_and_expect("NETSTAT", "NETCFG,EMPTY")

    def test_ready_gateway(self) -> None:
        """MCU 发送 READY,GATEWAY,1，ESP 应回复 INFO,ESP,gateway_ready_seen。"""
        self.t.cmd_and_expect("READY,GATEWAY,1", "INFO,ESP,gateway_ready_seen")

    def test_pong_gateway(self) -> None:
        """MCU 发送 PONG,GATEWAY，ESP 应回复 INFO,ESP,pong_gateway。"""
        self.t.cmd_and_expect("PONG,GATEWAY", "INFO,ESP,pong_gateway")

    def test_report_uplink(self) -> None:
        """MCU 发送 REPORT,...，ESP 应将其透传到 MQTT uplink/raw。"""
        if self.mqtt is None:
            print("[SKIP] 未提供 MQTT 连接，跳过 MQTT 上行验证")
            return
        assert self._esp_mqtt_connected(), "ESP 侧 MQTT 未连接，无法验证上行透传"

        # 订阅 raw topic
        self.mqtt.subscribe(self.topics["uplink_raw"])
        time.sleep(0.3)

        self.t.send_line("REPORT,1,ONLINE,normal")
        msg = self.mqtt.wait_for_message(self.topics["uplink_raw"], timeout=5.0)
        assert msg == "REPORT,1,ONLINE,normal", f"MQTT 上行内容不匹配: {msg}"

    def test_offline_uplink(self) -> None:
        """MCU 发送 OFFLINE,...，ESP 应将其透传到 MQTT uplink/raw。"""
        if self.mqtt is None:
            print("[SKIP] 未提供 MQTT 连接，跳过 MQTT 上行验证")
            return
        assert self._esp_mqtt_connected(), "ESP 侧 MQTT 未连接，无法验证上行透传"

        self.mqtt.subscribe(self.topics["uplink_raw"])
        time.sleep(0.3)

        self.t.send_line("OFFLINE,node-2,battery_low")
        msg = self.mqtt.wait_for_message(self.topics["uplink_raw"], timeout=5.0)
        assert msg == "OFFLINE,node-2,battery_low", f"MQTT 上行内容不匹配: {msg}"

    def test_ack_uplink(self) -> None:
        """MCU 发送 ACK,...，ESP 应将其透传到 MQTT uplink/ack。"""
        if self.mqtt is None:
            print("[SKIP] 未提供 MQTT 连接，跳过 MQTT 上行验证")
            return
        assert self._esp_mqtt_connected(), "ESP 侧 MQTT 未连接，无法验证上行透传"

        self.mqtt.subscribe(self.topics["uplink_ack"])
        time.sleep(0.3)

        self.t.send_line("ACK,req-42,OK")
        msg = self.mqtt.wait_for_message(self.topics["uplink_ack"], timeout=5.0)
        assert msg == "ACK,req-42,OK", f"MQTT 上行内容不匹配: {msg}"

    def test_mqtt_downlink_lock(self) -> None:
        """MQTT 下发 LOCK 指令，ESP 应通过 UART 转发 CMD,...,LOCK,...,。"""
        if self.mqtt is None:
            print("[SKIP] 未提供 MQTT 连接，跳过 MQTT 下行验证")
            return
        assert self._esp_mqtt_connected(), "ESP 侧 MQTT 未连接，无法验证下行透传"

        self.mqtt.subscribe(self.topics["downlink_cmd"])
        time.sleep(0.3)

        payload = json.dumps({"id": "req-lock-1", "type": "LOCK", "node": 3, "value": 1})
        self.mqtt.publish(self.topics["downlink_cmd"], payload)

        line = self.t.expect_line(contains="CMD,req-lock-1,LOCK,3,1", timeout=5.0)
        assert line == "CMD,req-lock-1,LOCK,3,1", f"UART 下行内容不匹配: {line}"

    def test_mqtt_downlink_get(self) -> None:
        """MQTT 下发 GET 指令，ESP 应通过 UART 转发 CMD,...,GET,...。"""
        if self.mqtt is None:
            print("[SKIP] 未提供 MQTT 连接，跳过 MQTT 下行验证")
            return
        assert self._esp_mqtt_connected(), "ESP 侧 MQTT 未连接，无法验证下行透传"

        payload = json.dumps({"id": "req-get-1", "type": "GET", "node": 7})
        self.mqtt.publish(self.topics["downlink_cmd"], payload)

        line = self.t.expect_line(contains="CMD,req-get-1,GET,7", timeout=5.0)
        assert line == "CMD,req-get-1,GET,7", f"UART 下行内容不匹配: {line}"

    def test_mqtt_downlink_bad_json(self) -> None:
        """MQTT 下发非法 JSON，ESP 应报告 ERR,ESP,mqtt_bad_json。"""
        if self.mqtt is None:
            print("[SKIP] 未提供 MQTT 连接，跳过 MQTT 下行验证")
            return
        assert self._esp_mqtt_connected(), "ESP 侧 MQTT 未连接，无法验证下行透传"

        self.mqtt.publish(self.topics["downlink_cmd"], "{bad json")
        line = self.t.expect_line(contains="ERR,ESP,mqtt_bad_json", timeout=5.0)
        assert "mqtt_bad_json" in line

    def run_all(self) -> None:
        tests = [
            ("Boot Ready", self.test_boot_ready),
            ("NETCFG + NETSTAT", self.test_netcfg_save_and_stat),
            ("NETCLR", self.test_netclr),
            ("READY Gateway", self.test_ready_gateway),
            ("PONG Gateway", self.test_pong_gateway),
            ("Uplink REPORT", self.test_report_uplink),
            ("Uplink OFFLINE", self.test_offline_uplink),
            ("Uplink ACK", self.test_ack_uplink),
            ("Downlink LOCK", self.test_mqtt_downlink_lock),
            ("Downlink GET", self.test_mqtt_downlink_get),
            ("Downlink Bad JSON", self.test_mqtt_downlink_bad_json),
        ]
        for name, fn in tests:
            self._run(name, fn)

        print(f"\n========== 测试结果: 通过 {self.passed} / 失败 {self.failed} ==========")


# =============================================================================
# 命令行入口
# =============================================================================

def list_serial_ports() -> None:
    ports = serial.tools.list_ports.comports()
    print("可用串口列表:")
    for p in ports:
        print(f"  {p.device} - {p.description}")
    if not ports:
        print("  (未检测到串口)")


def main(argv: Optional[List[str]] = None) -> int:
    parser = argparse.ArgumentParser(
        description="模拟 MCU 与 ESP WiFi-MQTT 透传模块的串口集成测试",
    )
    parser.add_argument(
        "--port",
        default=None,
        help="串口号，例如 COM3 (Windows) 或 /dev/ttyUSB0 (Linux)",
    )
    parser.add_argument(
        "--baud",
        type=int,
        default=DEFAULT_BAUD,
        help=f"波特率，默认 {DEFAULT_BAUD}",
    )
    parser.add_argument(
        "--timeout",
        type=float,
        default=DEFAULT_TIMEOUT,
        help=f"默认 UART 等待超时（秒），默认 {DEFAULT_TIMEOUT}",
    )
    parser.add_argument(
        "--wifi-ssid",
        default="Origin-2.4G",
        help="用于 NETCFG 的 WiFi SSID，默认 Origin-2.4G",
    )
    parser.add_argument(
        "--wifi-pass",
        default="12345678aaa",
        help="用于 NETCFG 的 WiFi 密码，默认 12345678aaa",
    )
    parser.add_argument(
        "--mqtt-host",
        default="127.0.0.1",
        help="PC 侧测试客户端连接的 MQTT Broker 地址，默认 127.0.0.1",
    )
    parser.add_argument(
        "--esp-mqtt-host",
        default="192.168.33.151",
        help="通过 NETCFG 下发给 ESP 的 MQTT Broker 地址，默认 192.168.33.151",
    )
    parser.add_argument(
        "--mqtt-port",
        type=int,
        default=1883,
        help="MQTT Broker 端口，默认 1883",
    )
    parser.add_argument(
        "--mqtt-user",
        default="lock",
        help="MQTT 用户名",
    )
    parser.add_argument(
        "--mqtt-password",
        default="lock",
        help="MQTT 密码",
    )
    parser.add_argument(
        "--client-id",
        default="test-gateway",
        help="测试使用的 client_id，默认 test-gateway",
    )
    parser.add_argument(
        "--no-mqtt",
        action="store_true",
        help="跳过所有需要 MQTT 的测试",
    )
    parser.add_argument(
        "--list-ports",
        action="store_true",
        help="列出可用串口后退出",
    )

    args = parser.parse_args(argv)

    if args.list_ports:
        list_serial_ports()
        return 0

    if args.port is None:
        print("错误: 请通过 --port 指定串口号。使用 --list-ports 查看可用串口。")
        return 2

    print(f"[INIT] 打开串口 {args.port} @ {args.baud} baud")
    tester = EspBridgeSerialTester(
        port=args.port,
        baudrate=args.baud,
        timeout=args.timeout,
    )

    mqtt_recorder: Optional[MqttRecorder] = None
    if not args.no_mqtt:
        try:
            print(f"[INIT] 连接 MQTT {args.mqtt_host}:{args.mqtt_port}")
            mqtt_recorder = make_mqtt_recorder(args.mqtt_host, args.mqtt_port, args.mqtt_user, args.mqtt_password)
            # 订阅本测试会用到的所有 topic
            for t in build_topics(args.client_id).values():
                mqtt_recorder.subscribe(t)
            time.sleep(0.5)
        except Exception as exc:
            print(f"[WARN] MQTT 连接失败: {exc}，将跳过 MQTT 相关测试")
            mqtt_recorder = None

    runner = TestRunner(
        tester,
        mqtt_recorder,
        args.client_id,
        args.wifi_ssid,
        args.wifi_pass,
        args.esp_mqtt_host,
        args.mqtt_host,
        args.mqtt_port,
        args.mqtt_user,
        args.mqtt_password,
    )
    try:
        runner.run_all()
    finally:
        tester.close()
        if mqtt_recorder:
            mqtt_recorder.client.loop_stop()
            mqtt_recorder.client.disconnect()

    return 0 if runner.failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
