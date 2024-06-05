/*
  ____                                         _
 |  _ \ _   _ _ __ ___  _ __ ___  _   _    ___| | __ _ ___ ___
 | | | | | | | '_ ` _ \| '_ ` _ \| | | |  / __| |/ _` / __/ __|
 | |_| | |_| | | | | | | | | | | | |_| | | (__| | (_| \__ \__ \
 |____/ \__,_|_| |_| |_|_| |_| |_|\__, |  \___|_|\__,_|___/___/
                                  |___/

Author(s): Paolo Bosetti
*/

#ifndef DUMMY_HPP
#define DUMMY_HPP


#include "mads.hpp"
#include "agent.hpp"
#include <armadillo>
#include <deque>
#include <memory>
#include <sstream>
#include <vector>

using namespace std;

namespace Mads {

class ARIMA {
public:
  ARIMA() : p({0.5}), d(0), q({}) {
    sigma = 0.5;
    _mem = new vector<double>();
  }

  ~ARIMA() {}

  string info() {
    stringstream ss;
    ss << "ARIMA(" << p.size() << ", " << d << ", " << q.size() << ")"
       << " sigma: " << sigma << endl;
    return ss.str();
  }

  void reset(double x0 = 0) {
    _x.clear();
    _w.clear();
    _x0 = x0;
    _mem->clear();
    _mem->reserve(d+1);
    for (size_t i = 0; i <= d; i++) {
      _mem->push_back(0);
    }
  }

  void set_sigma(double s) { sigma = s; }

  double next() {
    double w = arma::randn(arma::distr_param(0.0, sigma));
    double x, xs = 0, ws = 0;
    double ret = 0;
    if (q.size() > 0) {
      _w.push_back(w);
      if (_w.size() > q.size() + 1) {
        _w.pop_front();
      }
      for (size_t i = 0; i < _w.size(); i++) {
        ws += _w[i] * q[_w.size() - 1 - i];
      }
    }
    if (p.size() > 0) {
      for (size_t i = 0; i < _x.size(); i++) {
        xs += _x[i] * p[_x.size() - 1 - i];
      }
      x = xs + w + ws;
      _x.push_back(x);
      if (_x.size() > p.size() + 1) {
        _x.pop_front();
      }
    } else {
      x = w + ws;
    }
    if (d > 0) {
      (*_mem)[0] = x;
      for (size_t i = 1; i < _mem->size(); i++) {
        (*_mem)[i] += (*_mem)[i-1];
      }
      ret = _x0 + round(_mem->back() * 1000) / 1000.0;
    } else {
      ret = _x0 + round(x * 1000) / 1000.0;
    }
    return ret;
  }

public:
  double sigma;
  vector<float> p;
  unsigned int d;
  vector<float> q;

private:
  double _x0 = 0;
  deque<double> _x = {}, _w = {};
  vector<double> *_mem;
};

class Dummy : public Agent {

public:
  Dummy(std::string name, std::string settings_path)
      : Agent(name, settings_path) {
  }

