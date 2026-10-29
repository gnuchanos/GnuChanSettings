#!/usr/bin/python3
"""Send one low-battery notification per discharge episode."""

import glob
import os
import re
import subprocess
import time
from pathlib import Path


def warning_percent() -> int:
    config_home = Path(os.environ.get(
        "XDG_CONFIG_HOME", str(Path.home() / ".config")
    ))
    config = config_home / "GnuChanSettings" / "system.conf"
    if not config.exists():
        return 10
    text = config.read_text(encoding="utf-8")
    line = next((line for line in text.splitlines()
                 if re.match(r"^\s*BatteryWarningPercent\s*=", line)), None)
    if line is None:
        return 10
    match = re.fullmatch(
        r"\s*BatteryWarningPercent\s*=\s*(\d+)\s*(?:#.*)?", line
    )
    if not match:
        raise ValueError("invalid BatteryWarningPercent setting")
    value = int(match.group(1))
    if not 1 <= value <= 100:
        raise ValueError("BatteryWarningPercent must be between 1 and 100")
    return value


def battery_state() -> tuple[int, bool] | None:
    capacities: list[int] = []
    charging = False
    for battery in glob.glob("/sys/class/power_supply/BAT*"):
        capacity_path = Path(battery) / "capacity"
        status_path = Path(battery) / "status"
        if not capacity_path.is_file():
            continue
        capacities.append(int(capacity_path.read_text(encoding="ascii").strip()))
        if status_path.is_file():
            charging |= status_path.read_text(encoding="ascii").strip().lower() in {
                "charging", "full"
            }
    if not capacities:
        return None
    return min(capacities), charging


def notify(percent: int) -> None:
    subprocess.run(
        ["notify-send", "--urgency=critical", "--app-name=GnuChanPower",
         "Battery low", f"Battery is at {percent}%. Connect the charger."],
        check=True,
    )


def main() -> int:
    warned = False
    while True:
        threshold = warning_percent()
        state = battery_state()
        if state is None:
            warned = False
        else:
            percent, charging = state
            if charging or percent > threshold:
                warned = False
            elif not warned:
                notify(percent)
                warned = True
        time.sleep(60)


if __name__ == "__main__":
    raise SystemExit(main())
