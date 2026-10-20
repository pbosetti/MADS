# NAME

**mads-ini** - Node editor and command-line tools for the MADS settings file (mads.ini)

# SYNOPSIS

**mads ini** [*FILE*]

**mads ini** **\-p**, **\-\-print**

**mads ini** [**\-o**, **\-\-output** *file* | **\-i**, **\-\-install**]
  [**\-b**, **\-\-broker** *host*]
  [**\-F**, **\-\-frontend** *port*]
  [**\-B**, **\-\-backend** *port*]
  [**\-s**, **\-\-settings** *port*]
  [**\-f**, **\-\-fps** *n*]
  [**\-m**, **\-\-mongo** *uri*]

**mads ini** **\-\-check** [*FILE*]

**mads ini** **\-\-get** *section.key* [*FILE*]

**mads ini** **\-\-set** *section.key=value* [**\-\-set** *section.key=value*]... [**\-\-comment** *text*] [*FILE*]

**mads ini** [**\-h**, **\-\-help**]

**mads-ini** accepts the same options. The **mads** dispatcher runs it for **mads ini** when it is installed.

# DESCRIPTION

**mads-ini** edits a MADS settings file, **mads.ini**, in two ways. Without options it opens a
graphical node editor. With **\-\-check**, **\-\-get**, **\-\-set** or the template options it works
headless, so it can be used in scripts and over SSH.

In the editor each agent section of the file is a node, and each topic that one agent publishes and
another subscribes to is a link. The **[agents]** and **[broker]** sections are not nodes: they are
the document settings, edited in the inspector. The editor reads and writes the file itself. It
keeps comments, leaves the values of keys it does not change exactly as written, and saves through a
temporary file, so a crash or power loss leaves either the old or the new file.

Bare **mads ini** opens the editor on `<prefix>/etc/mads.ini` when that file exists, or on a new
document rendered from the template when it does not. **mads ini** *FILE* opens the editor on *FILE*.
Builds without the GUI (or a system where mads-ini is not installed) have no editor: a bare
**mads ini** then prints the template, as older releases did, and **mads ini** *FILE* is an error that
points to **\-\-check**, **\-\-get** and **\-\-set**.

## Template output

**\-p**, **\-\-print** writes the rendered template to standard output. **\-o**, **\-\-output** writes it
to a file, and **\-i**, **\-\-install** installs it as `<prefix>/etc/mads.ini` (not on Windows). The
template options set values in the rendered text:

- **\-b**, **\-\-broker** sets the broker host name or address;
- **\-F**, **\-\-frontend**, **\-B**, **\-\-backend** and **\-s**, **\-\-settings** set the frontend,
  backend and settings ports of the broker (note that **\-s** is a port here, and a file path in the
  other **mads-*** commands);
- **\-f**, **\-\-fps** sets the frames per second;
- **\-m**, **\-\-mongo** sets the MongoDB URI.

The template options cannot be combined with **\-\-check**, **\-\-get** or **\-\-set**, nor with a FILE
argument. **\-o** and **\-i** cannot be used together.

## Checking a file

**\-\-check** [*FILE*] parses and lints the file without changing it. It prints one line per problem,
in the form *FILE:LINE: severity: [section] key: message*, and then a summary. A file with no errors
or warnings prints **OK**. Lint rules are:

- **error**: the file, or a value, is not valid TOML;
- **warning**: a key unknown to the node type (unless the type accepts custom keys), a value of the
  wrong TOML type, a string that is not one of the allowed choices, or a **sub_topic** entry that is
  not a valid wildcard (see below);
- **info**: a published topic that no agent subscribes to, or a subscribed topic that no agent
  publishes. These are reported but do not change the exit status.

Disabled (commented-out) keys are not checked.

## Reading and changing a single key

**\-\-get** *section.key* [*FILE*] prints the raw TOML text of the active value, for example
`100` or `["sensors/#"]`. The name is split at its first dot, so *agents.a.b* is section *agents*,
key *a.b*. A key that is missing, or commented out, is an error.

