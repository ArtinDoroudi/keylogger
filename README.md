# ethical-keylogger-demo

## Build

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

Produces `./build/keylogger` (and `./build/log_viewer`).

---

## Flags

| Flag | Description |
|------|-------------|
| `--consent` | Interactive mode. Prints banner, captures keystrokes to cwd, Ctrl+C to stop. |
| `--service` | Unattended foreground mode. Requires `--consent-file`. |
| `--consent-file <path>` | File whose first line is exactly: `I OWN THIS SYSTEM AND CONSENT TO LOGGING` |
| `--log-dir <path>` | Existing, writable directory for `.jsonl` logs (default: cwd). |
| `--session-tag <str>` | Tag added to every JSON event. 1–64 chars, `[A-Za-z0-9_-]` only. |
| `--install` | Register as a system service + auto-start at boot. Requires elevation. |
| `--uninstall` | Stop, disable, and remove service registration. Requires elevation. Idempotent. |
| `--status` | Print whether service is registered/enabled/running. Exit 0 if registered, 1 if not. |
| `--force` | With `--install`, overwrite existing registration. |
| `--help`, `-h` | Print help. |

### Network logging (sender, optional, off by default)

| Flag | Description |
|------|-------------|
| `--net-mode <off\|mirror\|net-only>` | Default `off`. `mirror` writes locally **and** sends. `net-only` sends without writing locally. |
| `--net-endpoint <url>` | `http://`, `https://`, or `tcp://` URL. Required when mode ≠ `off`. |
| `--net-auth-token <str>` | Sent as `Authorization: Bearer <str>`. Required for non-loopback endpoints unless mutual TLS is documented (not in this build). |
| `--net-insecure-local` | Allow missing token **only** when endpoint host is loopback (`127.0.0.0/8`, `::1`, `localhost`). Prints a warning. |
| `--net-ca-file <path>` | PEM bundle for HTTPS verification. Default: system CA paths. |
| `--net-timeout-ms <n>` | Connect/send timeout. Default `5000`. |
| `--net-retry-max <n>` | Max retries per batch before dropping. Default `3`. In `mirror` mode the local file remains complete regardless. |
| `--net-batch-ms <n>` | Coalesce events for up to `n` ms before POSTing. Default `0` (send immediately). |

### Remote viewer (subcommand)

| Flag | Description |
|------|-------------|
| `--viewer` | Run the receive/stream/serve viewer instead of logging. |
| `--listen <host:port>` | Bind address. Default `127.0.0.1:8765`. Refuses non-loopback / wildcard binds without `--listen-public`. |
| `--listen-public` | Explicit acknowledgment that the viewer is exposed on a LAN/WAN. Requires `--viewer-token`. |
| `--viewer-token <str>` | Required for non-loopback binds. Min 16 chars; alphanumerics and `-_.+/=` only. |
| `--tls-cert <path>` / `--tls-key <path>` | Optional HTTPS for the viewer. If set, **all** viewer endpoints require TLS. |
| `--storage-dir <path>` | Optional: append received events to a daily `.jsonl` (`viewer-YYYY-MM-DD.jsonl`). |
| `--max-clients <n>` | Cap concurrent SSE clients. Default `32`. |

`--install`, `--uninstall`, `--status`, and `--consent` are **mutually exclusive**. `--viewer` is its own subcommand and cannot be combined with logger flags.

Exit codes: `0` success, `1` runtime failure or "not registered" for `--status`, `2` validation/privilege/usage error.

---

## Scenarios

### 1. Run interactively (foreground, with banner)

```bash
./build/keylogger --consent
```

Press Ctrl+C to stop. Log written to cwd.

### 2. Run as a foreground service (no banner, no TTY needed)

```bash
echo "I OWN THIS SYSTEM AND CONSENT TO LOGGING" > /tmp/consent.txt
mkdir -p /tmp/keylogs

./build/keylogger --service \
    --consent-file /tmp/consent.txt \
    --log-dir /tmp/keylogs \
    --session-tag lab-01
```

### 3. Install as a managed system service (auto-start at boot)

See platform-specific sections below.

### 4. Check service status (no elevation needed)

```bash
./build/keylogger --status
```

Exit 0 if registered, 1 if not.

### 5. Reinstall with different settings

