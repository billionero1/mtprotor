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

## Minimal Secure SSH Profile (Login + Password)

If your bot UI needs `ip + username + password`, run:
```bash
sudo mtproxybot-setup --user mtproxybot --password '<STRONG_PASSWORD>' --allow-from <BOT_SERVER_IP>
```

This configures:
- dedicated SSH user
- membership in `mtproxy` group (socket access)
- forced command mode: `/usr/local/bin/proxybot-dispatch`
- disabled shell/forwarding/tunnel
- password auth for this user

Example output:
```json
{"ok":true,"host":"46.149.69.221","port":22,"username":"mtproxybot","password":"...","allow_from":"1.2.3.4"}
```

## Test from bot server
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
