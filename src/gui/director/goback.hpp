#pragma once

#include <algorithm>
#include <cstddef>
#include <iostream>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace Mads {

class GoBack {
public:
  explicit GoBack(std::size_t lines) : _lines(lines) {}

  std::size_t lines() const noexcept { return _lines; }

private:
  std::size_t _lines;

  friend std::ostream &operator<<(std::ostream &os, const GoBack &cmd) {
    if (cmd._lines == 0U) {
      return os;
    }

#if defined(_WIN32)
    HANDLE handle = invalid_handle();
    if (&os == &std::cout) {
      handle = ::GetStdHandle(STD_OUTPUT_HANDLE);
    } else if (&os == &std::cerr || &os == &std::clog) {
      handle = ::GetStdHandle(STD_ERROR_HANDLE);
    }

    if (handle != invalid_handle()) {
      if (try_enable_vt(handle)) {
        write_ansi(os, cmd._lines);
        return os;
      }

      if (clear_with_console_api(handle, cmd._lines)) {
        return os;
      }
    }
#endif

    write_ansi(os, cmd._lines);
    return os;
  }

#if defined(_WIN32)
  static HANDLE invalid_handle() noexcept {
    return INVALID_HANDLE_VALUE;
  }

  static bool try_enable_vt(HANDLE handle) {
    DWORD mode = 0;
    if (!::GetConsoleMode(handle, &mode)) {
      return false;
    }

    if ((mode & ENABLE_VIRTUAL_TERMINAL_PROCESSING) != 0) {
      return true;
    }

    return ::SetConsoleMode(handle, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING) != 0;
  }

  static bool clear_with_console_api(HANDLE handle, std::size_t lines) {
    CONSOLE_SCREEN_BUFFER_INFO info{};
    if (!::GetConsoleScreenBufferInfo(handle, &info)) {
      return false;
    }

    const SHORT width = info.dwSize.X;
    if (width <= 0) {
      return false;
    }

    SHORT cursor_y = info.dwCursorPosition.Y;
    SHORT first_row = static_cast<SHORT>(std::max<int>(0, static_cast<int>(cursor_y) - static_cast<int>(lines)));

    for (SHORT row = static_cast<SHORT>(cursor_y - 1); row >= first_row; --row) {
      DWORD written = 0;
      COORD row_start{0, row};
      if (!::FillConsoleOutputCharacterA(handle, ' ', static_cast<DWORD>(width), row_start, &written)) {
        return false;
      }
      if (!::FillConsoleOutputAttribute(handle, info.wAttributes, static_cast<DWORD>(width), row_start, &written)) {
        return false;
      }
      if (row == 0) {
        break;
      }
    }

    COORD target{0, first_row};
    if (!::SetConsoleCursorPosition(handle, target)) {
      return false;
    }

    return true;
  }
#endif

  static void write_ansi(std::ostream &os, std::size_t lines) {
    for (std::size_t i = 0; i < lines; ++i) {
      os << "\x1b[1A\x1b[2K";
      if (i + 1U < lines) {
        os << '\r';
      }
    }
    os << '\r' << std::flush;
  }
};

inline GoBack goback(std::size_t lines) {
  return GoBack(lines);
}

} // namespace Mads
