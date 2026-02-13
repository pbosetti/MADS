/*
  _  __
 | |/ /___ _   _ _ __  _ __ ___  ___ ___
 | ' // _ \ | | | '_ \| '__/ _ \/ __/ __|
 | . \  __/ |_| | |_) | | |  __/\__ \__ \
 |_|\_\___|\__, | .__/|_|  \___||___/___/
           |___/|_|

Read a single key press in a portable way.
*/

#include <iostream>
#include <string>
#include <thread> // contains <chrono>
#include <chrono>

static void kpsleep(const double t) {
  if (t > 0.0)
    std::this_thread::sleep_for(std::chrono::milliseconds((int)(1E3 * t + 0.5)));
}

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define VC_EXTRALEAN
#include <Windows.h>
using namespace std::chrono_literals;

char getch(std::chrono::milliseconds const &ms = 500ms) {
  HANDLE hStdin = GetStdHandle(STD_INPUT_HANDLE);
  DWORD dwMilliseconds = ms.count();
  
  // Save and modify console mode to disable line input
  DWORD dwMode = 0;
  GetConsoleMode(hStdin, &dwMode);
  DWORD newMode = dwMode & ~ENABLE_LINE_INPUT;
  SetConsoleMode(hStdin, newMode);
  
  DWORD result = WaitForSingleObject(hStdin, dwMilliseconds);
  
  char ch = '\0';
  if (result == WAIT_OBJECT_0) {
    DWORD dwRead;
    if (ReadFile(hStdin, &ch, 1, &dwRead, NULL) && dwRead == 1) {
      // Successfully read character
    }
  }
  
  // Restore original console mode
  SetConsoleMode(hStdin, dwMode);
  
  return ch;
}

#elif defined(__linux__) || defined(__unix__) || defined(__APPLE__)
#include <sys/ioctl.h>
#include <termios.h>
char getch(chrono::milliseconds const &ms = 500ms) {
  struct timeval tv;
  Mads::milliseconds_to_tv(ms, tv);
  struct termios oldt, newt;
  char ch;
  fd_set readfds;
  // struct timeval tv;

  tcgetattr(STDIN_FILENO, &oldt);
  newt = oldt;
  newt.c_lflag &= ~(ICANON | ECHO);

  tcsetattr(STDIN_FILENO, TCSANOW, &newt);

  // Set up file descriptor set for stdin
  FD_ZERO(&readfds);
  FD_SET(STDIN_FILENO, &readfds);

  int select_result = select(STDIN_FILENO + 1, &readfds, NULL, NULL, &tv);

  if (select_result > 0 && FD_ISSET(STDIN_FILENO, &readfds)) {
    ch = getchar();
  } else {
    ch = '\0'; // timeout or error
  }

  tcsetattr(STDIN_FILENO, TCSANOW, &oldt);

  return ch;
}
#endif // Windows/Linux