**\-\-set** *section.key=value* [*FILE*] sets a key. It can be repeated to set several keys in one
save, and creates the section if it does not exist. The value is TOML text when it is a valid TOML
value, so **100**, **true**, **1.5** and `["a", "b"]` keep their types. Anything else is stored as
a string: a plain word such as **sensors/imu** or **hello world** is written as `"sensors/imu"` or
`"hello world"`. Quote values yourself when a string must look like a number or a boolean.

Setting a key keeps all comments in the file. **\-\-comment** *text* also sets the inline comment of
each key given with the same **\-\-set**; line breaks in *text* become spaces. **\-\-comment** requires
**\-\-set**.

**\-\-set** does not lint the value. Run **\-\-check** afterwards to see whether it fits the schema.

# OPTIONS

**\-p**, **\-\-print**
:  Print the rendered template and exit.

**\-o**, **\-\-output** *file*
:  Write the rendered template to *file*. The directory must exist.

**\-i**, **\-\-install**
:  Install the rendered template as `<prefix>/etc/mads.ini`, creating the directory if needed. Not
   available on Windows.

**\-b**, **\-\-broker** *host*
:  Broker host name or address written into the template. Default `localhost`.

**\-F**, **\-\-frontend** *port*
:  Broker frontend port. Default `9090`.

**\-B**, **\-\-backend** *port*
:  Broker backend port. Default `9091`.

**\-s**, **\-\-settings** *port*
:  Broker settings port. Default `9092`.

**\-f**, **\-\-fps** *n*
:  Frames per second written into the template.

**\-m**, **\-\-mongo** *uri*
:  MongoDB URI. Default `mongodb://localhost:27017`.

**\-\-check** [*FILE*]
:  Parse and lint *FILE*, and exit with the status described below.

**\-\-get** *section.key* [*FILE*]
:  Print the raw value of an active key.

**\-\-set** *section.key=value* [*FILE*]
:  Set a key and save the file. Repeatable.

**\-\-comment** *text*
:  Inline comment for the keys given with **\-\-set**.

*FILE*
:  The settings file. For the GUI it is opened for editing. For **\-\-check**, **\-\-get** and **\-\-set**
   it defaults to `<prefix>/etc/mads.ini`.

**\-h**, **\-\-help**
:  Show a summary of the options.

# GRAPHICAL EDITOR

The window has three parts: a palette on the left, the node canvas in the middle, and the inspector
on the right. A status footer shows the file name (with *(modified)* when there are unsaved
changes), and the first error of the document, if any, in red.

## Nodes

Each section of the file that is not **[agents]** or **[broker]** is a node. Its type comes from the
editor's metadata line (see COMMENTS AND LAYOUT IN THE FILE), or else from the section name and the
keys it sets. The title bar is colored by role: green for a source (it publishes only), blue for a
filter (it both subscribes and publishes), and orange for a sink (it only subscribes). The palette
groups the types into categories (Agents, Plugin loaders, Other), and
clicking an entry adds a node in the centre of the view. The name of a new node is the first free
alias of its type (for example *federate_a*), else the type id, with *_2*, *_3*, ... appended when
taken.

The inspector shows every key the schema knows for the selected node, grouped by topic, timing and
so on, with typed widgets. A key can be enabled or commented out, given an inline comment, and the
section comment can be edited. Keys not in the schema can be added to node types that accept custom
keys. Sub-tables such as *[name.x]* are kept verbatim in the file.

Nodes can be renamed, duplicated (the copy is offset by 40 pixels on both axes) and deleted.
Renaming a node does not rewrite the **sub_topic** entries of other nodes that matched its old name
as an implicit **pub_topic**.

## Links

A link says that the subscriber receives what the publisher publishes. The publisher's topic is its
**pub_topic**, or its section name when the key is absent. Subscribers have one input pin per
**sub_topic** entry, and publishers have one output pin. The entry `""` is shown as the pin
**(all)**.

Each link is computed from the declared topics with the same rules the agents apply at run time:

- **exact**: a topic equal to a **sub_topic** entry, drawn in blue;
- **prefix**: a plain (wildcard-free) entry that is a byte prefix of the topic, drawn in amber. The
  broker and the agents match plain entries by byte prefix, so *sensors* also receives
  *sensors_raw*, not only *sensors/...*;
