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

`--install`, `--uninstall`, `--status`, and `--consent` are **mutually exclusive**.

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