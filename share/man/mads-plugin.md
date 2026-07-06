
# NAME

**mads-plugin** - The plugin stub creator for MADS

# SYNOPSIS

**mads-plugin** 
  [**\-t, \-\-type** *plugin type*]
  [**\-d, \-\-dir** *output directory*]
  [**\-i, \-\-install-dir** *install directory*]
  [**\-o, \-\-overwrite**]
  [**\-r, \-\-rust**]
  [**\-s, \-\-datastore**]
  [**\-v, \-\-version**]
  [**\-h, \-\-help**]
  [*plugin name*]

**mads-plugin** **\-\-update**
  [**\-d, \-\-dir** *plugin directory*]
  [**\-\-dry-run**]
  [**\-\-no-check**]
  [**\-\-from** *protocol*]
  [**\-\-to** *protocol*]
  [*plugin directory*]

# DESCRIPTION

**mads-plugin** creates a stub for implementing a new MADS plugin, either
in C++ (default) or in Rust (**\-\-rust**). With **\-\-update** it instead
migrates an *existing* C++ plugin to the current plugin protocol (see
**Migrating an existing plugin** below).

## C++ plugins

The generated project uses CMake and the pugg plugin kernel. The main source
file (*plugin_name*.cpp) compiles to a shared library (*plugin_name*.plugin)
that is loaded at runtime by **mads-source**, **mads-filter**, or **mads-sink**.

## Rust plugins

With **\-\-rust**, the generated project is a Cargo workspace that produces a
cdylib shared library. Rust plugins implement one of three traits from the
**mads-plugin** crate (**SourcePlugin**, **FilterPlugin**, or **SinkPlugin**)
and export a single C ABI symbol via the **export_*_plugin!** macro. The
library is loaded by the dedicated Rust loaders **mads-rsource**,
**mads-rfilter**, or **mads-rsink**, which mirror the behaviour of their C++
counterparts. No C++ toolchain is required to write or build a Rust plugin.

## Migrating an existing plugin

When the plugin harness bumps the plugin protocol version (the **\-P**\ *N*
suffix of the **mads_plugin** git tag, e.g. **v2.3-P7**), older C++ plugins must
update their pinned **GIT_TAG** and adapt their overridden methods to the new
base-class signatures. **mads plugin \-\-update** *dir* automates this:

1. it detects the plugin's current protocol from its **CMakeLists.txt**;
2. it chains the migration steps in **share/plugin_migrations** up to the current
   protocol (from **share/plugin_deps.json**), bumping the **GIT_TAG** and
   rewriting the affected method signatures. Signatures are located
   structurally (balanced parentheses), so reformatted or multi-line
   declarations migrate correctly;
3. it saves each modified file as *file*.**bak**, prints a change report and a
   manual follow-up checklist, then (unless **\-\-no-check**) compiles the
   migrated plugin so the compiler flags anything the rewrite could not handle.

Rust plugins are not handled by **\-\-update**: they track the **mads-plugin**
crate version in **Cargo.toml** rather than the **\-P**\ *N* tag.

# ARGUMENTS

*plugin name*
:  The name of the plugin to be created (typically lower-case with underscores).
   For C++ plugins it becomes the main source file (*plugin_name*.cpp) and the
   shared object (*plugin_name*.plugin).  For Rust plugins it becomes the Cargo
   package name and the cdylib file (*libplugin_name*.so / *.dylib*).

# OPTIONS

**\-t**, **\-\-type** **source|filter|sink**
:  The plugin type. Must be one of **source**, **filter**, or **sink**.
   The default is **source**.

**\-d**, **\-\-dir** *path*
:  The output directory where the new project will be created. Defaults to a
   directory named after the plugin under the current working directory.
   Existing files are never overwritten unless **\-o** is given.

**\-i**, **\-\-install-dir** *path*
:  Install prefix used in the generated build files. Defaults to **/usr/local**.
   For C++ plugins this sets the CMake install prefix. For Rust plugins this
   path is written into **Cargo.toml** as the local path to the **mads-plugin**
   crate (*install-dir*/share/rust/mads-plugin).

**\-o**, **\-\-overwrite**
:  Overwrite any already existing files (default: false).

**\-r**, **\-\-rust**
:  Generate a Rust plugin project instead of a C++ one. Produces
   **Cargo.toml**, **src/lib.rs**, and **README.md**. The **\-\-datastore**
   option is ignored when this flag is set.

**\-s**, **\-\-datastore**
:  Enable datastore support in the generated C++ plugin (default: false).
   Not applicable to Rust plugins.

**\-u**, **\-\-update**
:  Migrate an existing C++ plugin instead of scaffolding a new one. The plugin
   directory is taken from **\-\-dir** or the positional argument (default: the
   current directory). See **Migrating an existing plugin** above.

**\-\-dry-run**
:  With **\-\-update**: report the changes that would be made without writing any
   file.

**\-\-no-check**
:  With **\-\-update**: skip the post-migration compile check.

**\-\-from** *protocol*
:  With **\-\-update**: override the auto-detected source protocol number.

**\-\-to** *protocol*
:  With **\-\-update**: migrate up to this protocol number instead of the current
   one from **share/plugin_deps.json**.

**\-v**, **\-\-version**
:  Show version information.

**\-h**, **\-\-help**
:  Show summary of options.

# EXAMPLES

Create a C++ filter plugin:

```
mads plugin my_filter --type filter
cd my_filter && cmake -Bbuild && cmake --build build
mads filter my_filter.plugin
```

Create a Rust filter plugin:

```
mads plugin my_filter --type filter --rust
cd my_filter && cargo build --release
mads-rfilter target/release/libmy_filter.so
```

Preview a protocol migration of an existing plugin, then apply it:

```
mads plugin --update ./my_filter --dry-run
mads plugin --update ./my_filter
```

# BUGS

The upstream bug tracker can be found at https://github.com/pbosetti/MADS/issues.

# SEE ALSO

**mads**(1), **mads-broker**(1), **mads-logger**(1),
**mads-source**(1), **mads-filter**(1), **mads-sink**(1),
**mads-rsource**(1), **mads-rfilter**(1), **mads-rsink**(1)

# FILES

**/usr/local/share/templates**: directory containing C++ and Rust plugin templates.

**/usr/local/share/rust/mads-plugin**: source of the **mads-plugin** Rust crate.

**/usr/local/share/plugin_migrations**: JSON migration definitions used by **\-\-update**.

**/usr/local/share/plugin_deps.json**: dependency version manifest pinned into scaffolded C++ plugins and used as the default **\-\-update** target.

# AUTHOR

**mads-plugin** was written by Paolo Bosetti <paolo.bosetti@unitn.it>.

# LICENSE

https://www.apache.org/licenses/LICENSE-2.0