```bash
sudo ./build/keylogger --install --force \
    --consent-file /etc/keylogger/consent.txt \
    --log-dir /var/log/keylogger \
    --session-tag updated-tag
```

### 6. Uninstall

```bash
sudo ./build/keylogger --uninstall
```

Consent file and log directory are **never** deleted.

---

## Linux (systemd)

### Setup

```bash
sudo mkdir -p /etc/keylogger /var/log/keylogger
echo "I OWN THIS SYSTEM AND CONSENT TO LOGGING" | sudo tee /etc/keylogger/consent.txt
```

### Install

```bash
sudo ./build/keylogger --install \
    --consent-file /etc/keylogger/consent.txt \
    --log-dir /var/log/keylogger \
    --session-tag honeypot-web-01
```

Writes `/etc/systemd/system/keylogger.service`, then runs:
- `systemctl daemon-reload`
- `systemctl enable keylogger.service`
- `systemctl start keylogger.service`

### Status

```bash
./build/keylogger --status
# or directly:
systemctl status keylogger.service
journalctl -u keylogger.service -f
```

### Uninstall

```bash
sudo ./build/keylogger --uninstall
```

Runs `systemctl stop`, `systemctl disable`, removes the unit file, then `systemctl daemon-reload`.

---

## macOS (launchd)

### Setup

```bash
sudo mkdir -p /etc/keylogger /var/log/keylogger
echo "I OWN THIS SYSTEM AND CONSENT TO LOGGING" | sudo tee /etc/keylogger/consent.txt
```

### Install

```bash
sudo ./build/keylogger --install \
    --consent-file /etc/keylogger/consent.txt \
    --log-dir /var/log/keylogger \
    --session-tag macbook-lab-02
```

Writes `/Library/LaunchDaemons/com.ethical-keylogger.plist` and runs `launchctl load -w`.

Daemon stdout/stderr go to:
- `/var/log/keylogger/launchd-stdout.log`
- `/var/log/keylogger/launchd-stderr.log`

> **Required permission:** Grant the keylogger binary **Input Monitoring** (and possibly **Accessibility**) access in **System Settings → Privacy & Security**. Without it the service runs but captures nothing.

### Status

```bash
./build/keylogger --status
# or directly:
sudo launchctl list com.ethical-keylogger
tail -f /var/log/keylogger/launchd-stderr.log
```

### Uninstall

```bash
sudo ./build/keylogger --uninstall
```

Runs `launchctl unload` and removes the plist.

---

## Windows (Windows Service via `sc.exe`)

Run all install/uninstall commands from an **elevated** PowerShell or `cmd.exe`.

### Setup

```powershell
mkdir C:\ProgramData\keylogger
mkdir C:\ProgramData\keylogger\logs
"I OWN THIS SYSTEM AND CONSENT TO LOGGING" | Out-File -Encoding ASCII C:\ProgramData\keylogger\consent.txt
```

### Install

```powershell
.\build\keylogger.exe --install `
    --consent-file C:\ProgramData\keylogger\consent.txt `
    --log-dir C:\ProgramData\keylogger\logs `
    --session-tag windows-lab-03
