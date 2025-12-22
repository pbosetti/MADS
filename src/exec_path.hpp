/*
  _____                             _   _
 | ____|_  _____  ___   _ __   __ _| |_| |__
 |  _| \ \/ / _ \/ __| | '_ \ / _` | __| '_ \
 | |___ >  <  __/ (__  | |_) | (_| | |_| | | |
 |_____/_/\_\___|\___| | .__/ \__,_|\__|_| |_|
                       |_|
* Calculate the path of an executable file.
*/
#include <string>
#include <filesystem>

#ifndef EXEC_PATH_HPP
#define EXEC_PATH_HPP

#if defined(_WIN32)
#include <Shlwapi.h>
#include <io.h>
#include <windows.h>
#define PATH_MAX MAX_PATH
#define access _access_s
#endif

#ifdef __APPLE__
#include <libgen.h>
#include <limits.h>
#include <mach-o/dyld.h>
#include <unistd.h>
#endif

#ifdef __linux__
#include <libgen.h>
#include <limits.h>
#include <unistd.h>

#if defined(__sun)
#define PROC_SELF_EXE "/proc/self/path/a.out"
#else
#define PROC_SELF_EXE "/proc/self/exe"
#endif

#endif

namespace Mads {

inline std::filesystem::path exec_path() {
  char raw_path[PATH_MAX];
#if defined(_WIN32)
  GetModuleFileNameA(NULL, raw_path, MAX_PATH);
  return std::string(raw_path);
#elif defined(__linux__) 
  realpath(PROC_SELF_EXE, raw_path);
#elif defined(__APPLE__)
  uint32_t rawPathSize = (uint32_t)sizeof(raw_path);
  _NSGetExecutablePath(raw_path, &rawPathSize);
#endif
  return std::filesystem::weakly_canonical(raw_path);
}

inline std::string exec_dir(std::string relative = "") {
  std::string path = exec_path().parent_path().string();
  if (relative != "") {
    return std::filesystem::weakly_canonical(path + "/" + relative).string();
  } else {
    return std::filesystem::weakly_canonical(path).string();
  }
}

inline std::string prefix() {
  return exec_dir("../");
}

} // namespace Mads


#endif // EXEC_PATH_HPP