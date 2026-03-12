# vibepods-cli

VibePods CLI

## CLI

```bash
./vibepods-cli connect
./vibepods-cli disconnect
./vibepods-cli status
./vibepods-cli mode nc
./vibepods-cli ca on
```

`disconnect` disconnects connected AirPods (or the device specified with `--mac`).

## Daemon

`vibepods-daemon` keeps a persistent AACP session open and writes a JSON cache file for status consumers such as Waybar.

```bash
./vibepods-daemon
./vibepods-daemon --output "$XDG_RUNTIME_DIR/vibepods/status.json"
./vibepods-daemon --snapshot-interval 300
systemctl --user kill -s USR1 vibepods-daemon.service
```

Default paths:

- state file: `$XDG_RUNTIME_DIR/vibepods/status.json` (fallback: `~/.cache/vibepods/status.json`)

Notes:

- `SIGUSR1` triggers an on-demand status snapshot without restarting the daemon. If you run the daemon under `systemd --user`, prefer `systemctl --user kill -s USR1 vibepods-daemon.service`.
- `--snapshot-interval 0` disables periodic snapshots; the daemon still writes updates from live notifications.
- The JSON cache stays on disk across disconnects and keeps an explicit `connected=false` state so status consumers do not flicker or disappear.
- JSON output keeps the existing `status --json` fields and adds `daemon_running`, `protocol_connected`, `headset_battery`, `updated_at`, `last_refresh_at`, `state_path`, `source`, and `last_error`.
