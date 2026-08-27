
# NAME

**mads-sink** - The sink agent for the MADS network

# SYNOPSIS

**mads-sink** 
  [**\-n, \-\-name** *agent_name*] 
  [**\-\-driver** *driver_name*]
  [**\-i, \-\-agent-id** *agent-id*]
  [**\-d, \-\-delay** *delay in ms*]
  [**\-b, \-\-dont-block**]
  [**\-s, \-\-settings** *URI*]
  [**\-r, \-\-room** [*room*]]
  [**\-S, \-\-save-settings** *filename*]
  [**\-o, \-\-option** *key=value*]
  [**\-\-silent**]
  [**\-\-crypto**]
  [**\-\-keys_dir[=path]**]
  [**\-\-key_broker[=name]**]
  [**\-\-key_client[=name]**]
  [**\-\-auth_verbose**]
  [**\-v, \-\-version**]
  [**\-h, \-\-help**]
  [*plugin*]

# DESCRIPTION

**mads-sink** is the sink agent for the MADS network. 

# ARGUMENTS

*plugin*
:  The plugin to be loaded. The plugin is a shared library (with the **.plugin** extension) that implements the sink agent. You must provide either a full path to the plugin or the plugin name (without extension) if the plugin is in the standard plugin directory (/usr/local/lib/).

## OTA Plugins

Plugins can also be loaded **Over-The-Air** (OTA): the broker has a local copy of the plugins and provides each agent with the necessary plugin upon launch, as an attachment to the INI file. In this way, deployment of new versions of the plugins can be easily centralized.

For this to work, the INI section of a given agent must have the `attachment` key, set to the local path (in the broker filesystem) of the plugin needed by that agent.

When the agent starts on a remote device, it requests the broker for an copy of the INI file. If the INI section specifies the `attachment`, then the broker also sends a compiled copy of the plugin, which is saved by the agent to a temporary directory and then dynamically loaded.

The plugin is cached under a path that includes a digest of its own contents, `<temp>/mads/<section>/<digest>/<section>.<ext>`. Two consequences are worth knowing:

