
# NAME

**mads-plugin** - The plugin stub creator for MADS

# SYNOPSIS

**mads-plugin** 
  [**\-t, \-\-type** *plugin type*]
  [**\-d, \-\-dir** *output directory*]
  [**\-i, \-\-install-dir** *install directory*]
  [**\-o, \-\-overwrite**]
  [**\-s, \-\-datastore**]
  [**\-v, \-\-version**]
  [**\-h, \-\-help**]
  [*plugin name*]

# DESCRIPTION

**mads-plugin** creates a stub for implementing a new MADS plugin.

# ARGUMENTS

*plugin name*
:  The official name of the plugin to be created. This is typically all lower-case. It will be the name of the main source file (*plugin_name*.cpp) and the plugin shared object file (*plugin_name*.plugin).

# OPTIONS

**\-t**, **\-\-type** **source|filter|sink**
:  The plugin type. It must be one of the following: **source**, **filter**, **sink**. The default is **source**.

**\-d**, **\-\-dir** *path*
:  The output directory where new files will be created. The default is the directory named **plugin** under the current directory. Existing files will never be overwritten (see **\-o** option).

**\-i**, **\-\-install-dir** *path*
:  The CMake install prefix. The default is **/usr/local**. The plugin will be installed in this directory when the **install** target is called in the plugin's CMake (i.e. cmake --install build).

**\-o**, **\-\-overwrite** 
:  Overwrite any already existing file (default: false).

**\-s**, **\-\-datastore**
:  Enable datastore support (default: false).

**\-v**, **\-\-version**
: show version information.

**\-h**, **\-\-help**
:  show summary of options.

# BUGS

The upstream bug tracker can be found at https://github.com/pbosetti/MADS/issues.

# SEE ALSO

**mads**(1), **mads-broker**(1), **mads-logger**(1), **mads-source**(1), **mads-filter**(1), **mads-sink**(1)

# FILES

**/usr/local/share/templates**: directory containing templates.

# AUTHOR

**mads-plugin** was written by Paolo Bosetti <paolo.bosetti@unitn.it>.

# LICENSE

https://creativecommons.org/licenses/by-sa/4.0/