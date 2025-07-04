
# NAME

**mads-sink** - The sink agent for the MADS network

# SYNOPSIS

**mads-sink** 
  [**\-n, \-\-name** *agent_name*] 
  [**\-i, \-\-agent-id** *agent-id*]
  [**\-d, \-\-delay** *delay in ms*]
  [**\-s, \-\-settings** *URI*]
  [**\-S, \-\-save-settings** *filename*]
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

Pligin loading follows this logic:

1. the command line provides a plugin: that plugin is used (regardless the INI file);
2. there is no plugin on command line but the INI file has an `attachment`: the latter is used;
3. no plugin is given on command line and no attachment in the INI file: the default plugin is used.

In case of multiple devices using the same plugin but **on different architectures**:

* the broker needs to have a copy of the same plugin compiled for each architecture;
* the INI file needs one section for each architecture, e.g. `[my_plugin_x86]` and `[my_plugin_arm64]`;
* each section has a different `attachment`, pointing to the path of the properly compiled plugins;
* each agent is launched with custom name: e.g. `mads source -n my_plugin_x86` on X86 linux, `mads source -n my_plugin_arm64` on ARM64 linux;
* the INI section for each agent specified the same `pub_topic`, so that all plugins publish on the same topic (remember that the default publish topic is the agent name!).


# OPTIONS

**\-n**, **\-\-name**
:  The agent name. By default, the agent name is the plugin file name, without extension, so the plugin **publish.plugin** will have the agent name **publish**. The agent name is used for fetching the proper section forom the INI file, so this option allows to have different settings for different sink agents loading the same plugin.

**\-i**, **\-\-agent-id**
:  The agent_id field is appended to the message payload. It allows to mark different agents that share the same agent name and the same settings section.

**\-d**, **\-\-delay** *delay in ms*
:  if larger than 0, waits that amount of ms before sending the first message. This is useful to deal with ZeroMQ slow joiner problem, i.e. when the agent starts sending PUB messages before fully establishing the connection, with the result that those messages are lost. The delay is applied only at the beginning of the agent's life; the agent already waits a small amount of time to take care about this problem, but this option allows to increase it in case of excessive network latency.

**\-s**, **\-\-settings** *URI*
:  Path to the settings file (ini format). It can be a valid ZeroMQ url in the form tcp://host:port.

**\-S**, **\-\-save-settings** *filename*
:  Save the settings (loaded by the broker or via **\-s** option) to the given file (ini format).

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

https://creativecommons.org/licenses/by-sa/4.0/