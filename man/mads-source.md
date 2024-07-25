
# NAME

**mads-source** - The source agent for the MADS network

# SYNOPSIS

**mads-source** 
  [**\-n, \-\-name** *agent_name*] 
  [**\-i, \-\-agent-id** *agent-id*]
  [**\-d, \-\-delay** *delay in ms*]
  [**\-p, \-\-period** *sampling_period*]
  [**\-s, \-\-settings** *URI*]
  [**\-S, \-\-save-settings** *filename*]
  [**\-h, \-\-help**]
  [*plugin*]

# DESCRIPTION

**mads-source** is the source agent for the MADS network. 

# ARGUMENTS

*plugin*
:  The plugin to be loaded. The plugin is a shared library (with the **.plugin** extension) that implements the source agent. You must provide either a full path to the plugin or the plugin name (without extension) if the plugin is in the standard plugin directory (/usr/local/lib/).

# OPTIONS

**\-n**, **\-\-name**
:  The agent name. By default, the agent name is the plugin file name, without extension, so the plugin **publish.plugin** will have the agent name **publish**. The agent name is used for fetching the proper section forom the INI file, so this option allows to have different settings for different source agents loading the same plugin.

**\-i**, **\-\-agent-id**
:  The agent_id field is appended to the message payload. It allows to mark different agents that share the same agent name and the same settings section.

**\-p**, **\-\-period** *sampling_period*
:  Sampling period in milliseconds. Default is 0, meaning free run. If the sampling period is set to a value greater than 0, the source agent will send a message every *sampling_period* milliseconds.

**\-d**, **\-\-delay** *delay in ms*
:  if larger than 0, waits that amount of ms before sending the first message. This is useful to deal with ZeroMQ slow joiner problem, i.e. when the agent starts sending PUB messages before fully establishing the connection, with the result that those messages are lost. The delay is applied only at the beginning of the agent's life; the agent already waits a small amount of time to take care about this problem, but this option allows to increase it in case of excessive network latency.

**\-s**, **\-\-settings** *URI*
:  Path to the settings file (ini format). It can be a valid ZeroMQ url in the form tcp://host:port.

**\-S**, **\-\-save-settings** *filename*
:  Save the settings (loaded by the broker or via **\-s** option) to the given file (ini format).

**\-h**, **\-\-help**
:  show summary of options.

# DEFAULT PLUGIN

The default plugin (if omitted) is **publish.plugin**. This plugin listens on standard input and sends the input to the broker, assuming that any newline terminated string is a JSON message. This allows to use the source agent as a simple command line tool to send messages to the broker, or to pipe through it messages from a scripting language.

# BUGS

The upstream bug tracker can be found at https://github.com/pbosetti/MADS/issues.

# SEE ALSO

**mads**(1), **mads-broker**(1), **mads-logger**(1), **mads-sink**(1), **mads-filter**(1)

# FILES

**/usr/local/etc/mads.ini**: MADS configuration file.

# AUTHOR

**mads-source** was written by Paolo Bosetti <paolo.bosetti@unitn.it>.

# LICENSE

https://creativecommons.org/licenses/by-sa/4.0/