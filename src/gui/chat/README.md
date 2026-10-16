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

At startup the application reads and writes `mads_multitool_settings.json` in the working directory to persist the connection settings.
