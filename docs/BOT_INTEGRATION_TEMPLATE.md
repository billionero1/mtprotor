# Bot Integration Template (SSH)

This guide describes a minimal, production-safe way for a remote bot server to issue/revoke MTProxy user access.

## Model
- Keep admin API local-only (Unix socket).
- Bot server controls proxy server via SSH.
- Use `proxyctl bot ...` commands (JSON outputs).

## Commands Contract
All commands return JSON.

### Health
```bash
proxyctl bot health
```
Example:
```json
{"service_active":"yes","api_ok":"yes","listener_443":"yes"}
```

### Issue access
```bash
proxyctl bot issue user_1001 --days 30
```
Example:
```json
{"ok":true,"secret":"<hex32>","expires":1770000000,"link":"https://t.me/proxy?..."}
```

### Disable access
```bash
proxyctl bot disable <hex32>
```

### Enable access
```bash
proxyctl bot enable <hex32>
```

### Revoke access
```bash
proxyctl bot revoke <hex32>
```

## Minimal Secure SSH Profile

## 1) Create bot user and add to `mtproxy` group
```bash
sudo useradd -m -s /bin/bash mtproxybot || true
sudo usermod -aG mtproxy mtproxybot
```

## 2) Add bot SSH public key with forced command
`~mtproxybot/.ssh/authorized_keys` entry:
```text
command="/usr/local/bin/proxybot-dispatch",no-agent-forwarding,no-port-forwarding,no-X11-forwarding,no-pty <BOT_PUBLIC_KEY>
```

This allows only approved lifecycle commands (`health`, `issue`, `enable`, `disable`, `revoke`).

## 3) Optional extra hardening in `sshd_config`
```text
Match User mtproxybot
    PermitTTY no
    X11Forwarding no
    AllowTcpForwarding no
    PermitTunnel no
```

Restart SSH after config changes:
```bash
sudo systemctl restart ssh
```

## 4) Test from bot server
```bash
ssh mtproxybot@<proxy-host> 'health'
ssh mtproxybot@<proxy-host> 'issue user_1001 --days 30'
ssh mtproxybot@<proxy-host> 'disable <hex32>'
ssh mtproxybot@<proxy-host> 'revoke <hex32>'
```

## Lifecycle Suggested Flow
1. Payment success -> `issue` (store `secret`, `expires`, `link`).
2. Grace period / failed renewal -> `disable`.
3. Payment restored -> `enable`.
4. Final subscription end -> `revoke`.

## Error Handling
- Non-zero exit code means operation failed.
- JSON error format:
```json
{"ok":false,"error":"..."}
```