```

Registers a service named `EthicalKeylogger` with `start= auto`, then starts it.

### Status

```powershell
.\build\keylogger.exe --status
# or directly:
sc query EthicalKeylogger
```

### Uninstall

```powershell
.\build\keylogger.exe --uninstall
```

Runs `sc stop EthicalKeylogger` and `sc delete EthicalKeylogger`.

---

## Expected errors

| Command | Result |
|---------|--------|
| `./keylogger --install …` (no sudo) | exit 2, `error: --install requires root privileges. Run with sudo.` |
| `./keylogger --uninstall` (no sudo) | exit 2, `error: --uninstall requires root privileges. Run with sudo.` |
| `./keylogger --install --uninstall` | exit 2, mutually-exclusive error |
| `./keylogger --consent --status` | exit 2, mutually-exclusive error |
| `./keylogger --force --consent` | exit 2, `--force is only valid with --install` |
| `sudo ./keylogger --install …` (already installed) | exit 2, prompts to use `--uninstall` or `--force` |
| `sudo ./keylogger --uninstall` (nothing registered) | exit 0, `warning: no service is registered` |
| `./keylogger --status` (nothing registered) | exit 1, `Service: not registered` |

---

## Reboot persistence test

```bash
sudo ./build/keylogger --install --consent-file … --log-dir …
sudo reboot
# after reboot:
./build/keylogger --status        # expect: registered + running
ls /var/log/keylogger/            # expect: new .jsonl files
```

---

## Log format

Newline-delimited JSON (`.jsonl`). Filename encodes start time, duration, OS:

```
2026-04-28_14-30-00-0h05m12s-linux.jsonl
```

Every event includes `session_tag` (string or `null`).

---

## Network logging (optional)

> **Network features are disabled by default.** Enabling any `--net-*` flag is opt-in and **must** be coupled with explicit authentication for non-loopback destinations. This tool is intended for systems the operator owns (labs, honeypots, controlled test networks). It does not perform cloud telemetry, mDNS advertisement, UPnP, automatic discovery, or any phone-home behavior.

### Modes

- `off` (default) — no sockets are opened.
- `mirror` — writes the local `.jsonl` **and** sends each event to the network endpoint. Network failures never affect the local log.
- `net-only` — sends only; no local file is written. Requires `--net-endpoint`.

### Wire format

Each event is a single JSON object identical to the local logger schema (`ts`, `vk`, `key`, `down`, `os`, `session_tag`, plus `event` for control messages like `service_start` / `session_start`).

- For batches, the body is **NDJSON**: one event per line, terminated by `\n`. The `Content-Type` is `application/x-ndjson`.
- For unbatched single events, the body is one JSON object and the `Content-Type` is `application/json`.
- For `tcp://` endpoints, framing is **one JSON object per line** (`\n` terminator). No length prefix.

### Authentication

When configured, the sender adds `Authorization: Bearer <token>` to every HTTP/HTTPS request. The viewer enforces the same header.

- Non-loopback endpoint **without** `--net-auth-token` → exits with code 2 at startup.
- Loopback endpoint (`127.0.0.0/8`, `::1`, `localhost`) without a token → still rejected unless `--net-insecure-local` is passed (with a stderr warning).
- The token is validated as `[A-Za-z0-9._+/=-]+`; the same alphabet applies to `--viewer-token`.
- Tokens are **never** logged to the local file or the viewer's storage directory.

### Example: send with `curl`

```bash
TOKEN="$(openssl rand -hex 32)"
# Single event
curl -X POST http://127.0.0.1:8765/ingest \
     -H "Authorization: Bearer $TOKEN" \
     -H "Content-Type: application/json" \
     -d '{"ts":"2026-04-28T14:00:00.000Z","vk":65,"key":"a","down":true,"os":"linux","session_tag":"lab-01"}'

# NDJSON batch
printf '{"event":"hello"}\n{"event":"world"}\n' | \
  curl -X POST http://127.0.0.1:8765/ingest \
       -H "Authorization: Bearer $TOKEN" \
       -H "Content-Type: application/x-ndjson" \
       --data-binary @-
```

### Example: keylogger → collector (mirror mode)

```bash
sudo ./build/keylogger --service \
    --consent-file /etc/keylogger/consent.txt \
    --log-dir /var/log/keylogger \
    --session-tag honeypot-web-01 \
    --net-mode mirror \
    --net-endpoint https://collector.lab.internal/ingest \
    --net-auth-token "$(cat /root/keylog-token)" \
    --net-ca-file /etc/keylogger/lab-ca.pem
```

The `.jsonl` in `/var/log/keylogger` remains authoritative even if the collector is unreachable.

### TLS

`https://` endpoints require an OpenSSL-enabled build (auto-detected at CMake configure time). If the build does not have TLS support, both `https://` and `--net-ca-file` are rejected at startup with a clear message.

---

## Remote viewer

The viewer is a small HTTP server that ingests NDJSON events and streams them to live consumers via Server-Sent Events (SSE). It runs as a subcommand of the same binary: `keylogger --viewer ...`.

> **Security defaults.** The viewer binds to `127.0.0.1:8765` by default. Exposing it on a LAN/WAN requires you to pass `--listen-public` *and* `--viewer-token` simultaneously. There are no anonymous open listeners. **Do not** expose the viewer to the public internet without TLS; the token is otherwise sent in clear text.

### Endpoints

