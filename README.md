# vibepods-cli

100% vibecoded airpods manager. forked from librepods.
No human code is allowed on this repo

## CLI

```bash
./vibepods-cli connect
./vibepods-cli disconnect
./vibepods-cli status
./vibepods-cli mode nc # Mode noise control
./vibepods-cli ca on   # Conversation awarness
```

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
- control socket: `$XDG_RUNTIME_DIR/vibepods/control.sock` (fallback: `~/.cache/vibepods/control.sock`)

When the daemon is running, `vibepods-cli status`, `vibepods-cli mode`, and `vibepods-cli ca` automatically proxy through the daemon control socket instead of opening a second AACP connection.

If you use custom paths, pass matching options to both sides:

```bash
./vibepods-daemon --control-socket /tmp/vibepods.sock
./vibepods-cli status --daemon-socket /tmp/vibepods.sock
```

Notes:

- `SIGUSR1` triggers an on-demand status snapshot without restarting the daemon. If you run the daemon under `systemd --user`, prefer `systemctl --user kill -s USR1 vibepods-daemon.service`.
- `--snapshot-interval 0` disables periodic snapshots; the daemon still writes updates from live notifications.
- The JSON cache stays on disk across disconnects and keeps an explicit `connected=false` state so status consumers do not flicker or disappear.
- JSON output keeps the existing `status --json` fields and adds `daemon_running`, `protocol_connected`, `headset_battery`, `updated_at`, `last_refresh_at`, `state_path`, `control_socket`, `source`, and `last_error`.
