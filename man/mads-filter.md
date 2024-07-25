
# NAME

**mads-filter** - The filter agent for the MADS network

# SYNOPSIS

**mads-filter** 
  [**\-n, \-\-name** *agent_name*] 
  [**\-i, \-\-agent-id** *agent-id*]
  [**\-d, \-\-delay** *delay in ms*]
  [**\-s, \-\-settings** *URI*]
  [**\-S, \-\-save-settings** *filename*]
  [**\-h, \-\-help**]
  [*plugin*]

# DESCRIPTION

**mads-filter** is the filter agent for the MADS network. 

# ARGUMENTS

*plugin*
:  The plugin to be loaded. The plugin is a shared library (with the **.plugin** extension) that implements the filter agent. You must provide either a full path to the plugin or the plugin name (without extension) if the plugin is in the standard plugin directory (/usr/local/lib/).

# OPTIONS

**\-n**, **\-\-name**
:  The agent name. By default, the agent name is the plugin file name, without extension, so the plugin **publish.plugin** will have the agent name **publish**. The agent name is used for fetching the proper section forom the INI file, so this option allows to have different settings for different filter agents loading the same plugin.

**\-i**, **\-\-agent-id**
:  The agent_id field is appended to the message payload. It allows to mark different agents that share the same agent name and the same settings section.

**\-d**, **\-\-delay** *delay in ms*
:  if larger than 0, waits that amount of ms before sending the first message. This is useful to deal with ZeroMQ slow joiner problem, i.e. when the agent starts sending PUB messages before fully establishing the connection, with the result that those messages are lost. The delay is applied only at the beginning of the agent's life; the agent already waits a small amount of time to take care about this problem, but this option allows to increase it in case of excessive network latency.

**\-s**, **\-\-settings** *URI*
:  Path to the settings file (ini format). It can be a valid ZeroMQ url in the form tcp://host:port.

**\-S**, **\-\-save-settings** *filename*
:  Save the settings (loaded by the broker or via **\-s** option) to the given file (ini format).

**\-h**, **\-\-help**
:  show summary of options.

# DEFAULT PLUGIN

The default plugin (if omitted) is **bridge.plugin**. This plugin prints to standard output any message received from the broker, then listens for messages on standard input and sends them back to the broker. This allows to use the filter agent as a simple command line tool to prototype a filter agent, typically by piping stdin and stdout to a scripting language.

## EXAMPLE SCRIPT (in Python3)

An example of using the default plugin and piping **mads-filter** via Python3 would be:

```python
  #!/usr/bin/env python3
  import subprocess
  import json

  command = "./products/bin/mads-filter"
  process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, stdin=subprocess.PIPE, shell=True)

  while True:
    # read input from the agent
    json_input = json.loads(process.stdout.readline())
    # change the input somehow
    json_input["reply"] = True
    # Dump the json as string
    json_str = json.dumps(json_input, separators=(',', ':'))
    print(json_str)
    # write the json to the agent stdin (convert to bytes and append \n)
    process.stdin.write(json_str.encode('UTF-8') + b'\n')
    process.stdin.flush()
```

# BUGS

The upstream bug tracker can be found at https://github.com/pbosetti/MADS/issues.

# SEE ALSO

**mads**(1), **mads-broker**(1), **mads-logger**(1), **mads-sink**(1), **mads-source**(1)

# FILES

**/usr/local/etc/mads.ini**: MADS configuration file.

# AUTHOR

**mads-filter** was written by Paolo Bosetti <paolo.bosetti@unitn.it>.

# LICENSE

https://creativecommons.org/licenses/by-sa/4.0/