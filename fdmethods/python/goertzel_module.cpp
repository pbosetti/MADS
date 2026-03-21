#define PY_SSIZE_T_CLEAN
#include <Python.h>

#include "goertzel/goertzel.hpp"

#include <complex>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

std::vector<double> sequence_to_doubles(PyObject *sequence, const char *name) {
  PyObject *fast = PySequence_Fast(sequence, name);
  if (fast == nullptr) {
    throw std::runtime_error(name);
  }

  const Py_ssize_t size = PySequence_Fast_GET_SIZE(fast);
  std::vector<double> values;
  values.reserve(static_cast<std::size_t>(size));

  PyObject **items = PySequence_Fast_ITEMS(fast);
  for (Py_ssize_t i = 0; i < size; ++i) {
    const double value = PyFloat_AsDouble(items[i]);
    if (PyErr_Occurred() != nullptr) {
      Py_DECREF(fast);
      throw std::runtime_error(name);
    }
    values.push_back(value);
  }

  Py_DECREF(fast);
  return values;
}

PyObject *build_complex_list(const std::vector<std::complex<double>> &values) {
  PyObject *result = PyList_New(static_cast<Py_ssize_t>(values.size()));
  if (result == nullptr) {
    return nullptr;
  }

  for (Py_ssize_t i = 0; i < static_cast<Py_ssize_t>(values.size()); ++i) {
    PyObject *item = PyComplex_FromDoubles(values[static_cast<std::size_t>(i)].real(),
                                           values[static_cast<std::size_t>(i)].imag());
    if (item == nullptr) {
      Py_DECREF(result);
      return nullptr;
    }
    PyList_SET_ITEM(result, i, item);
  }

  return result;
}

PyObject *build_float_list(const std::vector<double> &values) {
  PyObject *result = PyList_New(static_cast<Py_ssize_t>(values.size()));
  if (result == nullptr) {
    return nullptr;
  }

  for (Py_ssize_t i = 0; i < static_cast<Py_ssize_t>(values.size()); ++i) {
    PyObject *item = PyFloat_FromDouble(values[static_cast<std::size_t>(i)]);
    if (item == nullptr) {
      Py_DECREF(result);
      return nullptr;
    }
    PyList_SET_ITEM(result, i, item);
  }

  return result;
}

PyObject *analyze(PyObject *, PyObject *args) {
  PyObject *samples_obj = nullptr;
  PyObject *alpha_obj = nullptr;
  if (!PyArg_ParseTuple(args, "OO", &samples_obj, &alpha_obj)) {
    return nullptr;
  }

  try {
    const auto samples = sequence_to_doubles(samples_obj, "samples must be a sequence of real numbers");
    const auto alpha = sequence_to_doubles(alpha_obj, "alpha must be a sequence of real numbers");

    Goertzel::Analyzer analyzer(alpha);
    analyzer.process(samples);
    return build_float_list(analyzer.state_vector());
  } catch (const std::exception &error) {
    PyErr_SetString(PyExc_ValueError, error.what());
    return nullptr;
  }
}

PyObject *goertzel_bins(PyObject *, PyObject *args) {
  PyObject *samples_obj = nullptr;
  PyObject *bins_obj = nullptr;
  if (!PyArg_ParseTuple(args, "OO", &samples_obj, &bins_obj)) {
    return nullptr;
  }

  try {
    const auto samples = sequence_to_doubles(samples_obj, "samples must be a sequence of real numbers");
    const auto bins = sequence_to_doubles(bins_obj, "bins must be a sequence of real numbers");

    std::vector<double> alpha;
    alpha.reserve(bins.size());
    for (const double bin : bins) {
      alpha.push_back(Goertzel::Analyzer::alpha_from_bin(bin, samples.size()));
    }

    Goertzel::Analyzer analyzer(alpha);
    analyzer.process(samples);

    std::vector<std::complex<double>> values;
    values.reserve(bins.size());
    for (std::size_t i = 0; i < bins.size(); ++i) {
      values.push_back(analyzer.dft_term_from_bin(i, bins[i], samples.size()));
    }
    return build_complex_list(values);
  } catch (const std::exception &error) {
    PyErr_SetString(PyExc_ValueError, error.what());
    return nullptr;
  }
}