| Method | Path | Description |
|--------|------|-------------|
| `POST` | `/ingest` | Accepts a single JSON object (`Content-Type: application/json`) or an NDJSON batch (`application/x-ndjson`). Validates `Authorization: Bearer <token>`. Returns `200 {"accepted":N}`. |
| `GET` | `/events` | Server-Sent Events stream of every ingested event, in order. Each frame is `data: <json>\n\n`. Token-protected unless on loopback with no token configured. |
| `GET` | `/health` | Returns `{"ok":true,"clients":N}`. Auth is **bypassed only for loopback peers**; non-loopback peers must present the token. |

(WebSocket may be added later as an alternative to SSE; SSE is the supported primary today.)

### Run a local viewer (loopback only, no auth required)

```bash
./build/keylogger --viewer
# [viewer] listening on 127.0.0.1:8765 tls=off storage=(none)
```

### Run a viewer on the LAN

```bash
TOKEN="$(openssl rand -hex 32)"
./build/keylogger --viewer \
    --listen 0.0.0.0:8765 \
    --listen-public \
    --viewer-token "$TOKEN" \
    --storage-dir /var/log/keylog-collector \
    --max-clients 8
```

### Run a viewer with TLS

```bash
./build/keylogger --viewer \
    --listen 0.0.0.0:8765 \
    --listen-public \
    --viewer-token "$TOKEN" \
    --tls-cert /etc/keylogger/server.crt \
    --tls-key  /etc/keylogger/server.key
```

### Consuming the SSE stream

From the shell:

```bash
curl -N -H "Authorization: Bearer $TOKEN" http://127.0.0.1:8765/events
```

From a browser (loopback, no auth):

```html
<script>
  const es = new EventSource("http://127.0.0.1:8765/events");
  es.onmessage = (e) => console.log("event:", JSON.parse(e.data));
</script>
```

When the viewer is token-protected, browser consumption requires either passing the token via a cookie/header proxy or using a tool like `curl`/`websocat`/`httpie` that allows custom headers.

### Storage

When `--storage-dir <path>` is set, the viewer appends each accepted event to `viewer-YYYY-MM-DD.jsonl` in that directory (UTC date). The schema is identical to the keylogger's local logs.

---

## Service-mode integration

Both `--consent` (interactive) and `--service` (unattended) accept the `--net-*` flags. Network mode is **never** enabled implicitly — you must pass `--net-mode mirror` (or `net-only`) on the command line.

### Systemd / launchd / `sc.exe`

The `--install` command does **not** automatically include `--net-*` arguments. If you want the installed unit to ship events to a collector, edit the generated unit/plist and add the same `--net-*` flags to `ExecStart` (systemd), `ProgramArguments` (launchd), or `binPath=` (Windows). A future revision may add an env-file flag for credentials; **today, secrets must not be committed into a unit file readable by non-root users**. If you place the token directly in the unit file, ensure the file mode is `0600` and owned by root, or use a wrapper script that reads from a root-owned env file. Operator responsibility — there is no automatic credential management.

---

## Test plan / sanity checks

| What to test | Command | Expected |
|--------------|---------|----------|
| Default = no network | `./build/keylogger --consent` | Banner says "No network transmission". No outbound TCP (verify with `tcpdump`). |
| Invalid: net-only with no endpoint | `./build/keylogger --consent --net-mode net-only` | Exit 2, error message. |
| Auth: non-loopback, no token | `./build/keylogger --consent --net-mode mirror --net-endpoint http://10.0.0.5/ingest` | Exit 2, error message. |
| Bad scheme | `--net-endpoint ftp://example.com/x` | Exit 2, "unsupported URL scheme". |
| Public viewer w/o ack | `./build/keylogger --viewer --listen 0.0.0.0:8765` | Exit 2, requires `--listen-public`. |
| Public viewer w/o token | `--viewer --listen 0.0.0.0:8765 --listen-public` | Exit 2, requires `--viewer-token`. |
| Token too short | `--viewer-token short` (with public bind) | Exit 2, min length 16. |
| Mirror end-to-end | viewer on `127.0.0.1:18765` + keylogger `--net-mode mirror --net-endpoint http://127.0.0.1:18765/ingest` | Local `.jsonl` complete; SSE stream shows the same events. |
| SIGTERM finalization | `kill -TERM` the keylogger in service mode | Local file is renamed to its final `<start>-<duration>-<os>.jsonl`. |