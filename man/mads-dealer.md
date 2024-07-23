
# NAME

**mads-dealer** - The dealer agent for the MADS network

# SYNOPSIS

**mads-dealer** 
  [**\-n, \-\-name** *agent_name*] 
  [**\-i, \-\-agent-id** *agent-id*]
  [**\-s, \-\-settings** *URI*]
  [**\-S, \-\-save-settings** *filename*]
  [**\-h, \-\-help**]

# DESCRIPTION

**mads-dealer** is the dealer agent for the MADS network. A **dealer** is a special agent that can distribute incoming messages to multiple **worker** agents, in a round-robin fashion. This is typically suitable to parallel computing needs, where a single task can be split into multiple sub-tasks that can be executed in parallel.

The dealer agent acts as a filter agent: it subscribes and receives messages from the broker (via SUB socket), sends them to the workers (via PUSH socket), then publishes a message to the broker (PUB socket) confirming that the given payload has been dispatched to a worker for elaboration.

# OPTIONS

**\-n**, **\-\-name**
:  The agent name. By default, the agent name is the plugin file name, without extension, so the plugin **publish.plugin** will have the agent name **publish**. The agent name is used for fetching the proper section forom the INI file, so this option allows to have different settings for different dealer agents loading the same plugin.

**\-i**, **\-\-agent-id**
:  The agent_id field is appended to the message payload. It allows to mark different agents that share the same agent name and the same settings section.

**\-s**, **\-\-settings** *URI*
:  Path to the settings file (ini format). It can be a valid ZeroMQ url in the form tcp://host:port.

**\-S**, **\-\-save-settings** *filename*
:  Save the settings (loaded by the broker or via **\-s** option) to the given file (ini format).

**\-h**, **\-\-help**
:  show summary of options.


# BUGS

The upstream bug tracker can be found at https://github.com/pbosetti/MADS/issues.

# SEE ALSO

**mads**(1), **mads-worker**(1)

# FILES

**/usr/local/etc/mads.ini**: MADS configuration file.

# AUTHOR

**mads-dealer** was written by Paolo Bosetti <paolo.bosetti@unitn.it>.

# LICENSE

https://creativecommons.org/licenses/by-sa/4.0/