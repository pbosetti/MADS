#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iosfwd>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#ifndef TERMINAL_LOGO_STB_IMAGE_INCLUDED
#define TERMINAL_LOGO_STB_IMAGE_INCLUDED
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#endif

namespace terminal_logo {

class TerminalLogoRenderer {
public:
  enum class ColorMode {
    Auto,
    TrueColor,
    Ansi256,
    Ansi16,
    Mono
  };

  struct Options {
    int width = 80;
    ColorMode mode = ColorMode::Auto;
  };

  static ColorMode parse_mode(std::string_view value) {
    if (value == "auto") {
      return ColorMode::Auto;
    }
    if (value == "truecolor") {
      return ColorMode::TrueColor;
    }
    if (value == "256") {
      return ColorMode::Ansi256;
    }
    if (value == "ansi16") {
      return ColorMode::Ansi16;
    }
    if (value == "mono") {
      return ColorMode::Mono;
    }
    throw std::invalid_argument("Invalid mode: " + std::string(value));
  }

  static void render_from_path(const std::filesystem::path& image_path,
                               std::ostream& out,
                               const Options& options) {
    if (options.width <= 0) {
      throw std::invalid_argument("width must be positive");
    }
    if (!std::filesystem::exists(image_path)) {
      throw std::runtime_error("Image path does not exist: " + image_path.string());
    }

#ifdef _WIN32
    enable_ansi_on_windows();
#endif

    const Image image = load_image(image_path.string());
    const ColorMode resolved_mode = detect_mode(options.mode);
    render_to_stream(image, options.width, resolved_mode, out);
  }

  static void render_from_path(const std::filesystem::path& image_path, std::ostream& out) {
    render_from_path(image_path, out, Options{});
  }

private:
  struct Color {
    std::uint8_t r;
    std::uint8_t g;
    std::uint8_t b;
    std::uint8_t a;
  };

  struct Image {
    int width = 0;
    int height = 0;
    std::vector<Color> pixels;
  };

#ifdef _WIN32
  static void enable_ansi_on_windows() {
    HANDLE handle = GetStdHandle(STD_OUTPUT_HANDLE);
    if (handle == INVALID_HANDLE_VALUE || handle == nullptr) {
      return;
    }

    DWORD mode = 0;
    if (!GetConsoleMode(handle, &mode)) {
      return;
    }

    mode |= ENABLE_VIRTUAL_TERMINAL_PROCESSING;
    SetConsoleMode(handle, mode);
  }
#endif

  static ColorMode detect_mode(ColorMode preferred) {
    if (preferred != ColorMode::Auto) {
      return preferred;
    }

    if (std::getenv("NO_COLOR") != nullptr) {
      return ColorMode::Mono;
    }

    const std::string colorterm = std::getenv("COLORTERM") ? std::getenv("COLORTERM") : "";
    const std::string term = std::getenv("TERM") ? std::getenv("TERM") : "";

    if (colorterm.find("truecolor") != std::string::npos || colorterm.find("24bit") != std::string::npos) {
      return ColorMode::TrueColor;
    }
    if (term.find("256color") != std::string::npos) {
      return ColorMode::Ansi256;
    }
    if (!term.empty() && term != "dumb") {
      return ColorMode::Ansi16;
    }

#ifdef _WIN32
    return ColorMode::Ansi16;
#else
    return ColorMode::Mono;
#endif
  }

  static Image load_image(const std::string& path) {
    int width = 0;
    int height = 0;
    int channels = 0;
    unsigned char* data = stbi_load(path.c_str(), &width, &height, &channels, 4);
    if (data == nullptr) {
      throw std::runtime_error("Failed to load image: " + path);
    }

    Image image;
    image.width = width;
    image.height = height;
    image.pixels.resize(static_cast<std::size_t>(width) * static_cast<std::size_t>(height));

    for (int y = 0; y < height; ++y) {
      for (int x = 0; x < width; ++x) {
        const std::size_t idx = static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x);
        const std::size_t off = idx * 4;
        image.pixels[idx] = Color{
            static_cast<std::uint8_t>(data[off + 0]),
            static_cast<std::uint8_t>(data[off + 1]),
            static_cast<std::uint8_t>(data[off + 2]),
            static_cast<std::uint8_t>(data[off + 3])};
      }
    }

