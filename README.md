# RelapseGtk

RelapseGtk is a Linux GTK script editor for Relapse. It connects to the local
Relapse script socket to submit Luau scripts, shows script output, and can
optionally expose script execution.

## Build and run

Requirements: CMake 3.20 or newer, a C11 compiler, `pkg-config`, GTK 4.16 or
newer, GtkSourceView 5.16 or newer, GIO Unix, JSON-GLib 1.0, and libepoxy.

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
- Script folder: `~/relapse-scripts-tmpdir` by default; configurable under Settings → Scripts

## License

RelapseGtk is licensed under the GNU General Public License, version 3 or later
(`GPL-3.0-or-later`). You can use and modify it privately. If you distribute
the app or a modified version, you must license the distributed work under
the GPL and provide recipients with the corresponding source code. See
[LICENSE](LICENSE) for the full terms.
