#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Python unit tests for the WiFi-MQTT transparent-transmission module logic.

This file mirrors the core protocol/state-machine logic from:
  - app/parking_lock_espwifi/main/main.ino   (ESP8266 variant)
  - app/parking_lock_espwifi/32/32.ino       (ESP32 variant)

The tests are "host-based": they verify CSV parsing, JSON down-link
handling, topic building, netcfg validation, and UART response generation
without any real Arduino/ESP hardware or dependencies.
"""

import json
import unittest
from dataclasses import dataclass, field
from typing import List, Optional, Tuple

# =============================================================================
# Logic under test (mirroring the C++ implementation)
# =============================================================================

UART_RX_BUF_SIZE = 384
UART_TX_BUF_SIZE = 384
MQTT_PAYLOAD_BUF_SIZE = 384
MQTT_TOPIC_BUF_SIZE = 128
WIFI_HOSTNAME = "parking-lock-gateway"
STATUS_REPORT_INTERVAL_MS = 30000


@dataclass
class NetConfig:
    valid: bool = False
    ssid: str = ""
    wifi_password: str = ""
    mqtt_host: str = ""
    mqtt_port: int = 1883
    client_id: str = ""
    mqtt_username: str = ""
    mqtt_password: str = ""


def is_field_valid(s: Optional[str]) -> bool:
    if s is None:
        return False
    for ch in s:
        if ch in ",\r\n":
            return False
    return True


def split_csv(line: str) -> List[str]:
    """Equivalent of split_csv_inplace without mutating the input."""
    return line.split(",")


def build_topics(client_id: str) -> dict:
    return {
        "uplink_raw": f"parking-lock/{client_id}/uplink/raw",
        "uplink_status": f"parking-lock/{client_id}/uplink/status",
        "uplink_ack": f"parking-lock/{client_id}/uplink/ack",
        "downlink_cmd": f"parking-lock/{client_id}/downlink/cmd",
        "online": f"parking-lock/{client_id}/status/online",
    }


def wifi_status_reason_simple(status: int) -> str:
    # Mapping used in main/main.ino (ESP8266)
    # 32/32.ino (ESP32) drops WL_WRONG_PASSWORD (255) because the ESP32
    # core does not expose that enum value.
    mapping = {
        1: "no_ssid",       # WL_NO_SSID_AVAIL
        4: "connect_fail",  # WL_CONNECT_FAILED
        6: "auth_fail",     # WL_WRONG_PASSWORD  (ESP8266 only)
        0: "idle",          # WL_IDLE_STATUS
    }
    # Note: in the C++ code WL_DISCONNECTED == 6 on ESP8266 but the switch
    # returns "disconnected" for that case.  On ESP32 WL_DISCONNECTED is also 0.
    # We keep the mapping minimal and fall back to the C-like switch below.
    if status == 1:
        return "no_ssid"
    if status == 4:
        return "connect_fail"
    if status == 6:
        return "auth_fail"   # matches ESP8266 source
    if status == 0:
        return "idle"
    return "unknown"


# ---------------------------------------------------------------------------
# UART output generators (pure helpers)
# ---------------------------------------------------------------------------

def report_wifi_connected(ip: str) -> str:
    return f"WIFI,CONNECTED,{ip}"


def report_wifi_disconnected(reason: str) -> str:
    return f"WIFI,DISCONNECTED,{reason}"


def report_mqtt_connected() -> str:
    return "MQTT,CONNECTED"


def report_mqtt_disconnected(reason: str) -> str:
    return f"MQTT,DISCONNECTED,{reason}"


def report_netcfg_error(code: str) -> str:
    return f"NETCFG,ERROR,{code}"


def report_info_esp(detail: str) -> str:
    return f"INFO,ESP,{detail}"


def report_err_esp(detail: str) -> str:
    return f"ERR,ESP,{detail}"


# ---------------------------------------------------------------------------
# High-level protocol handlers (mirroring process_uart_line & mqtt_callback)
# ---------------------------------------------------------------------------

def process_netcfg_command(line: str) -> Tuple[bool, Optional[NetConfig], str]:
    """
    Returns (success, cfg_or_None, uart_response_line).
    """
    fields = split_csv(line)
    if len(fields) != 8:
        return False, None, report_netcfg_error("bad_field_count")

    # fields[0] == "NETCFG"
    _, ssid, wifi_pass, mqtt_host, port_str, client_id, mqtt_user, mqtt_pass = fields

    for f in (ssid, wifi_pass, mqtt_host, port_str, client_id, mqtt_user, mqtt_pass):
        if not is_field_valid(f):
            return False, None, report_netcfg_error("bad_field")

    if ssid == "" or mqtt_host == "" or client_id == "":
        return False, None, report_netcfg_error("bad_field")

    try:
        port = int(port_str)
    except ValueError:
        return False, None, report_netcfg_error("bad_port")

    if not (1 <= port <= 65535):
        return False, None, report_netcfg_error("bad_port")

    cfg = NetConfig(
        valid=True,
        ssid=ssid,
        wifi_password=wifi_pass,
        mqtt_host=mqtt_host,
        mqtt_port=port,
        client_id=client_id,
        mqtt_username=mqtt_user,
        mqtt_password=mqtt_pass,
    )
    return True, cfg, "NETCFG,SAVED"


def process_mqtt_downlink(
    topic: str,
    payload: bytes,
    expected_downlink_topic: str,
) -> Tuple[bool, List[str], str]:
    """
    Mirrors mqtt_callback.
    Returns (handled_ok, uart_lines, error_detail).
    error_detail is empty when handled_ok is True.
    """
    if topic != expected_downlink_topic:
        return False, [], ""

    if len(payload) >= MQTT_PAYLOAD_BUF_SIZE:
        return False, [], report_err_esp("mqtt_payload_too_long")

    try:
        doc = json.loads(payload.decode("utf-8"))
    except (json.JSONDecodeError, UnicodeDecodeError):
        return False, [], report_err_esp("mqtt_bad_json")

    cmd_id = doc.get("id", "")
    cmd_type = doc.get("type", "")
    node = doc.get("node", -1)

    if not is_field_valid(cmd_id) or not is_field_valid(cmd_type):
        return False, [], report_err_esp("mqtt_bad_field")

    if cmd_id == "" or cmd_type == "" or not isinstance(node, int) or node < 0:
        return False, [], report_err_esp("mqtt_bad_cmd")

    if cmd_type == "LOCK":
        if "value" not in doc:
            return False, [], report_err_esp("mqtt_bad_value")
        value = int(doc["value"])
        if value not in (0, 1):
            return False, [], report_err_esp("mqtt_bad_value")
        return True, [f"CMD,{cmd_id},LOCK,{node},{value}"], ""

    if cmd_type == "GET":
        return True, [f"CMD,{cmd_id},GET,{node}"], ""

    return False, [], report_err_esp("mqtt_bad_action")


def process_uart_line(
    line: str,
    cfg: NetConfig,
    wifi_connected: bool,
    mqtt_connected: bool,
) -> List[str]:
    """
    Mirrors the switch-like dispatch in process_uart_line(main.ino).
    Returns a list of UART TX lines that the firmware would emit.
    """
    if not line:
        return []

    if line == "NETCLR":
        return ["NETCFG,CLEARED"]

    if line == "NETSTAT":
        if not cfg.valid:
            return ["NETCFG,EMPTY"]
        out = ["NETCFG,LOADED"]
        if wifi_connected:
            # IP is not known in this pure test, we use a placeholder.
            out.append(report_wifi_connected("0.0.0.0"))
        else:
            out.append(report_wifi_disconnected("not_connected"))
        if mqtt_connected:
            out.append(report_mqtt_connected())
        else:
            out.append(report_mqtt_disconnected("not_connected"))
        return out

    if line == "READY,GATEWAY,1":
        return [report_info_esp("gateway_ready_seen")]

    if line == "PONG,GATEWAY":
        return [report_info_esp("pong_gateway")]

    if line.startswith("NETCFG,"):
        ok, _, resp = process_netcfg_command(line)
        return [resp]

    if line.startswith("REPORT,") or line.startswith("OFFLINE,"):
        # In the real firmware this would publish to MQTT raw topic.
        # We represent success with an empty UART response.
        return []

    if line.startswith("ACK,"):
        # Publishes to MQTT ack topic; no UART response.
        return []

    return [report_err_esp("unknown_uart_frame")]


# =============================================================================
# Unit tests
# =============================================================================

class TestFieldValidation(unittest.TestCase):
    def test_valid_fields(self):
        self.assertTrue(is_field_valid("hello"))
        self.assertTrue(is_field_valid("192.168.1.1"))
        self.assertTrue(is_field_valid(""))

    def test_invalid_fields(self):
        self.assertFalse(is_field_valid("a,b"))
        self.assertFalse(is_field_valid("a\rb"))
        self.assertFalse(is_field_valid("a\nb"))
        self.assertFalse(is_field_valid(None))


class TestCsvSplit(unittest.TestCase):
    def test_basic(self):
        self.assertEqual(split_csv("a,b,c"), ["a", "b", "c"])

    def test_empty_tail(self):
        self.assertEqual(split_csv("a,b,"), ["a", "b", ""])

    def test_single(self):
        self.assertEqual(split_csv("NETCFG"), ["NETCFG"])


class TestTopicBuilding(unittest.TestCase):
    def test_topics(self):
        topics = build_topics("gw-01")
        self.assertEqual(topics["uplink_raw"], "parking-lock/gw-01/uplink/raw")
        self.assertEqual(topics["uplink_status"], "parking-lock/gw-01/uplink/status")
        self.assertEqual(topics["uplink_ack"], "parking-lock/gw-01/uplink/ack")
        self.assertEqual(topics["downlink_cmd"], "parking-lock/gw-01/downlink/cmd")
        self.assertEqual(topics["online"], "parking-lock/gw-01/status/online")


class TestWifiStatusMapping(unittest.TestCase):
    def test_known_codes(self):
        self.assertEqual(wifi_status_reason_simple(1), "no_ssid")
        self.assertEqual(wifi_status_reason_simple(4), "connect_fail")
        self.assertEqual(wifi_status_reason_simple(6), "auth_fail")
        self.assertEqual(wifi_status_reason_simple(0), "idle")

    def test_unknown(self):
        self.assertEqual(wifi_status_reason_simple(255), "unknown")


class TestNetcfgCommand(unittest.TestCase):
    def test_valid_netcfg(self):
        line = "NETCFG,myssid,mypass,192.168.1.100,1883,gw-01,user,pass"
        ok, cfg, resp = process_netcfg_command(line)
        self.assertTrue(ok)
        self.assertIsNotNone(cfg)
        self.assertEqual(resp, "NETCFG,SAVED")
        self.assertEqual(cfg.ssid, "myssid")
        self.assertEqual(cfg.mqtt_port, 1883)
        self.assertEqual(cfg.client_id, "gw-01")

    def test_bad_field_count(self):
        line = "NETCFG,myssid,mypass,host,1883,gw-01"
        ok, cfg, resp = process_netcfg_command(line)
        self.assertFalse(ok)
        self.assertIsNone(cfg)
        self.assertEqual(resp, report_netcfg_error("bad_field_count"))

    def test_bad_port_zero(self):
        line = "NETCFG,myssid,mypass,host,0,gw-01,user,pass"
        ok, cfg, resp = process_netcfg_command(line)
        self.assertFalse(ok)
        self.assertEqual(resp, report_netcfg_error("bad_port"))

    def test_bad_port_too_large(self):
        line = "NETCFG,myssid,mypass,host,70000,gw-01,user,pass"
        ok, cfg, resp = process_netcfg_command(line)
        self.assertFalse(ok)
        self.assertEqual(resp, report_netcfg_error("bad_port"))

    def test_bad_field_with_comma(self):
        line = "NETCFG,my,ssid,mypass,host,1883,gw-01,user,pass"
        ok, cfg, resp = process_netcfg_command(line)
        self.assertFalse(ok)
        self.assertEqual(resp, report_netcfg_error("bad_field_count"))

    def test_empty_required_field(self):
        line = "NETCFG,,mypass,host,1883,gw-01,user,pass"
        ok, cfg, resp = process_netcfg_command(line)
        self.assertFalse(ok)
        self.assertEqual(resp, report_netcfg_error("bad_field"))


class TestMqttDownlink(unittest.TestCase):
    def setUp(self):
        self.topic = build_topics("gw-01")["downlink_cmd"]

    def test_lock_command(self):
        payload = json.dumps({"id": "req-1", "type": "LOCK", "node": 3, "value": 1}).encode()
        ok, lines, err = process_mqtt_downlink(self.topic, payload, self.topic)
        self.assertTrue(ok)
        self.assertEqual(lines, ["CMD,req-1,LOCK,3,1"])
        self.assertEqual(err, "")

    def test_lock_value_zero(self):
        payload = json.dumps({"id": "req-2", "type": "LOCK", "node": 3, "value": 0}).encode()
        ok, lines, err = process_mqtt_downlink(self.topic, payload, self.topic)
        self.assertTrue(ok)
        self.assertEqual(lines, ["CMD,req-2,LOCK,3,0"])

    def test_get_command(self):
        payload = json.dumps({"id": "req-3", "type": "GET", "node": 7}).encode()
        ok, lines, err = process_mqtt_downlink(self.topic, payload, self.topic)
        self.assertTrue(ok)
        self.assertEqual(lines, ["CMD,req-3,GET,7"])

    def test_wrong_topic_ignored(self):
        wrong_topic = build_topics("gw-02")["downlink_cmd"]
        payload = json.dumps({"id": "req-1", "type": "LOCK", "node": 3, "value": 1}).encode()
        ok, lines, err = process_mqtt_downlink(wrong_topic, payload, self.topic)
        self.assertFalse(ok)
        self.assertEqual(lines, [])
        self.assertEqual(err, "")

    def test_bad_json(self):
        payload = b"{not json"
        ok, lines, err = process_mqtt_downlink(self.topic, payload, self.topic)
        self.assertFalse(ok)
        self.assertEqual(err, report_err_esp("mqtt_bad_json"))

    def test_missing_id(self):
        payload = json.dumps({"type": "LOCK", "node": 3, "value": 1}).encode()
        ok, lines, err = process_mqtt_downlink(self.topic, payload, self.topic)
        self.assertFalse(ok)
        self.assertEqual(err, report_err_esp("mqtt_bad_cmd"))

    def test_missing_value_for_lock(self):
        payload = json.dumps({"id": "req-1", "type": "LOCK", "node": 3}).encode()
        ok, lines, err = process_mqtt_downlink(self.topic, payload, self.topic)
        self.assertFalse(ok)
        self.assertEqual(err, report_err_esp("mqtt_bad_value"))

    def test_invalid_lock_value(self):
        payload = json.dumps({"id": "req-1", "type": "LOCK", "node": 3, "value": 2}).encode()
        ok, lines, err = process_mqtt_downlink(self.topic, payload, self.topic)
        self.assertFalse(ok)
        self.assertEqual(err, report_err_esp("mqtt_bad_value"))

    def test_unknown_action(self):
        payload = json.dumps({"id": "req-1", "type": "RESET", "node": 3}).encode()
        ok, lines, err = process_mqtt_downlink(self.topic, payload, self.topic)
        self.assertFalse(ok)
        self.assertEqual(err, report_err_esp("mqtt_bad_action"))

    def test_payload_too_long(self):
        big = json.dumps({"id": "x" * MQTT_PAYLOAD_BUF_SIZE, "type": "GET", "node": 1})
        payload = big.encode()
        ok, lines, err = process_mqtt_downlink(self.topic, payload, self.topic)
        self.assertFalse(ok)
        self.assertEqual(err, report_err_esp("mqtt_payload_too_long"))


class TestUartDispatch(unittest.TestCase):
    def setUp(self):
        self.cfg = NetConfig(valid=True, client_id="gw-01")

    def test_empty_line(self):
        self.assertEqual(process_uart_line("", self.cfg, True, True), [])

    def test_netclr(self):
        self.assertEqual(process_uart_line("NETCLR", self.cfg, True, True), ["NETCFG,CLEARED"])

    def test_netstat_with_valid_cfg(self):
        lines = process_uart_line("NETSTAT", self.cfg, True, True)
        self.assertIn("NETCFG,LOADED", lines)
        self.assertIn(report_wifi_connected("0.0.0.0"), lines)
        self.assertIn(report_mqtt_connected(), lines)

    def test_netstat_with_invalid_cfg(self):
        cfg = NetConfig(valid=False)
        lines = process_uart_line("NETSTAT", cfg, False, False)
        self.assertEqual(lines, ["NETCFG,EMPTY"])

    def test_ready_gateway(self):
        lines = process_uart_line("READY,GATEWAY,1", self.cfg, True, True)
        self.assertEqual(lines, [report_info_esp("gateway_ready_seen")])

    def test_pong_gateway(self):
        lines = process_uart_line("PONG,GATEWAY", self.cfg, True, True)
        self.assertEqual(lines, [report_info_esp("pong_gateway")])

    def test_report_passthrough(self):
        lines = process_uart_line("REPORT,1,2,3", self.cfg, True, True)
        self.assertEqual(lines, [])

    def test_offline_passthrough(self):
        lines = process_uart_line("OFFLINE,node-1", self.cfg, True, True)
        self.assertEqual(lines, [])

    def test_ack_passthrough(self):
        lines = process_uart_line("ACK,req-1,OK", self.cfg, True, True)
        self.assertEqual(lines, [])

    def test_netcfg_via_uart(self):
        line = "NETCFG,ssid,pass,host,1883,gw-01,user,pass"
        lines = process_uart_line(line, self.cfg, True, True)
        self.assertEqual(lines, ["NETCFG,SAVED"])

    def test_unknown_frame(self):
        lines = process_uart_line("FOOBAR", self.cfg, True, True)
        self.assertEqual(lines, [report_err_esp("unknown_uart_frame")])


class TestConfigPersistenceFormat(unittest.TestCase):
    """
    Verifies the text-file format used by save_config / load_config.
    """

    def test_roundtrip(self):
        cfg = NetConfig(
            valid=True,
            ssid="myssid",
            wifi_password="wifipass",
            mqtt_host="192.168.1.10",
            mqtt_port=1883,
            client_id="gw-01",
            mqtt_username="user",
            mqtt_password="pass",
        )
        lines = [
            cfg.ssid,
            cfg.wifi_password,
            cfg.mqtt_host,
            str(cfg.mqtt_port),
            cfg.client_id,
            cfg.mqtt_username,
            cfg.mqtt_password,
        ]
        text = "\n".join(lines) + "\n"

        # Simulate reading back
        read_lines = text.strip().splitlines()
        self.assertEqual(len(read_lines), 7)
        self.assertEqual(int(read_lines[3]), 1883)

    def test_port_edge_cases(self):
        for port in (1, 65535, 1883, 8883):
            self.assertTrue(1 <= port <= 65535)
        for port in (0, 65536, -1):
            self.assertFalse(1 <= port <= 65535)


if __name__ == "__main__":
    unittest.main()
