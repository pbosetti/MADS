#pragma once

#include <cstddef>
#include <cstdlib>
#include <new>
#include <type_traits>

#if defined(_MSC_VER)
#include <malloc.h>
#endif

template <typename T, std::size_t Alignment> class AlignedAllocator {
public:
  using value_type = T;

  AlignedAllocator() noexcept = default;

  template <typename U>
  AlignedAllocator(const AlignedAllocator<U, Alignment> &) noexcept {}

  [[nodiscard]] T *allocate(std::size_t n) {
    void *p = nullptr;
#if defined(_MSC_VER)
    p = _aligned_malloc(n * sizeof(T), Alignment);
    if (!p) {
      throw std::bad_alloc();
    }
#else
    if (posix_memalign(&p, Alignment, n * sizeof(T)) != 0) {
      throw std::bad_alloc();
    }
#endif
    return static_cast<T *>(p);
  }

  void deallocate(T *p, std::size_t) noexcept {
#if defined(_MSC_VER)
    _aligned_free(p);
#else
    free(p);
#endif
  }

  template <typename U> struct rebind {
    using other = AlignedAllocator<U, Alignment>;
  };

  using is_always_equal = std::true_type;
};

template <typename T, typename U, std::size_t Alignment>
inline bool operator==(const AlignedAllocator<T, Alignment> &,
                       const AlignedAllocator<U, Alignment> &) noexcept {
  return true;
}

template <typename T, typename U, std::size_t Alignment>
inline bool operator!=(const AlignedAllocator<T, Alignment> &lhs,
                       const AlignedAllocator<U, Alignment> &rhs) noexcept {
  return !(lhs == rhs);
}
