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

// ASCII codes (key>0): 8 backspace, 9 tab, 10 newline, 27 escape, 127 delete,
// !"#$%&'()*+,-./0-9:;<=>?@A-Z[]^_`a-z{|}~üäÄöÖÜßµ´§°¹³² control key codes
// (key<0): -38/-40/-37/-39 up/down/left/right arrow, -33/-34 page up/down,
// -36/-35 pos1/end other key codes (key<0): -45 insert, -144 num lock, -20 caps
// lock, -91 windows key, -93 kontext menu key, -112 to -123 F1 to F12 not
// working: ¹ (251), num lock (-144), caps lock (-20), windows key (-91),
// kontext menu key (-93), F11 (-122)
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define VC_EXTRALEAN
#include <Windows.h>
using namespace std::chrono_literals;
char getch(std::chrono::milliseconds const &ms = 500ms) { // not working: F11 (-122, toggles fullscreen)
  KEY_EVENT_RECORD keyevent;
  INPUT_RECORD irec;
  DWORD events;
  while (true) {
    ReadConsoleInput(GetStdHandle(STD_INPUT_HANDLE), &irec, 1, &events);
    if (irec.EventType == KEY_EVENT &&
        ((KEY_EVENT_RECORD &)irec.Event).bKeyDown) {
      keyevent = (KEY_EVENT_RECORD &)irec.Event;
      const int ca = (int)keyevent.uChar.AsciiChar;
      const int cv = (int)keyevent.wVirtualKeyCode;
      const int key = ca == 0 ? -cv : ca + (ca > 0 ? 0 : 256);
      switch (key) {
      case -16:
        continue; // disable Shift
      case -17:
        continue; // disable Ctrl / AltGr
      case -18:
        continue; // disable Alt / AltGr
      case -220:
        continue; // disable first detection of "^" key (not "^" symbol)
      case -221:
        continue; // disable first detection of "`" key (not "`" symbol)
      case -191:
        continue; // disable AltGr + "#"
      case -52:
        continue; // disable AltGr + "4"
      case -53:
        continue; // disable AltGr + "5"
      case -54:
        continue; // disable AltGr + "6"
      case -12:
        continue; // disable num block 5 with num lock deactivated
      case 13:
        return 10; // enter
      case -46:
        return 127; // delete
      case -49:
        return 251; // ¹
      case 0:
        continue;
      case 1:
        continue; // disable Ctrl + a (selects all text)
      case 2:
        continue; // disable Ctrl + b
      case 3:
        continue; // disable Ctrl + c (terminates program)
      case 4:
        continue; // disable Ctrl + d
      case 5:
        continue; // disable Ctrl + e
      case 6:
        continue; // disable Ctrl + f (opens search)
      case 7:
        continue; // disable Ctrl + g
      // case    8: continue; // disable Ctrl + h (ascii for backspace)
      // case    9: continue; // disable Ctrl + i (ascii for tab)
      case 10:
        continue; // disable Ctrl + j
      case 11:
        continue; // disable Ctrl + k
      case 12:
        continue; // disable Ctrl + l
      // case   13: continue; // disable Ctrl + m (breaks console, ascii for new
      // line)
      case 14:
        continue; // disable Ctrl + n
      case 15:
        continue; // disable Ctrl + o
      case 16:
        continue; // disable Ctrl + p
      case 17:
        continue; // disable Ctrl + q
      case 18:
        continue; // disable Ctrl + r
      case 19:
        continue; // disable Ctrl + s
      case 20:
        continue; // disable Ctrl + t
      case 21:
        continue; // disable Ctrl + u
      case 22:
        continue; // disable Ctrl + v (inserts clipboard)
      case 23:
        continue; // disable Ctrl + w
      case 24:
        continue; // disable Ctrl + x
      case 25:
        continue; // disable Ctrl + y
      case 26:
        continue; // disable Ctrl + z
      default:
        return key; // any other ASCII/virtual character
      }
    }
  }
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