- **wildcard**: an entry with MQTT wildcards that matches the topic, drawn in cyan;
- **implied**: a publisher without a **pub_topic** key, drawn in grey.

The wildcard grammar follows MQTT:

- **+** matches exactly one level, and is a wildcard only when it is a whole level (*a/+/b*);
- **#** matches the level it replaces and everything below it, so *sensors/#* also matches
  *sensors*. It is valid only as the last level (*#* or *a/#*);
- **\*** is not a wildcard. It matches itself, and the editor warns about it. Use a prefix
  (*fmu_*) or **+** and **#** levels instead;
- a **+** that shares a level with other characters (*fmu_+*) is not a wildcard either. It matches
  itself literally, and the editor warns about it.

The **sub_topic** entry `""` subscribes to everything. Its links are *catch-all* links. They are
hidden by default: the **(all)** pin of the subscriber shows a badge **<- N**, where *N* is the
number of publishers hidden behind it. Turn them on with **View > Show catch-all links**, or in
the canvas context menu.

An input pin whose entry no agent publishes to is marked with **!** in its label. Both kinds of
unmatched topic are listed as info in the problems list.

## Connecting and disconnecting

Drag from an output pin to an input pin to subscribe the target to the source's topic. The editor
appends that topic to the target's **sub_topic**, turning a single string into an array and creating
the key if needed. If the target already receives the topic through an existing entry, nothing is
changed and the status footer names the entry. Connecting a node to itself, or from a node without a
topic, is refused.

To disconnect, Ctrl-click the link's pin, or select the link and press **Delete**. The editor removes
the **sub_topic** entries that equal the topic exactly. Removing the last entry leaves
**sub_topic = []**. If another entry (a prefix, a wildcard, or the catch-all `""`) also delivers
the topic, the editor refuses and names the pattern: edit that **sub_topic** by hand.

Each connect or disconnect is one undo step.

## Keyboard and mouse

The shortcuts below use Ctrl. On macOS the editor shows and uses Cmd in their place.

| Action | Input |
|---|---|
| New document | Ctrl+N |
| Open | Ctrl+O |
| Save | Ctrl+S |
| Save As | Ctrl+Shift+S |
| Reload from disk | File > Reload |
| Quit | Ctrl+Q |
| Undo | Ctrl+Z |
| Redo | Ctrl+Shift+Z or Ctrl+Y |
| Duplicate node | Ctrl+D |
| Delete selected nodes and links | Delete or Backspace, with the pointer over the canvas |
| Find node | Shift+Space, or View > Find node... |
| Pan the canvas | Hold Space and drag over the canvas, nodes included |
| Detach a link | Ctrl-click a link's pin (same as deleting it) |
| Move a node | Drag it; the new position is saved in the document |

The shortcuts are inactive while a dialog is open. While a text field has the keyboard, undo, redo,
duplicate and the node finder are inactive, and Space types a space.

The node finder (Shift+Space) is a box near the top of the window. It matches node names by prefix,
then by substring, then by the letters of the query in order. Up and Down arrows move the
highlight, Enter selects the node and centers the canvas on it, and Escape closes the box. At most
twelve matches are listed.

Right-click on a node for **Rename...**, **Duplicate** and **Delete**. Right-click on empty canvas
for **Add node** (by category), **Auto layout** and **Show catch-all links**. Right-click on a link
does nothing.

**Auto layout** (View menu, or the canvas menu) moves all nodes, arranging them left to right by
topic flow: sources left, sinks right. When a file is opened, only nodes with no saved position are
placed, and that does not mark the document as modified.

## Document settings

Select **Document settings** in the palette, or the View menu, to edit **[agents]** and **[broker]**
in the inspector. **[agents]** holds fleet-wide defaults. A key that the schema marks as inheritable, when set
there, applies to every node unless the node sets its own value. **[broker]** holds the broker settings.

# COMMENTS AND LAYOUT IN THE FILE

The editor reads the file line by line, so that comments stay where they were written. A comment
directly above a section header is the section's comment, and comment lines directly above a key
belong to that key. Commented-out keys (*# key = value*) are kept and
can be enabled again.

Layout is normalized when the file is written: spacing around **=** and before inline comments,
at most one consecutive blank line, one blank line before each section, no blank lines at the end of
a section, and a single trailing newline. Values are written back exactly as they were read. A file
written by hand is therefore reformatted the first time it is saved.

Node positions and other editor-only data are stored in a line of the form:

```
[sensors]
#@ type=filter pos=420,180
```

The line is a TOML comment, so agents never read it. **type** is the node type, and **pos** is the
canvas position in whole units. Unknown *key=value* pairs on the line are kept. When the line is
missing, the type is inferred from the section name and its keys.

# SAVING

Saving writes the new content to *FILE.tmp* in the same directory, copies the previous file to
*FILE.bak*, and renames the temporary file over *FILE*. The editor refuses to save text that is not
valid TOML.

The editor remembers a hash of the file as it was loaded or last saved. If the file has changed on
disk since then, a save does not overwrite it. The dialog offers **Reload (discard mine)**,
**Overwrite** and **Cancel**. The editor also checks for outside changes about once a second and
shows a banner when it finds one.

Opening, creating or closing a document with unsaved changes asks for **Save**, **Discard** or
**Cancel**.

# BROKER

The broker watches its settings file and reloads it about a second after it changes, whether the
change comes from this editor, **mads ini \-\-set** or a text editor (including editors that save by
writing a new file and renaming it over the old one). Agents fetch their settings from the broker
when they start, so a running agent keeps its settings until it is restarted.

One exception: a changed **attachment** path is only picked up when the broker restarts (press
**X** in the broker's console, or restart it).

The editor and **mads ini \-\-set** change the file only; they do not contact the broker.

# EXIT STATUS

**\-\-check**: **0** when there are no errors or warnings (info lines do not count). **1** when there
are warnings and no errors. **2** when there are errors, or when the file cannot be read.

**\-\-get**: **0** when the value is printed. **1** when the argument is not *section.key*, the file
cannot be parsed, or the key is missing or commented out.

**\-\-set**: **0** when the file is saved. **1** on an invalid argument, a file that cannot be parsed,
an invalid value, a save that fails, or a file that changed on disk before the save (the change is
not overwritten).

Template output: **0** on success. **1** when the template cannot be rendered, when **\-\-check**,
**\-\-get** or **\-\-set** is mixed with other modes or template options, or when **\-\-comment** is
used without **\-\-set**. **255** for an unknown option, for **\-o** combined with **\-i**, or when the
output file cannot be written or its directory does not exist.

Without a GUI, **mads ini** *FILE* exits **1** with a message.

# FILES

`<prefix>/etc/mads.ini`
:  Default settings file for the editor (bare **mads ini**), **\-\-check**, **\-\-get** and **\-\-set**.

`<prefix>/share/templates/mads.ini`
:  Template rendered by the template options and by a new document.

*FILE.bak*
:  The content of *FILE* before the last save. Overwritten at each save.

*FILE.tmp*
:  Temporary file written while saving. Renamed over *FILE*.

# EXAMPLES

Open the editor on the default settings file:

```
mads ini
```

Open a file in the editor:

```
mads ini deploy/mads.ini
```

Check a file and show its problems, for use in a continuous integration job:

```
mads ini --check deploy/mads.ini
```

Read the sampling period of one agent:

```
mads ini --get logger.time_step deploy/mads.ini
```

Set two keys at once, with an inline comment:

```
mads ini --set logger.time_step=50 --set logger.sub_topic='["sensors/#"]' \
  --comment "tuned on the test rig" deploy/mads.ini
```

Write a template for a broker on another host:

```
mads ini -o mads.ini -b broker.local -F 19090 -B 19091
```

# BUGS

The upstream bug tracker can be found at https://github.com/pbosetti/MADS/issues.

# SEE ALSO

**mads**(1), **mads-doctor**(1), **mads-broker**(1), **mads-up**(1), **mads-source**(1),
**mads-filter**(1), **mads-sink**(1)

# AUTHOR

**mads-ini** was written by Paolo Bosetti paolo.bosetti@unitn.it.

# LICENSE

https://www.apache.org/licenses/LICENSE-2.0
