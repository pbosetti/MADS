%module Mads
%include "std_string.i"
%include "std_map.i"
%rename(json) nlohmann::json;

%{
#include "../mads.hpp"
#include "../agent.hpp"
#include <tuple>
using namespace std;
using namespace nlohmann;

int json_into_dict(json *j, PyObject *obj) {
  for (json::iterator it = j->begin(); it != j->end(); ++it) {
    if (it.value().is_number_integer()) {
      PyDict_SetItemString(obj, it.key().c_str(), PyLong_FromLong(it.value().get<int>()));
    } else if (it.value().is_number_float()) {
      PyDict_SetItemString(obj, it.key().c_str(), PyFloat_FromDouble(it.value().get<double>()));
    } else if (it.value().is_string()) {
      PyDict_SetItemString(obj, it.key().c_str(), PyUnicode_FromString(it.value().get<string>().c_str()));
    } else if (it.value().is_boolean()) {
      PyDict_SetItemString(obj, it.key().c_str(), PyBool_FromLong(it.value().get<bool>()));
    } else if (it.value().is_array()) {
      PyObject *list = PyList_New(it.value().size());
      for (size_t i = 0; i < it.value().size(); i++) {
        if (it.value()[i].is_number_integer()) {
          PyList_SetItem(list, i, PyLong_FromLong(it.value()[i].get<int>()));
        } else if (it.value()[i].is_number_float()) {
          PyList_SetItem(list, i, PyFloat_FromDouble(it.value()[i].get<double>()));
        } else if (it.value()[i].is_string()) {
          PyList_SetItem(list, i, PyUnicode_FromString(it.value()[i].get<string>().c_str()));
        } else {
          PyErr_SetString(PyExc_TypeError, (string("unsupported type in list ") + it.key()).c_str());
          return 1;
        }
      }
      PyDict_SetItemString(obj, it.key().c_str(), list);
    } else if (it.value().is_object()) {
      PyObject *sub = PyDict_New();
      json_into_dict(&it.value(), sub);
      PyDict_SetItemString(obj, it.key().c_str(), sub);
    } else {
      PyErr_SetString(PyExc_TypeError, (string("unsupported type in dictionary ") + it.key()).c_str());
      return 1;
    }
  }
  return 0;
}
%}

%include "../mads.hpp"

%pythonbegin %{
  import json
%}

%typemap(in) chrono::milliseconds {
  $1 = chrono::milliseconds(PyLong_AsLongLong($input));
}
%typemap(typecheck) chrono::milliseconds = long long;

%feature("novaluewrapper") tuple<std::string, std::string>;
class tuple<std::string, std::string>;

%typemap(out) tuple<std::string, std::string> {
  PyObject *tpl = PyTuple_New(2);
  PyTuple_SetItem(tpl, 0, Py_BuildValue("s", get<0>($1).c_str()));
  PyTuple_SetItem(tpl, 1, Py_BuildValue("s", get<1>($1).c_str()));
  $result = tpl;
}

%typemap(out) json {
  // string desc($1.dump());
  // $result = PyUnicode_FromString(desc.c_str());
  $result = PyDict_New();
  if (json_into_dict(&$1, $result)) 
    SWIG_fail;
}

%typemap(in) json payload {
  char const *str = nullptr;
  if (PyUnicode_Check($input)) {
    str = PyUnicode_AsUTF8($input);
  } else {
    str = PyString_AsString($input);
  }
  $1 = json::parse(str);
}
%typemap(typecheck) json {
  $1 = (PyUnicode_Check($input) || PyString_Check($input)) ? 1 : 0;
}

%feature("shadow") Mads::Agent::init() %{
def init(self):
    $action(self)
    return self
%}

%feature("shadow") Mads::Agent::publish(json payload, std::string topic = "") %{
def publish(self, *args):
    if (len(args) == 1):
        topic = ""
    else:
        topic = args[1]

    if (isinstance(args[0], dict)):
        msg = json.dumps(args[0])
    else:
        msg = args[0]
    return $action(self, msg, topic)
%}


namespace Mads {
  // string event_name(event_type type);
  class Agent {
    public:
      Agent(std::string name, std::string settings_uri);
      ~Agent();
      void init();
      void connect(chrono::milliseconds delay = chrono::milliseconds(0));
      void register_event(event_type type);
      void info(ostream &out = cout);
      void publish(json payload, std::string topic = "");
      message_type receive();
      tuple<std::string, std::string> last_message();
      std::string last_topic();
      void disconnect();
      json get_settings();

      bool dummy = true;

  };
}
