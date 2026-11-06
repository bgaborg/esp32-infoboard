# PlatformIO pre-script: .env -> src/config.h (gitignored).
import os
import sys

Import("env")  # noqa: F821

REQUIRED = ["WIFI_SSID", "WIFI_PASS", "BKK_API_KEY", "STOP_NAME",
            "STOP_A_ID", "STOP_A_TITLE", "STOP_B_ID", "STOP_B_TITLE", "ROUTES"]

root = env.subst("$PROJECT_DIR")  # noqa: F821
path = os.path.join(root, ".env")
if not os.path.exists(path):
    sys.exit("Missing .env - copy .env.example to .env and fill it in")

cfg = {}
for line in open(path, encoding="utf-8"):
    line = line.strip()
    if line and not line.startswith("#") and "=" in line:
        k, v = line.split("=", 1)
        cfg[k.strip()] = v.strip().strip('"').strip("'")
missing = [k for k in REQUIRED if not cfg.get(k)]
if missing:
    sys.exit("Missing in .env: " + ", ".join(missing))


def c(s):
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'


routes = []
for item in cfg["ROUTES"].split(","):
    rid, _, name = item.strip().partition(":")
    if not rid or not name:
        sys.exit("ROUTES format: ROUTE_ID:Label,ROUTE_ID:Label")
    routes.append("{%s, %s}" % (c(rid), c(name)))

out = "\n".join([
    "#pragma once",
    "// Generated from .env by config.py. Do not edit, do not commit.",
    "#define CFG_WIFI_SSID " + c(cfg["WIFI_SSID"]),
    "#define CFG_WIFI_PASS " + c(cfg["WIFI_PASS"]),
    "#define CFG_BKK_KEY " + c(cfg["BKK_API_KEY"]),
    "#define CFG_STOP_NAME " + c(cfg["STOP_NAME"]),
    "#define CFG_STOP_A " + c(cfg["STOP_A_ID"]),
    "#define CFG_STOP_B " + c(cfg["STOP_B_ID"]),
    "#define CFG_STOP_A_TITLE " + c(cfg["STOP_A_TITLE"]),
    "#define CFG_STOP_B_TITLE " + c(cfg["STOP_B_TITLE"]),
    "#define CFG_ROUTES {" + ", ".join(routes) + "}",
    "",
])
dst = os.path.join(root, "src", "config.h")
if not os.path.exists(dst) or open(dst, encoding="utf-8").read() != out:
    open(dst, "w", encoding="utf-8").write(out)
