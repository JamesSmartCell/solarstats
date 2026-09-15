#!/usr/bin/env python3
"""Force sdkconfig log + optimization to the release or debug profile.

ESP-IDF leaves existing sdkconfig values in place when overlays change,
so this rewrites those symbols before CMake runs Kconfig.
"""
from __future__ import annotations

import re
import sys
from pathlib import Path

LEVELS = ("NONE", "ERROR", "WARN", "INFO", "DEBUG", "VERBOSE")
OPT_CHOICES = ("DEBUG", "SIZE", "PERF", "NONE")


def set_symbol(text: str, key: str, enabled: bool) -> str:
    pat = re.compile(rf"^(?:# {re.escape(key)} is not set|{re.escape(key)}=y)\s*$", re.M)
    repl = f"{key}=y" if enabled else f"# {key} is not set"
    if pat.search(text):
        return pat.sub(repl, text, count=1)
    return text + ("\n" if text and not text.endswith("\n") else "") + repl + "\n"


def set_assign(text: str, key: str, value: str) -> str:
    pat = re.compile(rf"^{re.escape(key)}=.*$", re.M)
    repl = f"{key}={value}"
    if pat.search(text):
        return pat.sub(repl, text, count=1)
    return text + ("\n" if text and not text.endswith("\n") else "") + repl + "\n"


def set_choice(text: str, prefix: str, selected: str, numeric: int) -> str:
    for name in LEVELS:
        text = set_symbol(text, f"{prefix}_{name}", name == selected)
    return set_assign(text, prefix, str(numeric))


def set_optimization(text: str, selected: str) -> str:
    for name in OPT_CHOICES:
        text = set_symbol(text, f"CONFIG_COMPILER_OPTIMIZATION_{name}", name == selected)
    return text


def apply(profile: str, path: Path) -> None:
    text = path.read_text(encoding="utf-8")
    if profile == "debug":
        text = set_choice(text, "CONFIG_LOG_DEFAULT_LEVEL", "INFO", 3)
        text = set_choice(text, "CONFIG_LOG_BOOTLOADER_LEVEL", "INFO", 3)
        text = set_assign(text, "CONFIG_LOG_MAXIMUM_LEVEL", "3")
        text = set_symbol(text, "CONFIG_ZBGW_SERIAL_LOG", True)
        text = set_optimization(text, "DEBUG")
        text = set_symbol(text, "CONFIG_OPTIMIZATION_LEVEL_DEBUG", True)
        text = set_symbol(text, "CONFIG_COMPILER_OPTIMIZATION_LEVEL_DEBUG", True)
        text = set_symbol(text, "CONFIG_COMPILER_OPTIMIZATION_DEFAULT", True)
        text = set_symbol(text, "CONFIG_OPTIMIZATION_LEVEL_RELEASE", False)
        text = set_symbol(text, "CONFIG_COMPILER_OPTIMIZATION_LEVEL_RELEASE", False)
    else:
        text = set_choice(text, "CONFIG_LOG_DEFAULT_LEVEL", "NONE", 0)
        text = set_choice(text, "CONFIG_LOG_BOOTLOADER_LEVEL", "NONE", 0)
        text = set_assign(text, "CONFIG_LOG_MAXIMUM_LEVEL", "0")
        text = set_symbol(text, "CONFIG_ZBGW_SERIAL_LOG", False)
        text = set_optimization(text, "PERF")
        text = set_symbol(text, "CONFIG_OPTIMIZATION_LEVEL_DEBUG", False)
        text = set_symbol(text, "CONFIG_COMPILER_OPTIMIZATION_LEVEL_DEBUG", False)
        text = set_symbol(text, "CONFIG_COMPILER_OPTIMIZATION_DEFAULT", False)
        text = set_symbol(text, "CONFIG_OPTIMIZATION_LEVEL_RELEASE", True)
        text = set_symbol(text, "CONFIG_COMPILER_OPTIMIZATION_LEVEL_RELEASE", True)
    text = set_symbol(text, "CONFIG_LOG_MAXIMUM_EQUALS_DEFAULT", True)
    path.write_text(text, encoding="utf-8", newline="\n")


def main() -> int:
    if len(sys.argv) != 3 or sys.argv[1] not in ("release", "debug"):
        print("usage: apply_log_profile.py release|debug <sdkconfig>", file=sys.stderr)
        return 2
    path = Path(sys.argv[2])
    if not path.is_file():
        return 0
    apply(sys.argv[1], path)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
