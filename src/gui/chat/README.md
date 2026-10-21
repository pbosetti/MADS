# mads-chat

`mads-chat` is a small cross-platform GUI tool for inspecting and publishing messages on a MADS broker. It is built with Dear ImGui and the MADS C++ library and is mainly intended for debugging agent communication.

The window is split into two panes:

- `Publish`: choose a topic, edit a JSON payload with syntax highlighting, validate it, and publish it to the broker
- `Receive`: inspect the latest JSON received per topic, track elapsed time since the last message, and clear the current topic view

The top connection section lets you configure the host, subscribe and publish ports, subscription topics, and optional CURVE key files. The footer shows the detected MADS version and install prefix.


## Build

Built as part of MADS when `MADS_ENABLE_GUI=ON` (the default; see the top-level
`COMPILE.md`). The GUI dependencies (GLFW, Dear ImGui, `portable-file-dialogs`, `ImGuiColorTextEdit`) are shared with the
other GUI app under `src/gui` and fetched at configure time.

```bash
cmake -Bbuild -GNinja
cmake --build build --target mads-chat
```

On Linux, the file dialogs need `zenity` (or `kdialog`) at runtime.

## Run

After build:

```sh
./build/src/gui/chat/mads-chat
```

After install:

```sh
mads-chat
```

or simply:

```sh
mads chat
```

The connection settings are persisted in a per-user configuration directory, so they are the same whatever the working directory is:

| Platform | Settings file |
|----------|---------------|
| macOS    | `~/Library/Application Support/MADS/mads-chat.json` |
| Linux    | `$XDG_CONFIG_HOME/mads/mads-chat.json`, or `~/.config/mads/mads-chat.json` if `XDG_CONFIG_HOME` is unset |
| Windows  | `%APPDATA%\MADS\mads-chat.json` (or `%USERPROFILE%\AppData\Roaming\MADS\mads-chat.json`) |

Setting the `MADS_CONFIG_DIR` environment variable to a directory overrides all of the above, which is useful for portable setups and tests. The directory is created on first use with owner-only permissions.

Older versions stored the settings in `mads_chat_settings.json` in the working directory. On startup, if the new file does not exist and that old file does, its contents are copied to the new location and a message is printed. The old file is never deleted, so it can be removed by hand once you have checked the migration. If the configuration directory cannot be determined, the application warns on stderr and falls back to `./mads_chat_settings.json`.
