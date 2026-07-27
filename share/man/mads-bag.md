
# NAME

**mads-bag** - Inspects and exports bag files recorded by mads-record

# SYNOPSIS

**mads-bag info**
  **\-f, \-\-file** *path*
  [**\-t, \-\-topics**]
  [**\-j, \-\-json**]
  [**\-h, \-\-help**]

**mads-bag export**
  **\-f, \-\-file** *path*
  [**\-\-format** *jsonl*]
  [**\-o, \-\-output** *path*]
  [**\-h, \-\-help**]

# DESCRIPTION

**mads-bag** is a wrapper around two subcommands for working with bag files recorded by
**mads-record** (see **src/bag.hpp** for the on-disk format). Like every other **mads-\*** wrapper it is
normally invoked as **mads bag** *subcommand* ...; **mads-bag** *subcommand* ... works identically.

## info

Prints a summary of a bag file: whether its trailing index/footer is present (giving O(1) access) or
was recovered via a linear scan, whether the recovered data is a complete prefix (**truncated**),
whether per-record CRC32 is enabled, the record count, and the first/last recorded timestamp. With
**\-\-topics**, it also scans every record to report a per-topic message count.

## export

Streams the bag as **JSON Lines** (one JSON object per record, in order) to stdout or a file. Each line
has the shape:

```json
{"index": 0, "timestamp_ns": 1712345678901234567, "timestamp_iso": "2024-...", "topic": "sensors/acc/x", "parts_base64": ["..."]}
```

Every part is base64-encoded, regardless of whether it holds JSON text or binary blob bytes: a raw
wire part (which may be a compressed payload, a self-describing header, or blob bytes) is not
guaranteed to be valid UTF-8, so base64 is the only encoding that is always valid JSON Lines output.
To inspect a JSON part with **jq**, decode it explicitly, e.g. `jq -r '.parts_base64[0]' file.jsonl |
base64 -d | jq .`. **\-\-format mcap** is not implemented -- it is intentionally deferred as an optional
future export target (see **NEW_FEATURES.md**'s P3 section); any format other than **jsonl** is
rejected with an error.

# OPTIONS

## info

**\-f**, **\-\-file** *path*
:  Bag file to inspect. Required.

**\-t**, **\-\-topics**
:  Include a per-topic message count. Requires a full scan of the bag (still just N sequential record
   reads, using the index when present -- not the slow linear-scan recovery path).

**\-j**, **\-\-json**
:  Print the summary as a single JSON object instead of human-readable text.

**\-h**, **\-\-help**
:  Show summary of options.

## export

**\-f**, **\-\-file** *path*
:  Bag file to export. Required.

**\-\-format** *jsonl*
:  Export format. Only **jsonl** is currently implemented (also the default).

**\-o**, **\-\-output** *path*
:  Write to *path* instead of stdout.

**\-h**, **\-\-help**
:  Show summary of options.

# EXAMPLES

Summarize a bag file:

```
mads bag info -f session.bag
```

Same, as JSON, with a per-topic breakdown:

```
mads bag info -f session.bag -j -t
```

Export to JSON Lines and inspect one message's payload with jq:

```
mads bag export -f session.bag -o session.jsonl
jq -r 'select(.topic == "sensors/acc/x") | .parts_base64[0]' session.jsonl | head -1 | base64 -d | jq .
```

# BUGS

The upstream bug tracker can be found at https://github.com/pbosetti/MADS/issues.

# SEE ALSO

**mads**(1), **mads-record**(1), **mads-play**(1)

# AUTHOR

**mads-bag** was written by Paolo Bosetti <paolo.bosetti@unitn.it>.

# LICENSE

https://www.apache.org/licenses/LICENSE-2.0