* several instances of the same agent on one host — as produced by **mads-director**/**mads-up** with `scale` greater than 1 — share a single cached copy instead of each overwriting a common file. No `${ID}` templating or `--agent-id` is needed to make this safe.
* when the plugin binary on the broker changes, its digest changes with it, so the new version lands on a new path and is picked up automatically at the next launch. Superseded copies are removed once no longer selected.

Three things are resolved independently, each with its own precedence, and each is settled before the plugin file is actually loaded:

**Plugin file:**

1. the command line provides a *plugin* argument: that file is used (regardless of the INI file);
2. there is no plugin on the command line but the INI section has an `attachment`: the latter is used;
3. neither is given: the default plugin is used.

**Driver** (the name looked up inside the plugin file — matters when one file registers more than one driver, or when OTA's own naming, see below, doesn't match):

1. `--driver` on the command line;
2. the `driver` key in the INI section;
3. the plugin file's stem (e.g. `feedback.plugin` → `feedback`).

**Settings section** (which INI section configures this agent):

1. `-n`/`--name` on the command line;
2. the stem of a *plugin* argument, if one was given;
3. the compiled-in default agent name.

The settings section is always resolved before the plugin file, since it is what the broker request for an `attachment` is keyed on. A loaded plugin's self-reported `kind()` is checked against the driver name it was loaded under and a warning is printed on a mismatch, but it does not otherwise change anything — this is a diagnostic, not a fourth precedence rule.

Because the OTA-served file is always saved under a temporary name matching the *settings section*, not the driver's own name, an OTA plugin whose registered driver name differs from the section name needs an explicit `driver` key — see the multi-architecture example below.

In case of multiple devices using the same plugin but **on different architectures**:

* the broker needs to have a copy of the same plugin compiled for each architecture;
* the INI file needs one section for each architecture, e.g. `[my_plugin_x86]` and `[my_plugin_arm64]`;
* each section has a different `attachment`, pointing to the path of the properly compiled plugin, and the same `driver` key naming the driver actually registered inside it (e.g. `driver = "my_plugin"`, since OTA would otherwise look for a driver named after the section instead);
* each agent is launched with a custom name: e.g. `mads sink -n my_plugin_x86` on X86 linux, `mads sink -n my_plugin_arm64` on ARM64 linux;
* the INI section for each agent specifies the same `pub_topic`, so that all plugins publish on the same topic (remember that the default publish topic is the agent name!).

```ini
[my_plugin_x86]
attachment = "/opt/mads/plugins/x86/my_plugin.plugin"
driver = "my_plugin"
pub_topic = "my_plugin"

[my_plugin_arm64]
attachment = "/opt/mads/plugins/arm64/my_plugin.plugin"
driver = "my_plugin"
pub_topic = "my_plugin"
```


# OPTIONS

**\-n**, **\-\-name**
:  The agent name. By default, the agent name is the plugin file name, without extension, so the plugin **publish.plugin** will have the agent name **publish**. The agent name is used for fetching the proper section forom the INI file, so this option allows to have different settings for different sink agents loading the same plugin.

**\-\-driver** *driver_name*
:  The driver to instantiate from the plugin file, overriding both the section's `driver` setting and the file-stem default. Only needed when a plugin file registers more than one driver, or when OTA's temporary filename (named after the settings section, not the driver) doesn't match the driver's own registered name.

**\-i**, **\-\-agent-id**
:  The agent_id field is appended to the message payload. It allows to mark different agents that share the same agent name and the same settings section.

**\-d**, **\-\-delay** *delay in ms*
:  if larger than 0, waits that amount of ms before sending the first message. This is useful to deal with ZeroMQ slow joiner problem, i.e. when the agent starts sending PUB messages before fully establishing the connection, with the result that those messages are lost. The delay is applied only at the beginning of the agent's life; the agent already waits a small amount of time to take care about this problem, but this option allows to increase it in case of excessive network latency.

**\-b**, **\-\-dont-block**
:  Do not block on read.

**\-s**, **\-\-settings** *URI*
:  Path to the settings file (ini format). It can be a valid ZeroMQ url in the form tcp://host:port.

**\-r**, **\-\-room** [*room*]
:  Discover the broker settings port by listening for a broker advertisement in the named service-discovery room. If *room* is omitted, the default room is **mads**. This behavior is opt-in: when **\-\-room** is not given, the sink keeps the usual settings lookup behavior and uses **\-\-settings** or the compiled default settings URI.

**\-S**, **\-\-save-settings** *filename*
:  Save the settings (loaded by the broker or via **\-s** option) to the given file (ini format).

**\-o, \-\-option** *key=value*
:  Override plugin-specific options that are typically set in the `mads.ini` file. Do not put spaces around the `=`. the value is interpretes as a string, an integer or a float, according to standard heuristics. This option can be repeated.

**\-\-silent**
:  Do not print the status line while messages are processed.

**\-\-crypto**
:  Enable CURVE encryption for broker communication.

**\-\-keys_dir[=path]**
:  Directory where CURVE key files are stored.

**\-\-key_broker[=name]**
:  Name of the broker key file, without the `.key` extension. Defaults to **broker**.

**\-\-key_client[=name]**
:  Name of the client key file, without the `.key` extension. Defaults to **client**.

**\-\-auth_verbose**
:  Enable verbose authentication messages.

**\-v**, **\-\-version**
: show version information.

**\-h**, **\-\-help**
:  show summary of options.

# DEFAULT PLUGIN

The default plugin (if omitted) is **feedback.plugin**. This plugin prints on standard output any message received from the broker.

# BUGS

The upstream bug tracker can be found at https://github.com/pbosetti/MADS/issues.

# SEE ALSO

**mads**(1), **mads-broker**(1), **mads-logger**(1), **mads-source**(1), **mads-filter**(1)

# FILES

**/usr/local/etc/mads.ini**: MADS configuration file.

# AUTHOR

**mads-sink** was written by Paolo Bosetti <paolo.bosetti@unitn.it>.

# LICENSE

https://www.apache.org/licenses/LICENSE-2.0