  void load_settings() override {
    auto cfg = _config[_name];
    _topics.clear();
    cfg["active_topics"].as_array()->for_each(
        [&](auto &e) { _topics.push_back(e.value_or("undefined")); });
    for (auto &t : _topics) {
      if (!cfg["topics"][t].is_table()) {
        cerr << fg::red << "Topic " << t << " is not defined" << fg::reset
             << endl;
        continue;
      }
      auto tab = cfg["topics"][t];
      auto type = tab["type"].value_or("undefined");
      _topic_freqs[t] = tab["frequency"].value_or(1);
      if (type == "arima"s) {
        _topic_type[t] = "arima";
        map<string, ARIMA> m{};
        tab["content"].as_table()->for_each([&](auto &k, auto &v) {
          m[static_cast<string>(k)] = ARIMA();
          m[static_cast<string>(k)].p.clear();
          m[static_cast<string>(k)].q.clear();
          v.as_table()->at("p").as_array()->for_each([&](auto &e) {
            m[static_cast<string>(k)].p.push_back(
                e.as_floating_point()->value_or(0));
          });
          m[static_cast<string>(k)].d = v.as_table()->at("d").value_or(0);
          v.as_table()->at("q").as_array()->for_each([&](auto &e) {
            m[static_cast<string>(k)].q.push_back(
                e.as_floating_point()->value_or(0));
          });
          m[static_cast<string>(k)].sigma =
              v.as_table()->at("s").as_floating_point()->value_or(0.5);
          if (v.as_table()->contains("start")) {
            double x0 =
                v.as_table()->at("start").as_floating_point()->value_or(0);
            m[static_cast<string>(k)].reset(x0);
          } else {
            m[static_cast<string>(k)].reset();
          }
        });
        _topic_data[t] = m;
      } else if (type == "bits"s) {
        _topic_type[t] = "bits";
        map<string, double> m{};
        tab["content"].as_table()->for_each([&](auto &k, auto &v) {
          m[static_cast<string>(k)] = v.as_floating_point()->value_or(0.5);
        });
        _topic_data[t] = m;
      } else if (type == "gpio"s) {
        _topic_type[t] = "gpio";
        map<string, list<int>> m{{"lines", {}}};
        tab["content"]["lines"].as_array()->for_each(
            [&](auto &e) { m["lines"].push_back(e.value_or(0)); });
        _topic_data[t] = m;
      } else if (type == "image"s) {
        _topic_type[t] = "image";
        map<string, unsigned int> m{};
        m["width"] = tab["content"]["size"].as_array()->at(0).value_or(0);
        m["height"] = tab["content"]["size"].as_array()->at(1).value_or(1);
        m["levels"] = tab["content"]["levels"].value_or(256);
        _topic_data[t] = m;
      } else if (type == "matrix"s) {
        _topic_type[t] = "matrix";
        map<string, double> m{};
        m["width"] = tab["content"]["size"].as_array()->at(0).value_or(0);
        m["height"] = tab["content"]["size"].as_array()->at(1).value_or(1);
        m["mean"] = tab["content"]["mean"].as_floating_point()->value_or(0);
        m["std"] = tab["content"]["std"].as_floating_point()->value_or(0);
        _topic_data[t] = m;
      } else {
        cerr << fg::red << "Undefined topic type (topic " << t << ")"
             << fg::reset << endl;
      }
    }
  }

  nlohmann::json make_data() {
    nlohmann::json data;
    for (auto &t : _topics) {
      if (_topic_type[t] == "arima") {
        auto m = any_cast<map<string, ARIMA>>(_topic_data[t]);
        for (auto &e : m) {
          data[t][e.first] = e.second.next();
        }
      } else if (_topic_type[t] == "gpio") {
        auto m = any_cast<map<string, list<int>>>(_topic_data[t]);
        for (auto &l : m["lines"]) {
          data[t][to_string(l)] =
              static_cast<int>(round(arma::randu(arma::distr_param(0, 1))));
        }
      } else if (_topic_type[t] == "image") {
        auto m = any_cast<map<string, unsigned int>>(_topic_data[t]);
        data[t]["width"] = m["width"];
        data[t]["height"] = m["height"];
        data[t]["levels"] = m["levels"];
        data[t]["data"] = arma::randi<arma::Mat<unsigned int>>(
            m["height"], m["width"], arma::distr_param(0, m["levels"]));
      } else if (_topic_type[t] == "matrix") {
        auto m = any_cast<map<string, double>>(_topic_data[t]);
        data[t]["width"] = m["width"];
        data[t]["height"] = m["height"];
        data[t]["mean"] = m["mean"];
        data[t]["std"] = m["std"];
        data[t]["data"] = arma::randn<arma::Mat<double>>(
            m["height"], m["width"], arma::distr_param(m["mean"], m["std"]));
      } else if (_topic_type[t] == "bits") {
        auto m = any_cast<map<string, double>>(_topic_data[t]);
        for (auto &[k, v] : m) {
          data[t][k] = (arma::randu(arma::distr_param(0, 1)) > v ? 1 : 0);
        }
      }
    }
    return data;
  }

  void publish(bool echo = false) {
    static unsigned long int i = 0;
    nlohmann::json data = make_data();
    if (echo)
      cout << "(" << i << ") Publishing ";
    for (auto &t : _topics) {
      if ((i % _topic_freqs[t]) == 0) {
        Agent::publish(data[t], t);
        if (echo)
          cout << t << " ";
      }
    }
    i++;
    if (echo)
      cout << endl;
  }

  void info(ostream &out = cout) override {
    Agent::info(out);
    out << "  Topics published:" << style::bold;
    for (auto &t : _topics) {
      out << " " << t << " (" << _topic_freqs[t] << ") ";
    }
    out << style::reset << endl;
  }

private:
  list<string> _topics = {};
  map<string, any> _topic_data = {};
  map<string, string> _topic_type = {};
  map<string, unsigned int> _topic_freqs = {};
};

} // namespace Mads

#endif // DUMMY_HPP
