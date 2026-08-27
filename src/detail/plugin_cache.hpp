/*
Internal helper: a content-addressed on-disk cache for broker-served
attachments (the OTA plugin delivery driven by the `attachment` INI key). Not
part of the installed SDK (src/detail/ is excluded from the LIB_HEADERS install
glob in CMakeLists.txt).

The problem it solves. The attachment used to land on a fixed path derived only
from the settings section name, `<temp>/mads/<name>.plugin`, and was written
with a truncating ofstream. That is fine for one agent, and fatal for several:
`mads-director`/`mads-up` with `scale > 1` start N instances of the same
section on one host, and instance 2 rewrote the very inode instance 1 had
already dlopen()'d. Rewriting a mapped file in place does not give the running
process a stale-but-valid copy -- it changes the pages under it, so instance 1
took a SIGBUS (or executed garbage) at its next page fault. Intermittent, and
it killed a process other than the one that misbehaved.

The fix is not "one copy per instance" but the narrower invariant that makes
that unnecessary: *the bytes at an in-use path never change*. Naming the file
after a digest of its own content gives exactly that, and invalidation falls
out for free -- a new binary on the broker hashes differently, so it lands on a
new path, and no instance has to work out whether it is the "first" one.

Layout:

    <temp>/mads/<section-name>/<digest16>/<section-name>.<ext>

The digest is a *directory* component and never part of the filename stem,
because both C++ plugin loaders fall back to the plugin file's stem for the
driver name when the settings section has no `driver` key (plugin_loader.cpp
"Driver: --driver > 'driver' setting > file stem", and the same in worker.cpp).
A digest in the stem would silently change the driver name every OTA agent that
relies on that fallback resolves. Scoping the digest directory under the
section name also keeps sweep_stale() honest: it can only ever remove that
agent's own older versions, never a sibling agent's cache.

The digest is a cache key over bytes that already arrived from a trusted (and,
under --crypto, CURVE-authenticated) broker -- it is not a security control and
does not need to be a cryptographic hash. FNV-1a keeps this header free of any
dependency, in the same spirit as the self-contained CRC-32 in bag.cpp.
*/
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