    stbi_image_free(data);
    return image;
  }

  static Color sample_bilinear(const Image& image, float x, float y) {
    x = std::clamp(x, 0.0f, static_cast<float>(image.width - 1));
    y = std::clamp(y, 0.0f, static_cast<float>(image.height - 1));

    const int x0 = static_cast<int>(std::floor(x));
    const int y0 = static_cast<int>(std::floor(y));
    const int x1 = std::min(x0 + 1, image.width - 1);
    const int y1 = std::min(y0 + 1, image.height - 1);

    const float tx = x - static_cast<float>(x0);
    const float ty = y - static_cast<float>(y0);

    const auto pixel = [&](int px, int py) -> Color {
      const std::size_t idx = static_cast<std::size_t>(py) * static_cast<std::size_t>(image.width) + static_cast<std::size_t>(px);
      return image.pixels[idx];
    };

    const Color c00 = pixel(x0, y0);
    const Color c10 = pixel(x1, y0);
    const Color c01 = pixel(x0, y1);
    const Color c11 = pixel(x1, y1);

    auto lerp = [](float a, float b, float t) {
      return a + (b - a) * t;
    };

    const float r0 = lerp(static_cast<float>(c00.r), static_cast<float>(c10.r), tx);
    const float g0 = lerp(static_cast<float>(c00.g), static_cast<float>(c10.g), tx);
    const float b0 = lerp(static_cast<float>(c00.b), static_cast<float>(c10.b), tx);
    const float a0 = lerp(static_cast<float>(c00.a), static_cast<float>(c10.a), tx);

    const float r1 = lerp(static_cast<float>(c01.r), static_cast<float>(c11.r), tx);
    const float g1 = lerp(static_cast<float>(c01.g), static_cast<float>(c11.g), tx);
    const float b1 = lerp(static_cast<float>(c01.b), static_cast<float>(c11.b), tx);
    const float a1 = lerp(static_cast<float>(c01.a), static_cast<float>(c11.a), tx);

    return Color{
        static_cast<std::uint8_t>(std::round(lerp(r0, r1, ty))),
        static_cast<std::uint8_t>(std::round(lerp(g0, g1, ty))),
        static_cast<std::uint8_t>(std::round(lerp(b0, b1, ty))),
        static_cast<std::uint8_t>(std::round(lerp(a0, a1, ty)))};
  }

  static std::vector<Color> resize_image(const Image& image, int out_w, int out_h) {
    std::vector<Color> out(static_cast<std::size_t>(out_w) * static_cast<std::size_t>(out_h));
    for (int y = 0; y < out_h; ++y) {
      const float src_y = (static_cast<float>(y) + 0.5f) * static_cast<float>(image.height) / static_cast<float>(out_h) - 0.5f;
      for (int x = 0; x < out_w; ++x) {
        const float src_x = (static_cast<float>(x) + 0.5f) * static_cast<float>(image.width) / static_cast<float>(out_w) - 0.5f;
        out[static_cast<std::size_t>(y) * static_cast<std::size_t>(out_w) + static_cast<std::size_t>(x)] =
            sample_bilinear(image, src_x, src_y);
      }
    }
    return out;
  }

  static int ansi256_index(Color c) {
    const int r = static_cast<int>(std::round((c.r / 255.0) * 5.0));
    const int g = static_cast<int>(std::round((c.g / 255.0) * 5.0));
    const int b = static_cast<int>(std::round((c.b / 255.0) * 5.0));
    return 16 + 36 * r + 6 * g + b;
  }

  static int ansi16_index(Color c, bool bright) {
    const int r = c.r >= 128 ? 1 : 0;
    const int g = c.g >= 128 ? 1 : 0;
    const int b = c.b >= 128 ? 1 : 0;
    int idx = r + 2 * g + 4 * b;
    if (idx == 0 && bright) {
      idx = 7;
    }
    return idx;
  }

