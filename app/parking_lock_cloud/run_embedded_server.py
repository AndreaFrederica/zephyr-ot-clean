from __future__ import annotations

import argparse
import os
import sys
from pathlib import Path

import uvicorn


def ensure_repo_root_on_pythonpath() -> Path:
    repo_root = Path(__file__).resolve().parents[2]
    repo_root_str = str(repo_root)
    if repo_root_str not in sys.path:
        sys.path.insert(0, repo_root_str)

    existing = os.environ.get("PYTHONPATH", "")
    parts = [part for part in existing.split(os.pathsep) if part]
    if repo_root_str not in parts:
        os.environ["PYTHONPATH"] = os.pathsep.join([repo_root_str, *parts])

    return repo_root


def main() -> None:
    repo_root = ensure_repo_root_on_pythonpath()

    parser = argparse.ArgumentParser(
        description="Run parking_lock_cloud with the embedded MQTT broker enabled."
    )
    parser.add_argument("--host", default="0.0.0.0", help="HTTP bind host")
    parser.add_argument("--port", type=int, default=8000, help="HTTP bind port")
    parser.add_argument("--mqtt-host", default="127.0.0.1", help="MQTT broker host used by the app")
    parser.add_argument("--mqtt-port", type=int, default=1883, help="MQTT broker port used by the app")
    parser.add_argument("--mqtt-user", default="", help="MQTT username")
    parser.add_argument("--mqtt-password", default="", help="MQTT password")
    parser.add_argument(
        "--embed-host",
        default=None,
        help="Embedded broker bind host. Defaults to --mqtt-host.",
    )
    parser.add_argument(
        "--embed-port",
        type=int,
        default=None,
        help="Embedded broker bind port. Defaults to --mqtt-port.",
    )
    parser.add_argument("--reload", action="store_true", help="Enable uvicorn reload")
    args = parser.parse_args()

    os.environ["PARKING_LOCK_EMBED_BROKER"] = "1"
    os.environ["PARKING_LOCK_MQTT_BROKER"] = args.mqtt_host
    os.environ["PARKING_LOCK_MQTT_PORT"] = str(args.mqtt_port)
    os.environ["PARKING_LOCK_MQTT_USER"] = args.mqtt_user
    os.environ["PARKING_LOCK_MQTT_PASSWORD"] = args.mqtt_password
    os.environ["PARKING_LOCK_EMBED_HOST"] = args.embed_host or args.mqtt_host
    os.environ["PARKING_LOCK_EMBED_PORT"] = str(args.embed_port or args.mqtt_port)

    uvicorn.run(
        "app.parking_lock_cloud.server:app",
        host=args.host,
        port=args.port,
        reload=args.reload,
        reload_dirs=[str(repo_root)],
    )


if __name__ == "__main__":
    main()
