
# NAME

**mads-broker** - The broker for the MADS network

# SYNOPSIS

**mads-broker** [**\-n, \-\-nic**] [**\-s, \-\-settings** *arg*] [**\-d, \-\-daemon**] [**\-\-docker**] [**\-\-crypto[=arg]**] [**\-\-keys_dir[=arg]**] [**\-v, \-\-version**] [**\-h, \-\-help**]

# DESCRIPTION

**mads-broker** is the broker for the MADS network. It is a server that listens for incoming connections from sources and sinks and routes messages between them. It also provides a centralized INI file for configurating each agent/client.

# OPTIONS

**\-n**, **\-\-nic**
:  select the network interface (a string representing the nic name). This is purely cosmetic: the broker will listen on all interfaces anyway, but will use this nic name to identify the proper IP address and use it upon launch to provide the user with the correct IP address to be used for initializing the agents. Use **-n list** to list all available network interfaces.

**\-s**, **\-\-settings** *arg*
:  specify the settings file to be used. If not specified, the default settings file is used (usually /usr/local/etc/mads.ini).

**\-d**, **\-\-daemon**
:  run the broker as a daemon. This suppress the output to the console upon launch and the interactive behavior.

**\-\-docker**
: use it when it runs within a docker container

**\-\-crypto[=arg]**
: enable CURVE cryptography. The optional argument sets the name of the keys to be used for encryption (default to `broker`).

**\-\-keys_dir[=arg]**
: the path of the directory where to search for key files. Default to the `etc` directory under the mads prefix directory (`mads -p`).

**\-v**, **\-\-version**
: show version information.

**\-h**, **\-\-help**
:  show summary of options.

# BUGS

The upstream bug tracker can be found at https://github.com/pbosetti/MADS/issues.

# SEE ALSO

**mads**(1), **mads-logger**(1), **mads-source**(1), **mads-sink**(1), **mads-filter**(1)

# FILES

**/usr/local/etc/mads.ini**: MADS configuration file.

# AUTHOR

**mads-broker** was written by Paolo Bosetti <paolo.bosetti@unitn.it>.

# LICENSE

https://creativecommons.org/licenses/by-sa/4.0/