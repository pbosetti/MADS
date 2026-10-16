// Windows implementation of Mads::shm::SharedSegment (see shared_segment.hpp).
#ifdef _WIN32

#include "shm/shared_segment.hpp"

#include <atomic>
#include <cstdio>
#include <stdexcept>
#include <system_error>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace Mads::shm {

namespace {

std::system_error os_error(std::string const &what, std::string const &name,
                           DWORD err = GetLastError()) {
  return std::system_error(static_cast<int>(err), std::system_category(),
                           what + " " + name);
}

void check_name(std::string const &name) {
  if (name.size() < 2 || name.size() > 31 || name[0] != '/' ||
      name.find('/', 1) != std::string::npos ||
      name.find('\\') != std::string::npos)
    throw std::invalid_argument("Invalid shared-memory segment name '" + name +
                                "'");
}

// "/mads_x" -> L"Local\\mads_x". Names are ASCII by construction.
std::wstring section_name(std::string const &name) {
  std::wstring w = L"Local\\";
  for (char c : name.substr(1))
    w.push_back(static_cast<wchar_t>(static_cast<unsigned char>(c)));
  return w;
}

} // namespace

uint64_t current_pid() { return static_cast<uint64_t>(GetCurrentProcessId()); }

std::string make_segment_name() {
  static std::atomic<uint32_t> counter{0};
  char buf[32];
  std::snprintf(buf, sizeof(buf), "/mads_%08x%08x",
                static_cast<unsigned>(GetCurrentProcessId()),
                static_cast<unsigned>(counter.fetch_add(1)));
  return buf;
}

std::filesystem::path registry_file(std::string const &name) {
  // Not used on Windows; returned for API symmetry only.
  return std::filesystem::temp_directory_path() / "mads" / "shm" /
         (name.empty() || name[0] != '/' ? name : name.substr(1));
}

size_t sweep_orphans() { return 0; }

std::unique_ptr<SharedSegment> SharedSegment::create(std::string const &name,
                                                     size_t size) {
  check_name(name);
  if (size == 0)
    throw std::invalid_argument("Shared-memory segment size must be > 0");
  auto wname = section_name(name);
  auto sz = static_cast<uint64_t>(size);
  HANDLE h = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
                                static_cast<DWORD>(sz >> 32),
                                static_cast<DWORD>(sz & 0xFFFFFFFFu),
                                wname.c_str());
  if (!h)
    throw os_error("CreateFileMappingW", name);
  if (GetLastError() == ERROR_ALREADY_EXISTS) {
    CloseHandle(h);
    throw std::system_error(std::make_error_code(std::errc::file_exists),
                            "CreateFileMappingW " + name);
  }
  void *p = MapViewOfFile(h, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, size);
  if (!p) {
    DWORD err = GetLastError();
    CloseHandle(h);
    throw os_error("MapViewOfFile", name, err);
  }
  // Page-file-backed sections are zero-filled by the kernel.
  return std::unique_ptr<SharedSegment>(new SharedSegment(
      name, p, size, reinterpret_cast<intptr_t>(h)));
}

std::unique_ptr<SharedSegment> SharedSegment::open(std::string const &name) {
  check_name(name);
  auto wname = section_name(name);
  HANDLE h =
      OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, wname.c_str());
  if (!h)
    throw os_error("OpenFileMappingW", name);
  void *p = MapViewOfFile(h, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, 0);
  if (!p) {
    DWORD err = GetLastError();
    CloseHandle(h);
    throw os_error("MapViewOfFile", name, err);
  }
  MEMORY_BASIC_INFORMATION mbi{};
  if (VirtualQuery(p, &mbi, sizeof(mbi)) == 0) {
    DWORD err = GetLastError();
    UnmapViewOfFile(p);
    CloseHandle(h);
    throw os_error("VirtualQuery", name, err);
  }
  return std::unique_ptr<SharedSegment>(
      new SharedSegment(name, p, static_cast<size_t>(mbi.RegionSize),
                        reinterpret_cast<intptr_t>(h)));
}

SharedSegment::~SharedSegment() {
  if (_data)
    UnmapViewOfFile(_data);
  if (_handle != -1 && _handle != 0)
    CloseHandle(reinterpret_cast<HANDLE>(_handle));
}

// The kernel frees the section when its last handle closes.
void SharedSegment::unlink() { _unlinked = true; }

} // namespace Mads::shm

#endif // _WIN32
