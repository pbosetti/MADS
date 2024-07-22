
# NAME

**mads-worker** - The worker agent for the MADS network

# SYNOPSIS

**mads-worker** 
  [**\-n, \-\-name** *agent_name*] 
  [**\-i, \-\-agent-id** *agent-id*]
  [**\-s, \-\-settings** *URI*]
  [**\-S, \-\-save-settings** *filename*]
  [**\-h, \-\-help**]
  [*plugin*]

# DESCRIPTION

**mads-worker** is the worker agent for the MADS network. 

# ARGUMENTS

*plugin*
:  The plugin to be loaded. The plugin is a shared library (with the **.plugin** extension) that implements the worker agent. You must provide either a full path to the plugin or the plugin name (without extension) if the plugin is in the standard plugin directory (/usr/local/lib/).

For **mads-worker**, the plugin must be of type **filter**, i.e. it must implement the **mads::Filter** interface. It receives messages from the PUSH socket connected to a **mads-dealer** agent, processes them and sends the result back to the network via a PUB socket. The actual elaboration is performed by the *plugin*, which ---by default--- is the **broidge-plugin**.

# OPTIONS

**\-n**, **\-\-name**
:  The agent name. By default, the agent name is the plugin file name, without extension, so the plugin **publish.plugin** will have the agent name **publish**. The agent name is used for fetching the proper section forom the INI file, so this option allows to have different settings for different worker agents loading the same plugin.

**\-i**, **\-\-agent-id**
:  The agent_id field is appended to the message payload. It allows to mark different agents that share the same agent name and the same settings section.

**\-s**, **\-\-settings** *URI*
:  Path to the settings file (ini format). It can be a valid ZeroMQ url in the form tcp://host:port.

**\-S**, **\-\-save-settings** *filename*
:  Save the settings (loaded by the broker or via **\-s** option) to the given file (ini format).

**\-h**, **\-\-help**
:  show summary of options.

# DEFAULT PLUGIN

The default plugin (if omitted) is **bridge.plugin**. This plugin prints to standard output any message received from the broker, then listens for messages on standard input and sends them back to the broker. This allows to use the worker agent as a simple command line tool to prototype a worker agent, typically by piping stdin and stdout to a scripting language.


# BUGS

The upstream bug tracker can be found at https://github.com/pbosetti/MADS/issues.

# SEE ALSO

**mads**(1), **mads-dealer**(1)

# FILES

**/usr/local/etc/mads.ini**: MADS configuration file.

# AUTHOR

**mads-worker** was written by Paolo Bosetti <paolo.bosetti@unitn.it>.