#pragma once

#include <cmath>
#include <cstddef>
#include <numbers>
#include <stdexcept>
#include <vector>

#if defined(__AVX2__)
#include <immintrin.h>
#elif defined(__ARM_NEON)
#include <arm_neon.h>
#endif

#if defined(SLIDINGDFT_USE_DOUBLE)
using data_t = double;
#else
using data_t = float;
#endif

#if defined(__AVX2__) && defined(SLIDINGDFT_USE_DOUBLE)
static constexpr std::size_t VEC_WIDTH = 4;
#elif defined(__AVX2__)
static constexpr std::size_t VEC_WIDTH = 8;
#elif defined(__ARM_NEON) && defined(SLIDINGDFT_USE_DOUBLE)
static constexpr std::size_t VEC_WIDTH = 2;
#elif defined(__ARM_NEON)
static constexpr std::size_t VEC_WIDTH = 4;
#endif
