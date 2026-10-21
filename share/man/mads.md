
# NAME

**mads** - frontend wrapper to MADS command line tools

# SYNOPSIS

**mads** 
  [**\-i|\-\-info**]
  [**\-p|\-\-prefix**]
  [**\-\-plugins**]
  [**\-\-keypair[=mads]**]
  [**\-f|\-\-force**]
  [**\-v|\-\-version**]
  [**\-h|\-\-help**]

**mads** *subcommand* [*args*]

# DESCRIPTION

**mads** is a simple wrapper to the MADS command line tools. It just makes the syntax nicer, allowing you to call e.g. mads broker rather than mads-broker.

In addition, the **mads** command allows to create systemd services for running MADS agents as services. The services are created in the /etc/systemd/system directory and can be started, stopped, enabled and disabled using the systemctl command.

# OPTIONS

**\-i|\-\-info**
: provides installation info

**\-p|\-\-prefix**
: returns the MADS installation prefix

**\-\-plugins**
: list te available plugins

**\-\-keypair[=mads]**
: generate a pair of public/private keys, with an optional name (defailt to `mads`). Refuses to overwrite existing files, unless `--force`. The secret `NAME.key` is owner-only (mode 0600 on Unix); for remote editing of the broker's settings, copy only `NAME.pub` to the broker's editors folder and keep `NAME.key` on the editor's machine (see **mads-broker**(1))

**\-f|\-\-force**
: force overwrite existing files (only in conjunction with `--keypair`)

**\-v**, **\-\-version**
:   Show version information.

**-h**, **\-\-help**
:   Show summary of options.

# SUBCOMMANDS

**mads** supports the following subcommands:

**broker**
:  Start a MADS broker. Use -d option unless you want it interactive. Wraps **mads-broker**.

**logger**
:  Start a MADS logger. Wraps **mads-logger**.

**source**
:  Start a MADS source. Wraps **mads-source**.

**sink**
:  Start a MADS sink. Wraps **mads-sink**.

**filter**
:  Start a MADS filter. Wraps **mads-filter**.

**dealer**
:  Start a MADS dealer. Wraps **mads-dealer**.

**worker**
:  Start a MADS worker. Wraps **mads-worker**.

**service**
:  Create a systemd service for a MADS agent. The service is created in /etc/systemd/system and can be started, stopped, enabled and disabled using the systemctl command. See below for more details.

**ini**
:  Open the **mads.ini** node editor, or check, read or change the file from the command line. See **mads-ini**(1).

## SERVICE SUBCOMMAND (LINUX ONLY)

The **service** subcommand has the following syntax:

**mads** **service** *service_name* *command* [*args*]

where *command* is one of **broker**, **logger**, **source**, **sink** or **filter** and *name* is the name of the service to be created. The *args* are the arguments to be passed to the agent.

This command prints to the console the content of a systemd service file. Inspcet it to check if it is correct, then re-issue the same command with **sudo** to actually create the service.

Then, use **systemctl start** *service_name* to start the service, **systemctl stop** *service_name* to stop it, **systemctl enable** *service_name* to enable it at boot time and **systemctl disable** *service_name* to disable it.

## INI SUBCOMMAND

The **ini** subcommand manages the **mads.ini** settings file. It is described in full in **mads-ini**(1).

**mads ini** [*FILE*]
  opens the graphical node editor on *FILE*, or on *<prefix>/etc/mads.ini* when *FILE* is omitted (a new document from the template if that file does not exist). Without the GUI, a bare **mads ini** prints the template, as in earlier releases.

**mads ini** **\-\-check** [*FILE*], **mads ini** **\-\-get** *section.key* [*FILE*], **mads ini** **\-\-set** *section.key=value* [**\-\-comment** *text*] [*FILE*]
  parse and lint the file, print a raw value, or set keys while keeping the comments. See **mads-ini**(1) for exit codes and the syntax of values.

The template options generate a stub for the **mads.ini** file, allowing some minor configuration on the command line. They are unchanged: **mads ini** **\-\-print** prints the template, and the options below write it to a file or install it:

**mads ini**
  [**\-p,\-\-print**]
  [**\-o,\-\-output**] *filename*
  [**\-i,\-\-install**]
  [**\-b,\-\-broker**] *broker address*
  [**\-F,\-\-frontend**] *broker frontend port*
  [**\-B,\-\-backend**] *broker backend port*
  [**\-s,\-\-settings**] *broker settings port*
  [**\-f,\-\-fps**] *frames per second*
  [**\-m,\-\-mongo**] *MongoDB URI*
  [**\-h,\-\-help**]


# BUGS

The upstream bug tracker can be found at https://github.com/pbosetti/MADS/issues.

# SEE ALSO

**mads-broker**(1), **mads-logger**(1), **mads-source**(1), **mads-sink**(1), **mads-filter**(1)

# FILES

**/etc/systemd/system/mads-<service_names>.service**: systemd service files created by **mads service**.

**/usr/local/etc/mads.ini**: MADS configuration file.

# AUTHOR

**mads** was written by Paolo Bosetti <paolo.bosetti@unitn.it>.

# LICENSE

https://www.apache.org/licenses/LICENSE-2.0
