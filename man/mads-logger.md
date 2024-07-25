
# NAME

**mads-logger** - The logger for the MADS network

# SYNOPSIS

**mads-logger** 
  [**\-p, \-\-pause**] 
  [**\-e, \-\-echo**]
  [**\-n, \-\-no-mongo**]
  [**\-m, \-\-mongo** *URI*]
  [**\-f, \-\-file** *filename*]
  [**\-a, \-\-array**]
  [**\-x, \-\-cross**]
  [**\-s, \-\-settings** *arg*]
  [**\-S, \-\-save-settings** *arg*]
  [**\-v, \-\-version**]
  [**\-h, \-\-help**]

# DESCRIPTION

**mads-logger** is the logger for the MADS network. It is monolithinc agent that listens for incoming messages from sources and sinks and logs them to a file or to a MongoDB database. The logger can be configured to pause the logging, to echo the incoming messages to the console, to log to a file or to a MongoDB database.

# OPTIONS

**\-p**, **\-\-pause**
:  Start the logger paused. The logger will not log any message until the pause is lifted, typicaly by using the **resume** command on the GUI or by sending a {"pause": false} message to the metadata topic. The message {"pause": true} stops the logging

**\-e**, **\-\-echo**
:  echoes to the console any logged message.

**\-n**, **\-\-no-mongo**
:  Disable logging to the MongoDB database, only logs to a file if **\-f** *filename* is given.

**\-m**, **\-\-mongo** *URI*
:  Log to the MongoDB database at the given URI. The URI must be in the form mongodb://user:password@host:port. If the URI is not given, the default URI mongodb://localhost:27017 is used.

**\-f**, **\-\-file** *filename*
:  Log to the given file. If the file exists, the logger appends to it. If the file does not exist, the logger creates it.

**\-a**, **\-\-array**
:  Log to a file as an array of JSON objects. If not, each message is logged as a separate JSON object, one per line.

**\-x**, **\-\-cross**
:  Cross-connect sockets: this is for debugging purposes and allows to connect an agent directly to the logger, bypassing the broker. For this to work, the logger agent needs to read a local settings file (**\-s** option).

**\-s**, **\-\-settings** *URI*
:  Path to the settings file (ini format). It can be a valid ZeroMQ url in the form tcp://host:port.

**\-S**, **\-\-save-settings** *filename.ini*
:  Save the settings (loaded by the broker or via **\-s** option) to the given file (ini format).

**\-v**, **\-\-version**
: show version information.

**\-h**, **\-\-help**
:  show summary of options.

# BUGS

The upstream bug tracker can be found at https://github.com/pbosetti/MADS/issues.

# SEE ALSO

**mads**(1), **mads-broker**(1), **mads-source**(1), **mads-sink**(1), **mads-filter**(1)

# FILES

**/usr/local/etc/mads.ini**: MADS configuration file.

# AUTHOR

**mads-logger** was written by Paolo Bosetti <paolo.bosetti@unitn.it>.

# LICENSE

https://creativecommons.org/licenses/by-sa/4.0/