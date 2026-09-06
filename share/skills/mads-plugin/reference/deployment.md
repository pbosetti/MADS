# Installing, deploying and distributing a plugin

## Install location

```bash
cmake -Bbuild -DCMAKE_INSTALL_PREFIX="$(mads -p)"
cmake --build build -j4
sudo cmake --install build
```

The generated `CMakeLists.txt` already defaults `CMAKE_INSTALL_PREFIX` to the
output of `mads -p`, so the explicit flag is only needed when configuring in an
environment where `mads` is not on the `PATH`.

Plugins install to `<prefix>/lib` on Unix and `<prefix>/bin` on Windows. That
is exactly where a host looks when the plugin argument is not an existing path:

```bash
mads source mysensor.plugin      # found as <prefix>/lib/mysensor.plugin
mads source ./build/mysensor.plugin
```

`mads --plugins` lists what is installed.

## Architecture suffixes

In a mixed-architecture fleet, build with a suffix:

```bash
cmake -Bbuild -DPLUGIN_SUFFIX=arm64
```

This produces `mysensor_arm64.plugin` **and** compiles `PLUGIN_NAME` as
`"mysensor_arm64"`, so the file stem, the registered driver name and `kind()`
all stay consistent. Without a suffix you get `mysensor.plugin` and the plain
name. Do not rename a built plugin file: the stem is what the host looks the
driver up by (unless a `driver` setting or `--driver` says otherwise).

## OTA delivery from the broker

An agent can receive its plugin from the broker instead of holding a local
copy. In the **broker's** `mads.ini`, in the section of the agent that will run
it:

```ini
[publish]
attachment = "../lib/mysensor.plugin"   # a path in the BROKER's filesystem
driver = "mysensor"                     # usually required, see below
attachment_ext = "plugin"               # default
```

The agent asks the broker for its settings, receives the file as an attachment,
saves it locally and loads it. The catch: the downloaded file is named after
the **agent section**, not after the original plugin. If the section name and
the registered driver name differ, the file-stem lookup fails — so set
`driver` explicitly whenever using OTA.

The attachment is only used when no `--plugin` argument was given on the CLI.

## One library, several plugins

`MADS_REGISTER_PLUGINS()` accepts several classes:

```cpp
MADS_REGISTER_PLUGINS(MySource, MySink)
```

All of them register under the **same driver name** (`PLUGIN_NAME`) but for
different servers, and each host only registers the server type it cares
about — a `mads source` will pick up `MySource` and ignore `MySink`.
Registration succeeds as long as at least one class is accepted.

To ship several drivers *of the same type* in one library, build them as
separate targets with `add_plugin()`, or select between them with the `driver`
setting / `--driver`.

## Publishing to the MADS package list

`mads package -l` lists installable packages and `mads package -i <name>`
installs one, pulling from the MADS package registry
(`packages.json` in the `MADS-NET/.github` repository). To have a plugin listed,
add an entry pointing at its public repository:

```json
"mysensor.plugin": {
  "URI": "https://github.com/<owner>/<repo>",
  "type": "plugin"
}
```

Package names for plugins end in `.plugin`; `type` is one of `plugin`, `agent`
or `tool`. Users then get it with `mads package -i mysensor.plugin`.

## Checklist before shipping

- The README documents every INI key, its default and its unit.
- The plugin builds and installs on Linux, macOS and Windows, or the README
  states which platforms are supported.
- `mads inspect_plugin` reports the current protocol and a matching
  `nlohmann/json` version.
- The plugin runs with no hardware attached (see `io.md`).
- The version pinned in `CMakeLists.txt` for `mads_plugin`, `pugg` and
  `nlohmann/json` is the one the target MADS release expects — regenerate or
  run `mads plugin --update` rather than bumping them by hand.
