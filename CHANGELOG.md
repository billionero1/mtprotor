# Changelog

## 0.1.0

- Initial release of `mtprotor`.
- Hot secret reload at runtime (add/remove/enable/disable) without daemon restart.
- In-memory secret store with persistent atomic JSON state.
- Local Unix-socket admin API + CLI.
- Per-secret worker orchestration (`command` and `builtin` modes).
- Install/uninstall scripts and hardened systemd service.
- Unit and integration test coverage for core flows.
- GitHub Actions CI and tagged release pipeline.
