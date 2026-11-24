# RelapseGtk

RelapseGtk is a Linux GTK script editor for Relapse. It connects to the local
Relapse script socket to submit Luau scripts, shows script output, and can
optionally expose script execution.

## Build and run

Requirements: CMake 3.20 or newer, a C11 compiler, `pkg-config`, GTK 4.16 or
newer, GtkSourceView 5.16 or newer, GIO Unix, JSON-GLib 1.0, and libcurl 7.85 or newer.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel
./build/relapsegtk
```

## Maintainer packaging

See [packaging/README.md](packaging/README.md) to configure a fork, create a
release key, build a signed AppImage, and publish an update.

## Local data

- Settings: `$XDG_CONFIG_HOME/relapsegtk/settings.ini` (by default
  `~/.config/relapsegtk/settings.ini`)
- Tab session: `$XDG_STATE_HOME/relapsegtk/session.json` (by default
  `~/.local/state/relapsegtk/session.json`)
- Script folder: `~/relapse-scripts-tmpdir` by default; use the folder button beside
  Scripts to browse another folder, or change it under Settings → Workspace → Script folder.
  The selected folder is also used by the editor's open/save dialogs.
- Profile account cache: `<dataDir>/account.json`
- Shared join target: `$XDG_STATE_HOME/relapsegtk/join-target.ini`

Profile `dataDir` defaults to `$XDG_DATA_HOME/relapse/profiles/<name>` (by default
`~/.local/share/relapse/profiles/<name>`) and can be overridden in
`$XDG_CONFIG_HOME/relapse/profiles/<name>/config.json`.

The account cache is optional JSON with `version: 1`, `user_id`, `username`,
`display_name`, `checked_at` (Unix seconds), `cookie_sha256`, and optional
`avatar_png_base64`. It describes the last successful lookup and is only current
when its cookie hash matches the profile's `.ROBLOSECURITY` value. Login data stays
in the existing `<dataDir>/state/app/data/sober/cookies` file; launching still uses
`--profile <name>`.

## License

RelapseGtk is licensed under the GNU General Public License, version 3 or later
(`GPL-3.0-or-later`). You can use and modify it privately. If you distribute
the app or a modified version, you must license the distributed work under
the GPL and provide recipients with the corresponding source code. See
[LICENSE](LICENSE) for the full terms.
