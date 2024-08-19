#include <chrono>
#include <filesystem>
#include <functional>
#include <future>
#include <iostream>
#include <string>
#include <vector>
#include <thread>

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
#endif
  }

  static int getAnswer() {
    std::string answer;
    std::cin >> answer;
    return 0;
  }

  void watch(bool *running,
             const std::function<void(const std::string &)> &callback) {
    while (file_modified() == 0) {
      if (!*running) {
        break;
      }
    }
    if (*running)
      callback(_file_name);
  }

private:
  std::string _file_name;
  std::chrono::duration<float> _timeout;
  struct timespec _ts;
#if defined(__linux__)
  char _buffer[BUF_LEN];
  int _inotify_fd;
  int _watch;
#elif defined(__APPLE__)
  int _fd;
  int _kq = kqueue();
  struct kevent _change;
  struct kevent _event;
#elif defined(_WIN32)

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
    if (_timeout > 0s) { // non-blocking
      return kevent(_kq, &_change, 1, &_event, 1, &_ts);
    } else {             // blocking
      return kevent(_kq, &_change, 1, &_event, 1, NULL);
    }
#elif defined(_WIN32)
    // Use FindFirstChangeNotification to monitor file changes on Win32
    HANDLE hDir = FindFirstChangeNotification(fileName.c_str(), FALSE,
                                              FILE_NOTIFY_CHANGE_LAST_WRITE);
    WaitForSingleObject(hDir, INFINITE);
    FindCloseChangeNotification(hDir);
    return true;
#endif
  }
};
} // namespace Mads
