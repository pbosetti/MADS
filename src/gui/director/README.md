# mads-director

`director` is a simple C++20 process supervisor with a Dear ImGui desktop UI for launching and controlling multiple processes from a TOML file.

## Features

- TOML-driven process definitions
- Optional global `[director]` settings
- Required field per process section: `command` (the section name is the process name)
  - command template variables:
    - `${PWD}`: resolved working directory for the process instance
    - `${ID}`: process instance index (0-based)
- Optional fields:
  - `after = "process_name"`: start this process only after another process has started
  - `workdir = "path"`: run process in this working directory. Relative paths are resolved against the directory containing the `director.toml` file, which is also the default when omitted.
  - `enabled = <bool>`: disable auto-start when `false` (manual start from UI is still allowed; default: `true`)
  - `scale = <int>`: spawn N instances of this process
  - `relaunch = <bool>`: auto-restart on non-zero exit
  - `tty = <bool>`: run child under PTY and allow interactive `attach`/`detach`
  - `base_instance_id = <int>`: set the starting index for scaled process instance IDs (default: 0)
- If a process working directory contains `.venv`, it is exposed to launched Python commands.
- Interactive UI:
  - select process
  - start, stop, restart
  - for `tty=true`, choose either:
    - send input from GUI
    - open a new terminal window attached to the running process
  - send stdin input to non-tty processes
  - view buffered output logs per process with ANSI color support

## Build

Built as part of MADS when `MADS_ENABLE_GUI=ON` (the default; see the top-level
`COMPILE.md`). The GUI dependencies (GLFW, Dear ImGui) are shared with the
other GUI app under `src/gui` and fetched at configure time.

```bash
cmake -Bbuild -GNinja
cmake --build build --target mads-director
```

## Run

```bash
mads-director director.toml   # or: -c/--config <file>
```

Show built-in example TOML:

```bash
mads-director
# or
mads-director --example
```

Internal attach client mode (used by GUI): `--attach-socket <path>`

The executable is named like the other MADS apps (`mads-director`, following
`MADS_EXECUTABLE_PREFIX`). `director.toml` is parsed by MadsCore
(`src/director_config.hpp`), the same parser used by `mads up`.

## Config format

Optional global settings:

```toml
[director]
terminal = "gnome-terminal" # optional: terminal executable for detached attach windows (recommended on Linux)
sample_rate = 2.0 # optional: process metrics sampling period in seconds (fractions allowed)
```

Use one top-level section per process. The section name is the process name:

```toml
[api]
command = "./my-service --port 9000 --wd=${PWD} --id=${ID}"
after = "db"
workdir = "services/api"
enabled = true
scale = 2
relaunch = true
tty = false
```

Scaled processes are named as `name[1]`, `name[2]`, etc.

Scaling behavior:
- `scale = 1` (default): a single process entry is created with the section name (for example `api`)
- `scale = N` (`N > 1`): `N` entries are created (`api[1]` ... `api[N]`)
- if a process has `after = "db"` and `db` is scaled, each dependent instance waits for all `db[...]` instances to be running

Interactive external terminal attach is currently implemented for POSIX terminals (macOS/Linux).

On Linux, setting `[director].terminal = "gnome-terminal"` is recommended.
