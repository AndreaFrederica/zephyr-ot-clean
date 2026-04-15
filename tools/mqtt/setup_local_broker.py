from __future__ import annotations

from pathlib import Path

from passlib.apps import custom_app_context as pwd_context


def main() -> None:
    repo_root = Path(__file__).resolve().parents[2]
    password_file = repo_root / "tools" / "mqtt" / "passwords.txt"
    password_file.parent.mkdir(parents=True, exist_ok=True)

    users = {
        "lock": "lock",
        "user": "user",
    }
    lines = [f"{username}:{pwd_context.hash(password)}" for username, password in users.items()]
    password_file.write_text("\n".join(lines) + "\n", encoding="utf-8")
    print(f"[MQTT] 写入本地账号文件: {password_file}")
    print("[MQTT] 可用账号: lock/lock, user/user")


if __name__ == "__main__":
    main()