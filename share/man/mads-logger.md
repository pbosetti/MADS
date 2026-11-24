# NAME

**mads-logger** - Deprecated shortcut for the MongoDB logger filter

# SYNOPSIS

**mads-logger** [*mads-filter options*]

# DESCRIPTION

**mads-logger** is a deprecated shortcut for **mads filter mongodb.plugin -n logger -b**: it takes the same options as **mads-filter**(1), and warns on startup that it is deprecated.

The **mongodb.plugin** filter logs every message it receives to MongoDB, one collection per topic. It is not part of MADS: install it with **mads package -i mongodb.plugin**. If it is not installed, **mads-logger** exits with an error saying so.

The former options map as follows: **-m** *URI* becomes **-o mongo_uri=***URI*, and **-p** becomes **-o initially_paused=true**. The options **--echo**, **--file**, **--array**, **--no-mongo** and **--cross** no longer exist.

# SETTINGS

The **[logger]** section of the settings file:

**mongo_uri**
:  MongoDB connection URI (default *mongodb://localhost:27017*).

**mongo_db**
:  Database name (default *mads*).

**initially_paused**
:  Start with logging paused (default *false*). A message on the **metadata** topic with a boolean **pause** field pauses or resumes logging.

**status_period_ms**
:  Period of the **{"logger_paused": bool}** status published on the **logger_status** topic (default *1000*; *0* disables it).

**dont_block**
:  Non-blocking receive (default *true* for **mads-logger**). With *false* the status is published only when messages arrive.

**sub_topic**
:  Topics to log; **[""]** logs all of them.

# SEE ALSO

**mads**(1), **mads-filter**(1), **mads-package**(1)
