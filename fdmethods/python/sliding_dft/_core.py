from __future__ import annotations

import ctypes
import os
import platform
from pathlib import Path
from typing import Iterable


def _library_filenames() -> list[str]:
  if os.name == "nt":
    return ["SlidingDFTC.dll"]
  if os.name == "posix" and platform.system() == "Darwin":
    return ["libSlidingDFTC.dylib"]
  return ["libSlidingDFTC.so"]


def _candidate_paths() -> list[Path]:
  root = Path(__file__).resolve().parents[2]
  filenames = _library_filenames()
  candidates: list[Path] = []

  env_path = os.environ.get("SLIDINGDFT_LIBRARY")
  if env_path:
    candidates.append(Path(env_path))

  for filename in filenames:
    candidates.append(root / "build" / filename)
    candidates.append(root / "build" / "lib" / filename)
    candidates.append(root / "python" / filename)
    candidates.append(Path(filename))

  return candidates


def find_library(path: str | os.PathLike[str] | None = None) -> Path:
  if path is not None:
    library_path = Path(path).expanduser().resolve()
    if library_path.is_file():
      return library_path
    raise FileNotFoundError(f"SlidingDFT shared library not found: {library_path}")

  for candidate in _candidate_paths():
    resolved = candidate.expanduser().resolve()
    if resolved.is_file():
      return resolved

  searched = "\n".join(str(candidate) for candidate in _candidate_paths())
  raise FileNotFoundError(
      "Unable to locate the SlidingDFT shared library.\n"
      "Build the project first or set SLIDINGDFT_LIBRARY.\n"
      f"Searched:\n{searched}"
  )


class _Api:
  def __init__(self, library_path: str | os.PathLike[str] | None = None) -> None:
    self._library_path = find_library(library_path)
    self._lib = ctypes.CDLL(str(self._library_path))

    self._lib.sliding_dft_create.argtypes = [ctypes.c_size_t, ctypes.c_size_t]
    self._lib.sliding_dft_create.restype = ctypes.c_void_p

    self._lib.sliding_dft_destroy.argtypes = [ctypes.c_void_p]
    self._lib.sliding_dft_destroy.restype = None

    self._lib.sliding_dft_sample_size.argtypes = []
    self._lib.sliding_dft_sample_size.restype = ctypes.c_size_t

    sample_size = self._lib.sliding_dft_sample_size()
    if sample_size == ctypes.sizeof(ctypes.c_float):
      self._sample_t = ctypes.c_float
    elif sample_size == ctypes.sizeof(ctypes.c_double):
      self._sample_t = ctypes.c_double
    else:
      raise RuntimeError(f"Unsupported SlidingDFT sample size: {sample_size}")

    self._lib.sliding_dft_update.argtypes = [ctypes.c_void_p, self._sample_t]
    self._lib.sliding_dft_update.restype = ctypes.c_int

    self._lib.sliding_dft_bins.argtypes = [ctypes.c_void_p]
    self._lib.sliding_dft_bins.restype = ctypes.c_size_t

    self._lib.sliding_dft_get_power.argtypes = [
        ctypes.c_void_p,
        ctypes.POINTER(self._sample_t),
        ctypes.c_size_t,
    ]
    self._lib.sliding_dft_get_power.restype = ctypes.c_int

    self._lib.sliding_dft_last_error.argtypes = []
    self._lib.sliding_dft_last_error.restype = ctypes.c_char_p

  def error_message(self) -> str:
    message = self._lib.sliding_dft_last_error()
    if message is None:
      return "unknown error"
    return message.decode("utf-8")


class SlidingDFT:
  def __init__(
      self,
      window_size: int,
      renorm_interval: int = 4096,
      library_path: str | os.PathLike[str] | None = None,
  ) -> None:
    self._api = _Api(library_path)
    self._handle = self._api._lib.sliding_dft_create(window_size, renorm_interval)
    if not self._handle:
      raise RuntimeError(self._api.error_message())

  def __del__(self) -> None:
    handle = getattr(self, "_handle", None)
    if handle:
      self._api._lib.sliding_dft_destroy(handle)
      self._handle = None

  @property
  def bins(self) -> int:
    bins = self._api._lib.sliding_dft_bins(self._handle)
    if bins == 0:
      error = self._api.error_message()
      if error and error != "unknown error":
        raise RuntimeError(error)
    return int(bins)

  def update(self, sample: float) -> None:
    status = self._api._lib.sliding_dft_update(self._handle, sample)
    if status != 0:
      raise RuntimeError(self._api.error_message())

  def extend(self, samples: Iterable[float]) -> None:
    for sample in samples:
      self.update(sample)

  def get_power(self) -> list[float]:
    size = self.bins
    buffer_type = self._api._sample_t * size
    buffer = buffer_type()
    status = self._api._lib.sliding_dft_get_power(self._handle, buffer, size)
    if status != 0:
      raise RuntimeError(self._api.error_message())
    return [float(value) for value in buffer]