namespace Mads::detail {

// FNV-1a, 64-bit (offset basis 0xcbf29ce484222325, prime 0x100000001b3).
inline uint64_t fnv1a64(const void *data, size_t len) {
  const auto *p = static_cast<const unsigned char *>(data);
  uint64_t h = 0xcbf29ce484222325ull;
  for (size_t i = 0; i < len; ++i) {
    h ^= p[i];
    h *= 0x100000001b3ull;
  }
  return h;
}

// The digest as exactly 16 lowercase hex digits, so every cache directory name
// has the same width regardless of leading zeros.
inline std::string digest_hex(std::string_view bytes) {
  static constexpr char HEX[] = "0123456789abcdef";
  uint64_t h = fnv1a64(bytes.data(), bytes.size());
  std::string out(16, '0');
  for (int i = 15; i >= 0; --i) {
    out[static_cast<size_t>(i)] = HEX[h & 0xful];
    h >>= 4;
  }
  return out;
}

// Root of the attachment cache, shared by every agent on the host.
inline std::filesystem::path attachment_cache_root() {
  return std::filesystem::temp_directory_path() / "mads";
}

// Removes every version of `name`'s attachment except `keep_digest`, plus the
// flat `<temp>/mads/<name>.*` files left behind by MADS <= 2.4.2. Entirely
// best-effort: every call takes the error_code overload and ignores failures.
// Unlinking a mapped file is safe on POSIX (the inode outlives the last
// mapping), and on Windows removing a currently loaded DLL simply fails, which
// is equally harmless -- a still-running sibling keeps the copy it loaded.
inline void sweep_stale(const std::filesystem::path &agent_dir,
                        std::string_view keep_digest, const std::string &name,
                        const std::string &ext) {
  namespace fs = std::filesystem;
  std::error_code ec;

  for (fs::directory_iterator it(agent_dir, ec), end; !ec && it != end;
       it.increment(ec)) {
    if (it->path().filename() == keep_digest)
      continue;
    std::error_code rm_ec;
    fs::remove_all(it->path(), rm_ec);
  }

  // Pre-2.4.3 layout: one flat file per agent, directly under the cache root.
  // Both the ".plugin" the download always used and the settings-driven
  // extension it was then renamed to.
  const fs::path root = agent_dir.parent_path();
  std::error_code legacy_ec;
  fs::remove(root / (name + ".plugin"), legacy_ec);
  legacy_ec.clear();
  fs::remove(root / (name + "." + ext), legacy_ec);
}

/**
 * @brief Persist a broker-served attachment under its content digest and
 *        return the path to load it from.
 *
 * Safe to call concurrently from any number of processes and threads: the
 * bytes at the returned path are never mutated in place, so a copy another
 * process has already dlopen()'d stays exactly as that process mapped it.
 *
 * @param name Settings section name; also the returned file's stem.
 * @param ext Extension without the leading dot (e.g. "plugin").
 * @param bytes The attachment as received from the broker.
 * @return Absolute path to the cached file.
 * @throws std::runtime_error if the attachment cannot be persisted.
 */
inline std::filesystem::path store_attachment(const std::string &name,
                                              const std::string &ext,
                                              const std::string &bytes) {
  namespace fs = std::filesystem;

  const std::string digest = digest_hex(bytes);
  const fs::path agent_dir = attachment_cache_root() / name;
  const fs::path dir = agent_dir / digest;
  const fs::path target = dir / (name + "." + ext);

  // Already cached. This is the path every instance after the first takes when
  // an agent is scaled, so the common case does no I/O beyond a stat. A size
  // check is enough to reject a truncated leftover: the write below publishes
  // by rename, so a file at `target` is either complete or not there at all.
  std::error_code ec;
  if (fs::exists(target, ec) && fs::file_size(target, ec) == bytes.size()) {
    sweep_stale(agent_dir, digest, name, ext);
    return target;
  }

  fs::create_directories(dir, ec);
  if (!fs::is_directory(dir, ec)) {
    throw std::runtime_error(
        "Failed to create attachment cache directory " + dir.string() +
        (ec ? ": " + ec.message() : std::string{}));
  }

  // Stage in the destination directory so the publishing rename never crosses
  // a filesystem boundary. pid + counter keeps two processes -- or two threads
  // of one process -- from picking the same staging name.
  static std::atomic<uint64_t> seq{0};
#ifdef _WIN32
  const auto pid = static_cast<long long>(_getpid());
#else
  const auto pid = static_cast<long long>(getpid());
#endif
  const fs::path staged =
      dir / ("." + name + "." + std::to_string(pid) + "." +
             std::to_string(seq.fetch_add(1)) + ".tmp");

  {
    std::ofstream ofs(staged, std::ios::out | std::ios::binary);
    if (!ofs) {
      throw std::runtime_error("Failed to open " + staged.string() +
                               " for writing the attachment from the broker");
    }
    ofs.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    ofs.close();
    if (!ofs) {
      std::error_code rm_ec;
      fs::remove(staged, rm_ec);
      throw std::runtime_error(
          "Failed to write the attachment from the broker to " +
          staged.string());
    }
  }

  // Atomic publish: a reader either sees the previous file or this one, never
  // a partial write, and a process that mapped an earlier inode keeps it.
  ec.clear();
  fs::rename(staged, target, ec);
  if (ec) {
    // Lost a race with a concurrent writer, or -- on Windows -- the target is
    // a DLL some process still has loaded, which cannot be replaced. Both are
    // fine: content addressing means whatever sits at `target` has the same
    // digest as what we just staged, so it is the file we wanted.
    std::error_code check_ec;
    const bool usable = fs::exists(target, check_ec) &&
                        fs::file_size(target, check_ec) == bytes.size();
    std::error_code rm_ec;
    fs::remove(staged, rm_ec);
    if (!usable) {
      throw std::runtime_error("Failed to install the attachment at " +
                               target.string() + ": " + ec.message());
    }
  }

  sweep_stale(agent_dir, digest, name, ext);
  return target;
}

} // namespace Mads::detail
