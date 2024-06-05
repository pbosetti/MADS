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
using namespace std;

static void kpsleep(const double t) {
  if (t > 0.0)
    this_thread::sleep_for(chrono::milliseconds((int)(1E3 * t + 0.5)));
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
int key_press() { // not working: F11 (-122, toggles fullscreen)
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
int key_press() { // not working: ¹ (251), num lock (-144), caps lock (-20),
                  // windows key (-91), kontext menu key (-93)
  struct termios term;
  tcgetattr(0, &term);
  while (true) {
    term.c_lflag &= ~(ICANON | ECHO); // turn off line buffering and echoing
    tcsetattr(0, TCSANOW, &term);
    int nbbytes;
    ioctl(0, FIONREAD, &nbbytes); // 0 is STDIN
    while (!nbbytes) {
      kpsleep(0.01);
      fflush(stdout);
      ioctl(0, FIONREAD, &nbbytes); // 0 is STDIN
    }
    int key = (int)getchar();
    if (key == 27 || key == 194 ||
        key == 195) { // escape, 194/195 is escape for °ß´äöüÄÖÜ
      key = (int)getchar();
      if (key == 91) {               // [ following escape
        key = (int)getchar();        // get code of next char after \e[
        if (key == 49) {             // F5-F8
          key = 62 + (int)getchar(); // 53, 55-57
          if (key == 115)
            key++;              // F5 code is too low by 1
          getchar();            // take in following ~ (126), but discard code
        } else if (key == 50) { // insert or F9-F12
          key = (int)getchar();
          if (key == 126) { // insert
            key = 45;
          } else {     // F9-F12
            key += 71; // 48, 49, 51, 52
            if (key < 121)
              key++;   // F11 and F12 are too low by 1
            getchar(); // take in following ~ (126), but discard code
          }
        } else if (key == 51 || key == 53 ||
                   key == 54) { // delete, page up/down
          getchar();            // take in following ~ (126), but discard code
        }
      } else if (key == 79) {      // F1-F4
        key = 32 + (int)getchar(); // 80-83
      }
      key = -key; // use negative numbers for escaped keys
    }
    term.c_lflag |= (ICANON | ECHO); // turn on line buffering and echoing
    tcsetattr(0, TCSANOW, &term);
    switch (key) {
    case 127:
      return 8; // backspace
    case -27:
      return 27; // escape
    case -51:
      return 127; // delete
    case -164:
      return 132; // ä
    case -182:
      return 148; // ö
    case -188:
      return 129; // ü
    case -132:
      return 142; // Ä
    case -150:
      return 153; // Ö
    case -156:
      return 154; // Ü
    case -159:
      return 225; // ß
    case -181:
      return 230; // µ
    case -167:
      return 245; // §
    case -176:
      return 248; // °
    case -178:
      return 253; // ²
    case -179:
      return 252; // ³
    case -180:
      return 239; // ´
    case -65:
      return -38; // up arrow
    case -66:
      return -40; // down arrow
    case -68:
      return -37; // left arrow
    case -67:
      return -39; // right arrow
    case -53:
      return -33; // page up
    case -54:
      return -34; // page down
    case -72:
      return -36; // pos1
    case -70:
      return -35; // end
    case 0:
      continue;
    case 1:
      continue; // disable Ctrl + a
    case 2:
      continue; // disable Ctrl + b
    case 3:
      continue; // disable Ctrl + c (terminates program)
    case 4:
      continue; // disable Ctrl + d
    case 5:
      continue; // disable Ctrl + e
    case 6:
      continue; // disable Ctrl + f
    case 7:
      continue; // disable Ctrl + g
    case 8:
      continue; // disable Ctrl + h
    // case    9: continue; // disable Ctrl + i (ascii for tab)
    // case   10: continue; // disable Ctrl + j (ascii for new line)
    case 11:
      continue; // disable Ctrl + k
    case 12:
      continue; // disable Ctrl + l
    case 13:
      continue; // disable Ctrl + m
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
      continue; // disable Ctrl + v
    case 23:
      continue; // disable Ctrl + w
    case 24:
      continue; // disable Ctrl + x
    case 25:
      continue; // disable Ctrl + y
    case 26:
      continue; // disable Ctrl + z (terminates program)
    default:
      return key; // any other ASCII character
    }
  }
}
#endif // Windows/Linux