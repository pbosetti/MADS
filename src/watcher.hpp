#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>
#include <future>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "mads.hpp"

#if defined(__linux__)
#include <limits.h>
#include <sys/inotify.h>
#include <unistd.h>
#define BUF_LEN (10 * (sizeof(struct inotify_event) + NAME_MAX + 1))
#elif defined(__APPLE__)
#include <fcntl.h>
#include <sys/event.h>
#include <unistd.h>
#elif defined(_WIN32)
#include <windows.h>
#include <fileapi.h>
#endif

namespace fs = std::filesystem;
using namespace std::chrono_literals;

namespace Mads {
class Watcher {
public:
  Watcher(const std::string &file_name, std::chrono::duration<float> to = 0s)
      : _file_name(file_name), _timeout(to) {
#if defined(__APPLE__)
    _fd = open(_file_name.c_str(), O_EVTONLY);
    _ts.tv_sec =
        std::chrono::duration_cast<std::chrono::seconds>(_timeout).count();
    _ts.tv_nsec =
        std::chrono::duration_cast<std::chrono::nanoseconds>(_timeout).count() %
        1000000000;
    EV_SET(&_change, _fd, EVFILT_VNODE, EV_ADD | EV_ENABLE | EV_CLEAR,
           NOTE_WRITE, 0, (void *)_file_name.c_str());
#elif defined(__linux__)
    _inotify_fd = inotify_init1(IN_NONBLOCK);
    _watch = inotify_add_watch(_inotify_fd, _file_name.c_str(), IN_MODIFY);
#elif defined(_WIN32)
    _to =
        std::chrono::duration_cast<std::chrono::milliseconds>(_timeout).count();
    // Extract directory from file path for monitoring
    fs::path file_path(_file_name);
    std::string dir_path = file_path.parent_path().string();
    if (dir_path.empty()) dir_path = ".";
    _change_handle = FindFirstChangeNotificationA(
        dir_path.c_str(), FALSE, FILE_NOTIFY_CHANGE_LAST_WRITE);
    _last_change_time = std::chrono::steady_clock::now() - std::chrono::seconds(1);
#endif
  }

  ~Watcher() {
#if defined(__APPLE__)
    close(_fd);
    close(_kq);
#elif defined(__linux__)
    inotify_rm_watch(_inotify_fd, _watch);
    close(_inotify_fd);
#elif defined(_WIN32)
    FindCloseChangeNotification(_change_handle);
#endif
  }


  /**
   * @brief Watch the file, invoking the callback on each modification.
   *
   * Blocks the calling thread until stop() is called (typically from another
   * thread) or a process-wide stop is requested (Mads::Runtime::stop_process(),
   * e.g. on SIGINT). Platform waits are bounded, so the loop notices a stop
   * request within about a second.
   */
  void watch(const std::function<void(const std::string &)> &callback) {
    _watching = true;
    while (_watching && Mads::Runtime::process_running()) {
      if (file_modified()) callback(_file_name);
      std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
  }

  /**
   * @brief Ask a running watch() loop to return.
   *
   * Safe to call from any thread; watch() returns within its current wait
   * (bounded to about a second) plus the 200 ms poll interval.
   */
  void stop() { _watching = false; }

private:
  std::string _file_name;
  std::chrono::duration<float> _timeout;
  std::atomic<bool> _watching{false};
#if defined(__linux__)
  char _buffer[BUF_LEN];
  int _inotify_fd;
  int _watch;
#elif defined(__APPLE__)
  struct timespec _ts;
  int _fd;
  int _kq = kqueue();
  struct kevent _change;
  struct kevent _event;
#elif defined(_WIN32)
  DWORD _to;
  HANDLE _change_handle;
  std::chrono::steady_clock::time_point _last_change_time;
#endif

  int file_modified() {
#if defined(__linux__)
    // Use inotify to monitor file changes on Linux
    int rc = read(_inotify_fd, _buffer, BUF_LEN);
    if (rc < 0 && errno == EAGAIN) {
      std::this_thread::sleep_for(_timeout);
      return 0;
    }
    if (rc < 0 && errno != EAGAIN) {
      perror("read");
    }
    return rc;
#elif defined(__APPLE__)
    if (_timeout > 0s) {
      return kevent(_kq, &_change, 1, &_event, 1, &_ts);
    } else {
      // Bounded wait instead of blocking forever, so stop() stays responsive.
      struct timespec ts{1, 0};
      return kevent(_kq, &_change, 1, &_event, 1, &ts);
    }
#elif defined(_WIN32)
    // Debounce: only report a change if enough time has passed since the last one
    auto now = std::chrono::steady_clock::now();
    // Bounded wait instead of INFINITE, so stop() stays responsive.
    if (WaitForSingleObject(_change_handle, _to > 0 ? _to : 1000) == WAIT_OBJECT_0) {
      if (now - _last_change_time > std::chrono::milliseconds(1000)) {
        _last_change_time = now;
        FindNextChangeNotification(_change_handle);  // Re-arm for next change
        return 1;
      } else {
        FindNextChangeNotification(_change_handle);  // Re-arm for next change
        return 0;  // Suppress duplicate
      }
    } else {
      return 0;
    }
#endif
  }
};
} // namespace Mads
