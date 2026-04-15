from __future__ import annotations

import asyncio
from pathlib import Path

from amqtt.broker import Broker
from amqtt.contexts import BrokerConfig
import yaml


async def _run() -> None:
    repo_root = Path(__file__).resolve().parents[2]
    config_path = repo_root / "tools" / "mqtt" / "local_broker.yaml"
    config_dict = yaml.safe_load(config_path.read_text(encoding="utf-8"))
    config_dict["plugins"]["amqtt.plugins.authentication.FileAuthPlugin"]["password_file"] = str(
        repo_root / "tools" / "mqtt" / "passwords.txt"
    )
    config = BrokerConfig.from_dict(config_dict)
    broker = Broker(config)

    print("[MQTT] 启动本地测试 Broker: mqtt://127.0.0.1:1883")
    print("[MQTT] 用户名/密码: lock / lock")
    print("[MQTT] 按 Ctrl+C 停止")

    await broker.start()
    try:
        await asyncio.Event().wait()
    finally:
        await broker.shutdown()


def main() -> None:
    try:
        asyncio.run(_run())
    except KeyboardInterrupt:
        print("\n[MQTT] 本地 Broker 已停止")


if __name__ == "__main__":
    main()