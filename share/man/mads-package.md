
# NAME

**mads-package** - List, inspect, and install MADS binary packages

# SYNOPSIS

**mads-package**
  [**\-l**, **\-\-list**]
  [**\-v**, **\-\-verbose**]
  [**\-n**, **\-\-info** *package*]
  [**\-i**, **\-\-install** *package*]
  [**\-f**, **\-\-force**]
  [**\-\-no-cache**]
  [**\-\-json**]
  [**\-v**, **\-\-version**]
  [**\-h**, **\-\-help**]

# DESCRIPTION

**mads-package** lists, inspects, and installs optional MADS packages distributed as ZIP archives from GitHub releases.

The command reads the public MADS package index from GitHub, resolves each package repository, and inspects the latest compatible release asset for the current operating system and CPU architecture. Stable GitHub releases are preferred. If a repository has no stable latest release, **mads-package** falls back to the newest non-draft pre-release.

Packages are installed from release ZIP files. The selected ZIP is downloaded to a temporary unprivileged directory, extracted, and merged into the MADS installation prefix, i.e. the directory printed by **mads** **\-p**. Archive directory structure is preserved. If an archive contains a single top-level directory, that directory is stripped so its contents are installed directly under the prefix.

The package index and package metadata are cached under the user's home directory. Cached data is reused for up to six hours unless **\-\-no-cache** is specified. A fresh successful fetch updates the cache. If GitHub returns HTTP 403 during the fetch, the cache is not updated and a warning is printed, since this usually means GitHub is rate limiting traffic.

# OPTIONS

**\-l**, **\-\-list**
:  List installable MADS packages. For each package, show the repository, the latest stable release or latest pre-release fallback, and compatible ZIP release assets for the current platform.

**\-v**, **\-\-verbose**
:  Show additional information when listing packages. This includes the GitHub repository About text when available, plus package requirements read from an optional **mads_package.json** file in the repository root.

**\-n**, **\-\-info** *package*
:  Print detailed information for *package*. This includes the GitHub repository About text when available, plus package requirements read from an optional **mads_package.json** file in the repository root.

**\-i**, **\-\-install** *package*
:  Install *package*. The selected ZIP asset is downloaded to a temporary directory and extracted into the MADS prefix. On macOS, architecture-specific assets such as **darwin-arm64** or **darwin-x86_64** are preferred over **darwin-universal** when both are present.

**\-f**, **\-\-force**
:  Force overwrite of existing files during installation. This option is only valid together with **\-\-install**.

**\-\-no-cache**
:  Fetch package data without reading cached results. The cache is still updated after a successful fetch, unless a GitHub HTTP 403 response was observed.
  
**\-\-json**
:  Output information in JSON format.

**\-v**, **\-\-version**
:  Show version information.

**\-h**, **\-\-help**
:  Show summary of options.

# PACKAGE INDEX

The package index is a JSON document maintained by the MADS project. It maps package names to GitHub repository URLs. Each package entry is expected to contain a **URI** field:

```
{
  "packages": {
    "example.plugin": {
      "URI": "https://github.com/MADS-NET/example_plugin"
    }
  }
}
```

Package names from this index are used as arguments to **\-\-info** and **\-\-install**.

# RELEASE SELECTION

**mads-package** first requests the latest stable GitHub release for a package repository. If GitHub reports that no stable latest release exists, the command fetches the release list and selects the newest non-draft pre-release by timestamp.

Only ZIP assets matching the current platform are shown or installed. Asset compatibility is determined from the filename suffix:

**windows-amd64.zip**
:  Windows 64-bit package.

**linux-x86_64.zip**, **linux-aarch64.zip**
:  Linux packages for the corresponding architecture.

**darwin-x86_64.zip**, **darwin-arm64.zip**
:  macOS packages for Intel and Apple Silicon machines.

**darwin-universal.zip**
:  macOS universal package. This is compatible with both Intel and Apple Silicon macOS systems and is used when no exact macOS architecture-specific package is selected.

# PACKAGE METADATA

Package repositories may contain a **mads_package.json** file in the root of the default branch. **mads-package \-\-info** reads this file when present and prints requirements for the current platform.

The file may define common and platform-specific notes and commands:

```
{
  "name": "example_plugin",
  "requirements": {
    "Common": {
      "note": [
        "Requires Python 3.8 or later"
      ],
      "commands": []
    },
    "macOS": {
      "note": [
        "May require removing quarantine attributes"
      ],
      "commands": [
        "xattr -d com.apple.quarantine $(mads -p)/lib/example.plugin"
      ]
    }
  }
}
```

The **Common** requirements are printed first. The requirements for the current platform are printed after the common entries. Platform section names are **Windows**, **Linux**, and **macOS**.

The **note** field is a list of strings describing requirements. The **commands** field is a list of shell commands to run after installation to meet requirements. Commands are printed for informational purposes but not executed by **mads-package**.

# CACHE

Cache files are stored in:

**$HOME/.mads/packages/**
:  Package index, release metadata, repository metadata, and package information cache.

On Windows, the home directory is resolved from **HOME**, then **USERPROFILE**, then **HOMEDRIVE** plus **HOMEPATH**.

The list cache is stored as **list_packages.json**. Per-package info cache files are stored as **info_**\[*package*\]**.json**. Cache files older than six hours are ignored.

# EXAMPLES

List available packages:

```
mads-package --list
```

Show package information and platform-specific requirements:

```
mads-package --info rerunner.plugin
```

Refresh package information without reading cache:

```
mads-package --info rerunner.plugin --no-cache
```

Install a package:

```
mads-package --install rerunner.plugin
```

Install a package and overwrite existing files:

```
mads-package --install rerunner.plugin --force
```

# DIAGNOSTICS

If GitHub returns HTTP 403 during a package operation, **mads-package** prints a warning that GitHub may be limiting traffic. Wait and try again later, or use cached results where appropriate.

If no compatible ZIP asset is found for the current platform, the package cannot be installed on that system until a matching release asset is published.

# FILES

**$HOME/.mads/packages/**: package cache directory.

**$(mads -p)**: MADS installation prefix and installation destination for package contents.

# BUGS

The upstream bug tracker can be found at https://github.com/pbosetti/MADS/issues.

# SEE ALSO

**mads**(1), **mads-plugin**(1), **mads-broker**(1), **mads-source**(1), **mads-sink**(1), **mads-filter**(1)

# AUTHOR

**mads-package** was written by Paolo Bosetti <paolo.bosetti@unitn.it>.

# LICENSE

https://www.apache.org/licenses/LICENSE-2.0
