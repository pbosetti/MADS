# {{ name }} plugin for MADS

This is a {{ type }} plugin for [MADS](https://github.com/MADS-NET/MADS). 

<provide here some introductory info>

*Required MADS version: {{ mads_version }}.*


## Supported platforms

Currently, the supported platforms are:

* **Linux** 
* **MacOS**
* **Windows**


## Installation

Linux and MacOS:

```bash
cmake -Bbuild -DCMAKE_INSTALL_PREFIX="$(mads -p)"
cmake --build build -j4
sudo cmake --install build
```

Windows:

```powershell
cmake -Bbuild -DCMAKE_INSTALL_PREFIX="$(mads -p)"
cmake --build build --config Release
cmake --install build --config Release
```


## INI settings

The plugin supports the following settings in the INI file:

```ini
[{{ name }}]
# Describe the settings available to the plugin
```

All settings are optional; if omitted, the default values are used.

{% if datastore %}
## Datastore
The plugin supports a datastore file `{{ name }}.json` created in a temporary directory for persisting data between runs. Look at the `Datastore` class for more information on how to use it.
{% endif %}


## Executable demo

<Explain what happens if the test executable is run>

---