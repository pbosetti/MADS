// POSIX implementation of Mads::shm::SharedSegment (see shared_segment.hpp).
#ifndef _WIN32

#include "shm/shared_segment.hpp"
#include "detail/runtime_dir.hpp"

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <fstream>
#include <stdexcept>
#include <system_error>

#include <fcntl.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace Mads::shm {

namespace {

std::system_error os_error(std::string const &what, std::string const &name,
                           int err = errno) {
  return std::system_error(err, std::generic_category(),
                           what + " " + name);
}

void check_name(std::string const &name) {
  if (name.size() < 2 || name.size() > 31 || name[0] != '/' ||
      name.find('/', 1) != std::string::npos)
    throw std::invalid_argument("Invalid shared-memory segment name '" + name +
                                "'");
}

std::filesystem::path registry_dir() {
  return detail::runtime_dir() / "shm";
}

// Best effort: a missing registry entry only means the segment cannot be
// swept if this process dies without unlinking it.
void write_registry(std::string const &name) {
  try {
    std::error_code ec;
    std::filesystem::create_directories(registry_dir(), ec);
    std::ofstream f(registry_file(name), std::ios::trunc);
    f << current_pid() << "\n";
  } catch (...) {
  }
}

void remove_registry(std::string const &name) {
  try {
    std::error_code ec;
    std::filesystem::remove(registry_file(name), ec);
  } catch (...) {
  }
}

#ifdef __ANDROID__
// Bionic has no POSIX shared memory.
int shm_open(const char *, int, mode_t) {
  errno = ENOSYS;
  return -1;
}
int shm_unlink(const char *) {
  errno = ENOSYS;
  return -1;
}
#else
using ::shm_open;
using ::shm_unlink;
#endif

} // namespace

uint64_t current_pid() { return static_cast<uint64_t>(::getpid()); }

std::string make_segment_name() {
  static std::atomic<uint32_t> counter{0};
  char buf[32];
  std::snprintf(buf, sizeof(buf), "/mads_%08x%08x",
                static_cast<unsigned>(::getpid()),
                static_cast<unsigned>(counter.fetch_add(1)));
  return buf;
}

std::filesystem::path registry_file(std::string const &name) {
  return registry_dir() / (name.empty() || name[0] != '/' ? name
                                                          : name.substr(1));
}

size_t sweep_orphans() {
  size_t count = 0;
  try {
    std::error_code ec;
    for (auto const &entry :
         std::filesystem::directory_iterator(registry_dir(), ec)) {
      std::string seg = entry.path().filename().string();
      if (seg.rfind("mads_", 0) != 0)
        continue;
      long long pid = 0;
      {
        std::ifstream f(entry.path());
        if (!(f >> pid) || pid <= 0)
          continue; // being written, or garbage: leave it alone
      }
      if (::kill(static_cast<pid_t>(pid), 0) == 0 || errno != ESRCH)
        continue; // alive (or not ours to judge)
      std::string name = "/" + seg;
      if (shm_unlink(name.c_str()) == 0)
        ++count;
      std::filesystem::remove(entry.path(), ec);
    }
  } catch (...) {
  }
  return count;
}

std::unique_ptr<SharedSegment> SharedSegment::create(std::string const &name,
                                                     size_t size) {
  check_name(name);
  if (size == 0)
    throw std::invalid_argument("Shared-memory segment size must be > 0");
  sweep_orphans();

  int fd = shm_open(name.c_str(), O_CREAT | O_EXCL | O_RDWR, 0600);
  if (fd < 0)
    throw os_error("shm_open (create)", name);
  // macOS allows exactly one ftruncate on a shm object: size it once, here.
  if (::ftruncate(fd, static_cast<off_t>(size)) != 0) {
    int err = errno;
    ::close(fd);
    shm_unlink(name.c_str());
    throw os_error("ftruncate", name, err);
  }
  void *p = ::mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (p == MAP_FAILED) {
    int err = errno;
    ::close(fd);
    shm_unlink(name.c_str());
    throw os_error("mmap", name, err);
  }
  write_registry(name);
  return std::unique_ptr<SharedSegment>(
      new SharedSegment(name, p, size, fd));
}

std::unique_ptr<SharedSegment> SharedSegment::open(std::string const &name) {
  check_name(name);
  int fd = shm_open(name.c_str(), O_RDWR, 0);
  if (fd < 0)
    throw os_error("shm_open", name);
  struct stat st {};
  if (::fstat(fd, &st) != 0) {
    int err = errno;
    ::close(fd);
    throw os_error("fstat", name, err);
  }
  if (st.st_size <= 0) {
    ::close(fd);
    throw os_error("empty segment", name, EINVAL);
  }
  auto size = static_cast<size_t>(st.st_size);
  void *p = ::mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (p == MAP_FAILED) {
    int err = errno;
    ::close(fd);
    throw os_error("mmap", name, err);
  }
  return std::unique_ptr<SharedSegment>(
      new SharedSegment(name, p, size, fd));
}

SharedSegment::~SharedSegment() {
  if (_data)
    ::munmap(_data, _size);
  if (_handle >= 0)
    ::close(static_cast<int>(_handle));
}

void SharedSegment::unlink() {
  if (_unlinked)
    return;
  _unlinked = true;
  shm_unlink(_name.c_str());
  remove_registry(_name);
}

} // namespace Mads::shm

#endif // !_WIN32
