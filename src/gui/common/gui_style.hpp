// Shared look & feel of the MADS GUI apps: ImGui style, monospace font,
// logo texture loading, status footer and window constants.
#pragma once

#include <imgui.h>

#include <filesystem>
#include <string>

namespace MadsGui {

struct Texture {
  unsigned int id = 0;
  int width = 0;
  int height = 0;
};

inline constexpr int kWindowWidth = 1440;
inline constexpr int kWindowHeight = 900;
inline constexpr ImVec4 kClearColor{0.06F, 0.06F, 0.08F, 1.0F};
inline constexpr ImVec4 kOkColor{0.42F, 0.80F, 0.48F, 1.0F};
inline constexpr ImVec4 kErrorColor{0.92F, 0.39F, 0.39F, 1.0F};
inline constexpr ImGuiWindowFlags kMainWindowFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                                     ImGuiWindowFlags_NoSavedSettings |
                                                     ImGuiWindowFlags_NoScrollbar |
                                                     ImGuiWindowFlags_NoScrollWithMouse;

// Sets GLFW context hints; returns the matching GLSL version string for ImGui_ImplOpenGL3_Init.
const char *setup_gl_hints();
void apply_style();                         // StyleColorsDark + chat's rounding & colors
ImFont *load_monospace_font(float size_pixels = 15.0F);
Texture load_texture_from_png(const std::filesystem::path &path);
// Loads logo_white.png, trying primary_images_dir first (skipped if empty), then the
// source-tree share/images directory (fallback for running from the build tree).
// Returns the first texture that loads, or an empty Texture (id == 0) if none does.
Texture load_logo(const std::filesystem::path &primary_images_dir);
void free_texture(Texture &texture);        // glDeleteTextures if id != 0, then reset
float status_footer_height();               // height reserved for the status footer
// line2_color, when given, colors the second line (e.g. kErrorColor).
void draw_status_footer(const Texture &logo, const std::string &line1, const std::string &line2,
                        const ImVec4 *line2_color = nullptr);

} // namespace MadsGui
