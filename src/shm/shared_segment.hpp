/*
Internal helper: a named, OS-level shared-memory segment, the storage under the
shared-memory data plane (see src/shm/shared_ring.hpp and src/mads_shm.hpp).
Not part of the installed SDK (src/shm/ is excluded from the LIB_HEADERS
install glob in CMakeLists.txt).

  POSIX    shm_open(O_CREAT|O_EXCL|O_RDWR, 0600) + one ftruncate + mmap.
           macOS cannot resize a shm object after the first ftruncate, so the
           size is fixed at creation.
  Windows  CreateFileMappingW on the paging file, named "Local\<name>".

ERRORS. create() and open() throw std::system_error (with the OS error code)
on failure; a name collision on create() is reported with
std::errc::file_exists, so callers can retry with a fresh name. Nothing else
in this file throws, except std::invalid_argument for a bad name or size.

LIFETIME. The destructor unmaps and closes, but never removes the name: only
the producer that created a segment calls unlink(). On POSIX the name lives
until shm_unlink() (existing mappings stay valid after it); on Windows the
kernel frees the section when its last handle closes, so unlink() is a no-op.

ORPHANS (POSIX). A producer that dies without unlinking leaves its segment in
/dev/shm (Linux) or the kernel (macOS) until reboot. To reclaim them, create()
writes a registry file runtime_dir()/shm/<name without slash> holding the
owner pid, unlink() removes it, and sweep_orphans() unlinks every registered
segment whose owner is dead. create() runs a sweep first; `mads doctor` may
call it too. On Windows the registry is not used and sweep_orphans() is a
no-op.

THREAD SAFETY. A SharedSegment is immutable after construction except for
unlink(), which is idempotent but not synchronized: call it from one thread.
*/
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace Mads::shm {

/// Current process id, as stored in the registry and the ring header.
uint64_t current_pid();

/// A fresh segment name, "/mads_<8 hex pid><8 hex counter>": 22 chars, well
/// within macOS's PSHMNAMLEN (31). Unique within this process.
std::string make_segment_name();

/// Directory of the segment registry, runtime_dir()/shm (POSIX only; on
/// Windows the returned path is never created).
std::filesystem::path registry_dir();

/// Registry file recording the owner of segment `name` (POSIX only; on
/// Windows the returned path is never created).
std::filesystem::path registry_file(std::string const &name);

/// True if process `pid` may still be running: POSIX kill(pid, 0), where
/// EPERM counts as alive (not ours to judge), as does pid <= 0. Always true
/// on Windows.
bool pid_alive(int64_t pid);

/// One registry file: a segment and the pid that created it.
struct RegistryEntry {
  std::string name;  ///< segment name without the leading '/'
  int64_t pid = -1;  ///< -1 if unreadable (being written, or garbage)
  bool alive = false; ///< is_alive(pid), false when pid is -1

  /// A segment whose owner is known to be dead.
  bool orphan() const { return pid > 0 && !alive; }
};

/// Lists the `mads_*` files in `dir` (a registry directory), sorted by name.
/// A missing directory yields an empty list. `is_alive` decides whether a
/// pid still runs (injectable for tests). Never throws.
std::vector<RegistryEntry>
scan_registry(std::filesystem::path const &dir,
              std::function<bool(int64_t)> const &is_alive = pid_alive);

/// Unlinks registered segments whose owner process is dead and removes their
/// registry files. Returns how many segments it unlinked. Never throws.
/// Windows: no-op, returns 0.
size_t sweep_orphans();

class SharedSegment {
public:
  /// Creates a new segment of exactly `size` bytes (> 0), zero-filled, mapped
  /// read-write, and registers this process as its owner. `name` must start
  /// with '/', contain no other '/', and be at most 31 chars.
  static std::unique_ptr<SharedSegment> create(std::string const &name,
                                               size_t size);

  /// Maps an existing segment read-write. size() is the size reported by the
  /// OS, which may be rounded up to a whole page.
  static std::unique_ptr<SharedSegment> open(std::string const &name);

  SharedSegment(SharedSegment const &) = delete;
  SharedSegment &operator=(SharedSegment const &) = delete;
  ~SharedSegment();

  void *data() const { return _data; }
  size_t size() const { return _size; }
  std::string const &name() const { return _name; }

  /// Removes the name (POSIX shm_unlink) and the registry file, so that no
  /// new process can open the segment; this mapping stays valid. Idempotent.
  /// Windows: no-op.
  void unlink();

private:
  SharedSegment(std::string name, void *data, size_t size, intptr_t handle)
      : _name(std::move(name)), _data(data), _size(size), _handle(handle) {}

  std::string _name;
  void *_data = nullptr;
  size_t _size = 0;
  intptr_t _handle = -1; ///< POSIX fd, or Windows HANDLE
  bool _unlinked = false;
};

} // namespace Mads::shm