PyObject *goertzel_frequencies(PyObject *, PyObject *args) {
  PyObject *samples_obj = nullptr;
  PyObject *frequencies_obj = nullptr;
  double sample_rate_hz = 0.0;
  if (!PyArg_ParseTuple(args, "OOd", &samples_obj, &frequencies_obj, &sample_rate_hz)) {
    return nullptr;
  }

  try {
    const auto samples = sequence_to_doubles(samples_obj, "samples must be a sequence of real numbers");
    const auto frequencies =
        sequence_to_doubles(frequencies_obj, "frequencies must be a sequence of real numbers");

    std::vector<double> alpha;
    alpha.reserve(frequencies.size());
    for (const double frequency : frequencies) {
      alpha.push_back(Goertzel::Analyzer::alpha_from_frequency(frequency, sample_rate_hz));
    }

    Goertzel::Analyzer analyzer(alpha);
    analyzer.process(samples);

    std::vector<std::complex<double>> values;
    values.reserve(frequencies.size());
    for (std::size_t i = 0; i < frequencies.size(); ++i) {
      const double omega = Goertzel::Analyzer::omega_from_frequency(frequencies[i], sample_rate_hz);
      values.push_back(analyzer.dft_term_from_omega(i, omega));
    }
    return build_complex_list(values);
  } catch (const std::exception &error) {
    PyErr_SetString(PyExc_ValueError, error.what());
    return nullptr;
  }
}

PyObject *alpha_from_bin(PyObject *, PyObject *args) {
  double bin = 0.0;
  Py_ssize_t sample_count = 0;
  if (!PyArg_ParseTuple(args, "dn", &bin, &sample_count)) {
    return nullptr;
  }

  try {
    return PyFloat_FromDouble(
        Goertzel::Analyzer::alpha_from_bin(bin, static_cast<std::size_t>(sample_count)));
  } catch (const std::exception &error) {
    PyErr_SetString(PyExc_ValueError, error.what());
    return nullptr;
  }
}

PyObject *alpha_from_frequency(PyObject *, PyObject *args) {
  double frequency_hz = 0.0;
  double sample_rate_hz = 0.0;
  if (!PyArg_ParseTuple(args, "dd", &frequency_hz, &sample_rate_hz)) {
    return nullptr;
  }

  try {
    return PyFloat_FromDouble(
        Goertzel::Analyzer::alpha_from_frequency(frequency_hz, sample_rate_hz));
  } catch (const std::exception &error) {
    PyErr_SetString(PyExc_ValueError, error.what());
    return nullptr;
  }
}

PyMethodDef methods[] = {
    {"analyze", analyze, METH_VARARGS, "Return the interleaved internal states for each coefficient."},
    {"goertzel_bins",
     goertzel_bins,
     METH_VARARGS,
     "Evaluate DFT terms for a list of bins over a real-valued signal."},
    {"goertzel_frequencies",
     goertzel_frequencies,
     METH_VARARGS,
     "Evaluate DFT terms for a list of frequencies over a real-valued signal."},
    {"alpha_from_bin", alpha_from_bin, METH_VARARGS, "Return a Goertzel coefficient for a DFT bin."},
    {"alpha_from_frequency",
     alpha_from_frequency,
     METH_VARARGS,
     "Return a Goertzel coefficient for a frequency and sample rate."},
    {nullptr, nullptr, 0, nullptr}};

PyModuleDef module = {
    PyModuleDef_HEAD_INIT,
    "goertzel",
    "Python bindings for the header-only Goertzel analyzer.",
    -1,
    methods};

}  // namespace

PyMODINIT_FUNC PyInit_goertzel(void) {
  return PyModule_Create(&module);
}
