# VPS Rollout and Smoke Test (Ubuntu 24.04)

## 1. Install

```bash
curl -fsSL https://raw.githubusercontent.com/<org>/<repo>/main/scripts/install.sh | sudo bash -s -- --repo <org>/<repo>
```

## 2. Verify service

```bash
sudo systemctl status mtprotor --no-pager
sudo journalctl -u mtprotor -n 100 --no-pager
```

## 3. Open firewall

If UFW is enabled:

```bash
sudo ufw allow 443/tcp
sudo ufw status
```

## 4. Add a secret

```bash
sudo mtprotor secret add 00112233445566778899aabbccddeeff --label main
sudo mtprotor secret list
```

Save returned `id` for further commands.

## 5. Operational checks

```bash
sudo mtprotor status
sudo mtprotor secret list
sudo ls -l /var/lib/mtprotor/secrets.json
```

## 6. Hot reload checks

Disable secret (new connections should be rejected):

```bash
sudo mtprotor secret disable <id>
```

Enable back instantly:

```bash
sudo mtprotor secret enable <id>
```

Remove secret:

```bash
sudo mtprotor secret remove <id>
```

Fast automatic smoke:

```bash
sudo mtprotor-smoke
```

## 7. Persistence checks

```bash
sudo mtprotor secret add <secret_hex> --label restore-test
sudo systemctl restart mtprotor
sudo mtprotor secret list
```

Secret must still be present after restart.

## 8. Logs during secret operations

```bash
sudo journalctl -u mtprotor -f
```

Look for events:

- `secret added`
- `secret enabled` / `secret disabled`
- `secret removed`
- `worker started` / `worker stopped`

## Notes

- `mtprotor` main process is not restarted for secret updates.
- By default active connections are preserved on disable/remove (`drop_on_disable=false`).
- Admin API is local Unix socket: `/run/mtprotor/admin.sock`.
