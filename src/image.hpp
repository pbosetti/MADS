/*
  ___                                   _
 |_ _|_ __ ___   __ _  __ _  ___    ___| | __ _ ___ ___
  | || '_ ` _ \ / _` |/ _` |/ _ \  / __| |/ _` / __/ __|
  | || | | | | | (_| | (_| |  __/ | (__| | (_| \__ \__ \
 |___|_| |_| |_|\__,_|\__, |\___|  \___|_|\__,_|___/___/
                      |___/

This class is a pure frontend class: it is expected to stream an image as a
binary blob

Author(s): Paolo Bosetti
*/

#ifndef IMAGE_HPP
#define IMAGE_HPP


// #include "mads.hpp"
#include "agent.hpp"
#include <fcntl.h>
#ifdef __APPLE__
#include <sys/event.h>
#elif __linux__
#include <errno.h>
#include <sys/inotify.h>
#define EVENT_BUF_LEN sizeof(struct inotify_event) + NAME_MAX + 1
#else
#error "Unsupported platform"
#endif

using json = nlohmann::json;

namespace Mads {

typedef struct {
  unsigned int id;
  char topic[1024];
  char path[1024];
} image_data_t;

/**
 * @brief The RFID class represents metadata for an agent.
 *
 * This class inherits from the Agent class and provides additional
 * functionality for loading settings and managing metadata.
 */
class Image : public Agent {
public:
  /**
   * @brief Constructs a Image object with the given name and settings path.
   *
   * @param name The name of the Image object.
   * @param settings_path The path to the settings file.
   */
  Image(std::string name, std::string settings_path)
      : Agent(name, settings_path) {
#ifdef __linux__
    if (_infd < 0)
      throw runtime_error("Could not init inotify system: " +
                          string(strerror(errno)));
#endif
  }

  ~Image() {
    for (auto fd : _fds)
      close(fd);
    for (auto data : _image_data)
      free(data);
#ifdef __APPLE__
    free(_events);
    close(_kq);
#elif __linux__
    for (auto &[fd, fn] : _inwds) {
      inotify_rm_watch(_infd, fd);
    }
    close(_infd);
#endif
  }

  void info(ostream &out = cout) override {
    Agent::info(out);
    out << "  Watched files:    " << style::bold;
    for (auto &[k, v] : _watch_list) {
      out << k << ": " << v << "; ";
    }
    out << style::reset << endl;
  }

  /**
   * @brief Blocks until a write event happens on one of the watched files, 
   * then publishes the binary object.
  */
  void publish_change() {
    string topic, path, type;

#ifdef __APPLE__
    struct kevent change;
    image_data_t *data;
    if (kevent(_kq, NULL, 0, &change, 1, NULL) == -1) {
      exit(1);
    }
    data = (image_data_t *)change.udata;
    topic = data->topic;
    path = data->path;
    type = path.substr(path.find_last_of(".") + 1);
    cout << "Change detected to image '" << topic << "' at " << path
         << " (type " << type << ")" << endl;

#elif __linux__
    ssize_t length;
    char buffer[EVENT_BUF_LEN];
    length = read(_infd, buffer, EVENT_BUF_LEN);
    if (length < 0 && errno == EAGAIN) {
      return;
    }
    if (length < 0 && errno != EAGAIN) {
      throw runtime_error("Error watching file changes: " +
                          string(strerror(errno)));
    }
    struct inotify_event *event = (struct inotify_event *)&buffer;
    if (event->mask & IN_MODIFY) {
      topic = _inwds[event->wd];
      path = _watch_list[_inwds[event->wd]];
      type = path.substr(path.find_last_of(".") + 1);
      cout << "Change detected to topic '" << topic << "' at " << path
           << " (type " << type << ")" << endl;
    } else {
      throw runtime_error("Unexpected event: "s + to_string(event->mask) +
                          " for " + string(event->name));
    }

#endif
    json meta{{"format", type}};
    ifstream fin(path);
    // get pointer to associated buffer object
    filebuf *pbuf = fin.rdbuf();
    // get file size using buffer's members
    size_t size = pbuf->pubseekoff(0, fin.end, fin.in);
    pbuf->pubseekpos(0, fin.in);
    // allocate memory to contain file data
    char *filebuffer = new char[size];
    // get file data
    pbuf->sgetn(filebuffer, size);
    publish(filebuffer, size, meta, topic);
    fin.close();
    delete[] filebuffer;
  }

private:
  /**
   * @brief Loads the settings for the RFID.
   *
   * This function is called internally to load the settings from the specified
   * settings file.
   */
  void load_settings() override {
    auto cfg = _config[_name];
    string fname, topic;
    unsigned int count = 0;

#ifdef __APPLE__
    unsigned int i = 0;
    if (!cfg["watch_list"].is_table()) {
      throw std::runtime_error("No watch_list found in settings");
    }
    auto wl = cfg["watch_list"].as_table();
    unsigned int n = wl->size();
    _events = (struct kevent *)calloc(n, sizeof(struct kevent));
    wl->for_each([&](const auto &k, const auto &v) {
      fname = v.value_or("undefined");
      _fds.push_back(open(fname.c_str(), O_CREAT | O_RDONLY));
      if (_fds.back() >= 0) {
        _watch_list[static_cast<string>(k)] = fname;
      } else {
        throw std::runtime_error("File " + fname +
                                 " not found (and cannot be created)");
      }
      _image_data.push_back((image_data_t *)malloc(sizeof(image_data_t)));
      _image_data.back()->id = count++;
      strcpy(_image_data.back()->topic, static_cast<string>(k).c_str());
      strcpy(_image_data.back()->path, fname.c_str());
      EV_SET(&_events[i++], _fds.back(), EVFILT_VNODE,
             EV_ADD | EV_CLEAR | EV_ENABLE, NOTE_WRITE, 0,
             (void *)(_image_data.back()));
    });
    kevent(_kq, _events, n, NULL, 0, NULL);

#elif __linux__
    char buffer[EVENT_BUF_LEN];
    cfg["watch_list"].as_table()->for_each([&](const auto &k, const auto &v) {
      fname = v.value_or("undefined");
      topic = static_cast<string>(k);
      _watch_list[topic] = fname;
      _image_data.push_back((image_data_t *)malloc(sizeof(image_data_t)));
      _image_data.back()->id = count++;
      strcpy(_image_data.back()->topic, topic.c_str());
      strcpy(_image_data.back()->path, fname.c_str());
      count = inotify_add_watch(_infd, fname.c_str(), IN_MODIFY);
      if (count < 0) {
        throw runtime_error("Inotify add watch error: " +
                            string(strerror(errno)));
      }
      _inwds[count] = static_cast<string>(k);
    });
#endif
  }

  map<string, string> _watch_list;
  vector<int> _fds;
  vector<image_data_t *> _image_data;
#ifdef __APPLE__
  struct kevent *_events = NULL;
  int _kq = kqueue();
#elif __linux__
  int _infd = inotify_init1(IN_NONBLOCK);
  map<int, string> _inwds; /* a map fd->topic */
#endif
};

} // namespace Mads

#endif // IMAGE_HPP