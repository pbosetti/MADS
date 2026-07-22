/*
  __  __                        ___           _   
 |  \/  | ___  _ __   __ _  ___|_ _|_ __  ___| |_ 
 | |\/| |/ _ \| '_ \ / _` |/ _ \| || '_ \/ __| __|
 | |  | | (_) | | | | (_| | (_) | || | | \__ \ |_ 
 |_|  |_|\___/|_| |_|\__, |\___/___|_| |_|___/\__|
                     |___/                        
Internal header: NOT part of the installed SDK.

The LIB_HEADERS glob in the top-level CMakeLists is not recursive, so headers
under src/detail/ are deliberately kept out of the install set. Only files
compiled into MadsCore may include this.
*/
#pragma once

#include <mongocxx/instance.hpp>

namespace Mads {
namespace detail {

/**
 * @brief Return the process-wide mongocxx driver instance.
 *
 * `mongocxx::instance` must be constructed exactly once per process and must
 * outlive every other driver object; constructing a second one while the first
 * is alive throws `mongocxx::logic_error{k_cannot_recreate_instance}`. Holding
 * one as a class member therefore makes it impossible for two driver-using MADS
 * objects to coexist, so every translation unit that touches the driver shares
 * the single function-local static created here.
 *
 * The static is destroyed at process exit, after all clients built on top of it.
 *
 * @note This is `inline`, so all callers linked into the same binary share one
 * static. MadsCore is a single shared library, so that holds for MADS.
 */
inline mongocxx::instance &mongo_instance() {
  static mongocxx::instance instance{};
  return instance;
}

} // namespace detail
} // namespace Mads
