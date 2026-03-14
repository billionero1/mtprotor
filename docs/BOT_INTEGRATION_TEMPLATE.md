# Bot Integration Template (HTTP + SSH Fallback)

This guide describes a production-safe way for a remote bot server to issue/revoke MTProxy user access.

## Recommended Model (HTTP Bridge)
- Keep runtime admin API local-only (Unix socket).
- Expose only Bot HTTP Bridge with strict auth:
  - `host`
  - `base_path`
  - `login/password`
  - `api_key`
- Optionally restrict source by `allow-from` CIDR.

Show current profile:
```bash
proxyctl bot api show
```

Rotate credentials:
```bash
proxyctl bot api rotate-password
proxyctl bot api rotate-key
```

Set explicit profile:
```bash
proxyctl bot api set --user botapi --port 9443 --base-path /api-xxxx --allow-from <BOT_SERVER_IP>
```

## HTTP Contract
All responses are JSON.

### Health
```http
GET /<base_path>/health
Authorization: Basic base64(login:password)
X-API-Key: <api_key>
```

### List secrets
```http
GET /<base_path>/secrets
```

### Issue access
```http
POST /<base_path>/issue
Content-Type: application/json

{"label":"user_1001","days":30}
```

### Disable access
```http
POST /<base_path>/disable
Content-Type: application/json

{"secret":"<hex32>"}
```

### Enable access
```http
POST /<base_path>/enable
Content-Type: application/json

{"secret":"<hex32>"}
```

### Revoke access
```http
POST /<base_path>/revoke
Content-Type: application/json

{"secret":"<hex32>"}
```

## Suggested Lifecycle
1. Payment success -> `issue`.
2. Grace/failure -> `disable`.
3. Renewed -> `enable`.
4. Final expiry -> `revoke`.

## SSH Fallback (Forced Command)
If HTTP channel is unavailable, you can use SSH forced-command mode.

Show/rotate SSH profile:
```bash
proxyctl bot ssh show
proxyctl bot ssh rotate-password --user mtproxybot --allow-from <BOT_SERVER_IP>
```

Allowed SSH commands:
- `health`
- `issue <label> [--days N]`
- `disable <secret_hex32>`
- `enable <secret_hex32>`
- `revoke <secret_hex32>`

## Security Checklist
- Keep runtime admin socket local-only.
- Restrict bot access by source CIDR.
- Rotate password/api_key periodically.
- Prefer private tunnel (WireGuard/Tailscale) between bot and proxy server.
- Keep audit logs enabled (journalctl for `mtproxy-fork-bot-api.service` and `dogctl`).