  static std::string fg_escape(Color c, ColorMode mode) {
    if (mode == ColorMode::TrueColor) {
      return "\x1b[38;2;" + std::to_string(c.r) + ";" + std::to_string(c.g) + ";" + std::to_string(c.b) + "m";
    }
    if (mode == ColorMode::Ansi256) {
      return "\x1b[38;5;" + std::to_string(ansi256_index(c)) + "m";
    }
    if (mode == ColorMode::Ansi16) {
      const int idx = ansi16_index(c, true);
      return "\x1b[" + std::to_string((idx < 8 ? 30 : 90) + (idx % 8)) + "m";
    }
    return "";
  }

  static std::string bg_escape(Color c, ColorMode mode) {
    if (mode == ColorMode::TrueColor) {
      return "\x1b[48;2;" + std::to_string(c.r) + ";" + std::to_string(c.g) + ";" + std::to_string(c.b) + "m";
    }
    if (mode == ColorMode::Ansi256) {
      return "\x1b[48;5;" + std::to_string(ansi256_index(c)) + "m";
    }
    if (mode == ColorMode::Ansi16) {
      const int idx = ansi16_index(c, false);
      return "\x1b[" + std::to_string((idx < 8 ? 40 : 100) + (idx % 8)) + "m";
    }
    return "";
  }

  static char mono_char(Color top, Color bottom) {
    if (top.a < 16 && bottom.a < 16) {
      return ' ';
    }
    const int lum_top = (static_cast<int>(top.r) * 2126 + static_cast<int>(top.g) * 7152 + static_cast<int>(top.b) * 722) / 10000;
    const int lum_bottom =
        (static_cast<int>(bottom.r) * 2126 + static_cast<int>(bottom.g) * 7152 + static_cast<int>(bottom.b) * 722) / 10000;
    const int avg = (lum_top + lum_bottom) / 2;
    constexpr std::array<char, 10> ramp = {' ', '.', ':', '-', '=', '+', '*', '#', '%', '@'};
    const std::size_t idx = static_cast<std::size_t>(std::clamp(avg, 0, 255)) * (ramp.size() - 1) / 255;
    return ramp[idx];
  }

  static void render_to_stream(const Image& image, int out_w, ColorMode mode, std::ostream& out) {
    constexpr float pixel_aspect = 0.5f;
    const float out_h_f = static_cast<float>(image.height) * static_cast<float>(out_w) * pixel_aspect /
                          static_cast<float>(image.width);
    const int out_h = std::max(2, static_cast<int>(std::round(out_h_f)));

    const std::vector<Color> scaled = resize_image(image, out_w, out_h);

    for (int y = 0; y < out_h; y += 2) {
      for (int x = 0; x < out_w; ++x) {
        const Color top = scaled[static_cast<std::size_t>(y) * static_cast<std::size_t>(out_w) + static_cast<std::size_t>(x)];
        const Color bottom = scaled[static_cast<std::size_t>(std::min(y + 1, out_h - 1)) * static_cast<std::size_t>(out_w) +
                                    static_cast<std::size_t>(x)];
        const bool top_visible = top.a >= 16;
        const bool bottom_visible = bottom.a >= 16;

        if (mode == ColorMode::Mono) {
          out << mono_char(top, bottom);
        } else {
          if (!top_visible && !bottom_visible) {
            out << "\x1b[0m ";
          } else if (top_visible && bottom_visible) {
            out << fg_escape(top, mode) << bg_escape(bottom, mode) << "▀";
          } else if (top_visible) {
            // Upper half only; keep lower half as terminal background.
            out << "\x1b[0m" << fg_escape(top, mode) << "▀";
          } else {
            // Lower half only; keep upper half as terminal background.
            out << "\x1b[0m" << fg_escape(bottom, mode) << "▄";
          }
        }
      }
      if (mode != ColorMode::Mono) {
        out << "\x1b[0m";
      }
      out << '\n';
    }
  }
};

}  // namespace terminal_logo
