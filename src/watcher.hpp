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
    _ts.tv_sec =
        std::chrono::duration_cast<std::chrono::seconds>(_timeout).count();
    _ts.tv_nsec =
        std::chrono::duration_cast<std::chrono::nanoseconds>(_timeout).count() %
        1000000000;
    arm();
#elif defined(__linux__)
    _inotify_fd = inotify_init1(IN_NONBLOCK);
    arm();
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
    if (_fd >= 0) close(_fd);
    close(_kq);
#elif defined(__linux__)
    if (_watch >= 0) inotify_rm_watch(_inotify_fd, _watch);
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
  alignas(struct inotify_event) char _buffer[BUF_LEN];
  int _inotify_fd;
  int _watch = -1;
#elif defined(__APPLE__)
  struct timespec _ts;
  int _fd = -1;
  int _kq = kqueue();
  struct kevent _change;
  struct kevent _event;
#elif defined(_WIN32)
  DWORD _to;
  HANDLE _change_handle;
  std::chrono::steady_clock::time_point _last_change_time;
#endif

  // Both inotify and kqueue watch an inode, not a path. Editors and
  // Mads::Ini::FileSource save by writing a temporary file and renaming it
  // over the original, which replaces the inode: the old watch then never
  // fires again. So a delete/rename of the watched inode re-arms the watch on
  // the path (retrying while the path is missing) and counts as a change.
#if defined(__linux__)
  void arm() {
    _watch = inotify_add_watch(_inotify_fd, _file_name.c_str(),
                               IN_MODIFY | IN_DELETE_SELF | IN_MOVE_SELF);
  }
#elif defined(__APPLE__)
  void arm() {
    _fd = open(_file_name.c_str(), O_EVTONLY);
    if (_fd >= 0) {
      EV_SET(&_change, _fd, EVFILT_VNODE, EV_ADD | EV_ENABLE | EV_CLEAR,
             NOTE_WRITE | NOTE_EXTEND | NOTE_DELETE | NOTE_RENAME, 0,
             (void *)_file_name.c_str());
    }
  }
#endif

  int file_modified() {
#if defined(__linux__)
    if (_watch < 0) {
      // Replaced and not there yet: report the change once it reappears.
      arm();
      if (_watch < 0) {
        std::this_thread::sleep_for(_timeout);
      }
      return _watch >= 0 ? 1 : 0;
    }
    ssize_t rc = read(_inotify_fd, _buffer, BUF_LEN);
    if (rc < 0) {
      if (errno != EAGAIN) {
        perror("read");
      }
      std::this_thread::sleep_for(_timeout);
      return 0;
    }
    bool modified = false;
    bool replaced = false;
    bool removed = false; // the kernel already dropped the watch
    for (ssize_t i = 0; i < rc;) {
      const auto *event = reinterpret_cast<const struct inotify_event *>(_buffer + i);
      i += static_cast<ssize_t>(sizeof(struct inotify_event) + event->len);
      // Late events of a watch already replaced (typically its IN_IGNORED
      // arriving in a later read) must not tear down the current one, or
      // each re-arm would trigger the next.
      if (event->wd != _watch) {
        continue;
      }
      modified = modified || (event->mask & IN_MODIFY);
      replaced = replaced ||
                 (event->mask & (IN_DELETE_SELF | IN_MOVE_SELF | IN_IGNORED));
      removed = removed || (event->mask & IN_IGNORED);
    }
    if (replaced) {
      if (!removed) {
        inotify_rm_watch(_inotify_fd, _watch);
      }
      arm();
      return _watch >= 0 ? 1 : 0;
    }
    return modified ? 1 : 0;
#elif defined(__APPLE__)
    if (_fd < 0) {
      // Replaced and not there yet: report the change once it reappears.
      arm();
      if (_fd < 0) {
        std::this_thread::sleep_for(_timeout > 0s ? _timeout : 1s);
      }
      return _fd >= 0 ? 1 : 0;
    }
    // Bounded wait even with no timeout, so stop() stays responsive.
    struct timespec one_second{1, 0};
    const int rc = kevent(_kq, &_change, 1, &_event, 1,
                          _timeout > 0s ? &_ts : &one_second);
    if (rc > 0 && (_event.fflags & (NOTE_DELETE | NOTE_RENAME))) {
      close(_fd); // also removes the kevent
      arm();
      return _fd >= 0 ? 1 : 0;
    }
    return rc > 0 ? 1 : 0;
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
