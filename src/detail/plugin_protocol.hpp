/*
Internal helper: whether a plugin built for one mads_plugin protocol version
can be driven by a host built for another. Not part of the installed SDK.

pugg only rejects drivers whose version is *below* the server's minimum, so a
plugin built against newer mads_plugin headers would be accepted and then
crash: each protocol bump may add virtual methods, shifting the vtable (e.g.
a P9 plugin in a P8 host has load_data_view() where the host calls
set_params()). The plugin loaders therefore check the driver version, which
lives in the pugg::Driver base and is safe to read, before instantiating the
plugin.
*/
#pragma once

#include <string>

namespace Mads::detail {

/// "" if a plugin whose driver reports `plugin_version` can be loaded by a
/// host built for `host_version` (accepting down to `min_version`), or a
/// message telling the user what to rebuild or upgrade.
inline std::string plugin_protocol_mismatch(int plugin_version,
                                            int host_version,
                                            int min_version) {
  const std::string versions =
      "plugin built for protocol " + std::to_string(plugin_version) +
      ", this MADS supports " +
      (min_version < host_version ? std::to_string(min_version) + " to "
                                  : std::string()) +
      std::to_string(host_version);
  if (plugin_version > host_version)
    return versions + ": upgrade MADS, or rebuild the plugin against the "
                      "mads_plugin version matching this MADS";
  if (plugin_version < min_version)
    return versions + ": rebuild the plugin against a newer mads_plugin "
                      "(see `mads plugin --update`)";
  return {};
}

} // namespace Mads::detail
