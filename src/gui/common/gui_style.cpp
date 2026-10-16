#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#endif

#include "gui_style.hpp"

#include <imgui.h>

#include <GLFW/glfw3.h>

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#include <algorithm>
#include <array>
#include <cfloat>
#include <cstdint>

#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif

namespace MadsGui {

const char *setup_gl_hints() {
#if defined(__APPLE__)
  glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
  glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
  glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
  glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
  return "#version 150";
#else
  glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
  glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
  return "#version 130";
#endif
}

void apply_style() {
  ImGui::StyleColorsDark();
  ImGuiStyle &style = ImGui::GetStyle();
  style.WindowRounding = 8.0F;
  style.FrameRounding = 6.0F;
  style.ChildRounding = 6.0F;
  style.Colors[ImGuiCol_WindowBg] = ImVec4(0.09F, 0.10F, 0.12F, 1.0F);
  style.Colors[ImGuiCol_FrameBg] = ImVec4(0.13F, 0.15F, 0.18F, 1.0F);
  style.Colors[ImGuiCol_Button] = ImVec4(0.19F, 0.33F, 0.42F, 1.0F);
  style.Colors[ImGuiCol_ButtonHovered] = ImVec4(0.25F, 0.44F, 0.56F, 1.0F);
  style.Colors[ImGuiCol_ButtonActive] = ImVec4(0.18F, 0.29F, 0.37F, 1.0F);
}

ImFont *load_monospace_font(float size_pixels) {
  ImGuiIO &io = ImGui::GetIO();
  const std::array<const char *, 6> candidates = {
      "/System/Library/Fonts/Menlo.ttc",
      "/System/Library/Fonts/SFNSMono.ttf",
      "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
      "C:/Windows/Fonts/consola.ttf",
      "C:/Windows/Fonts/cour.ttf",
      "/usr/share/fonts/TTF/DejaVuSansMono.ttf",
  };

  for (const char *candidate : candidates) {
    if (std::filesystem::exists(candidate)) {
      if (ImFont *font = io.Fonts->AddFontFromFileTTF(candidate, size_pixels); font != nullptr) {
        return font;
      }
    }
  }
  return io.Fonts->AddFontDefault();
}

Texture load_texture_from_png(const std::filesystem::path &path) {
  Texture texture;
  int channels = 0;
  unsigned char *pixels = stbi_load(path.string().c_str(), &texture.width, &texture.height, &channels, STBI_rgb_alpha);
  if (pixels == nullptr) {
    return texture;
  }

  glGenTextures(1, &texture.id);
  glBindTexture(GL_TEXTURE_2D, texture.id);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  glTexImage2D(GL_TEXTURE_2D,
               0,
               GL_RGBA,
               texture.width,
               texture.height,
               0,
               GL_RGBA,
               GL_UNSIGNED_BYTE,
               pixels);
  glBindTexture(GL_TEXTURE_2D, 0);
  stbi_image_free(pixels);
  return texture;
}

Texture load_logo(const std::filesystem::path &primary_images_dir) {
  if (!primary_images_dir.empty()) {
    Texture texture = load_texture_from_png(primary_images_dir / "logo_white.png");
    if (texture.id != 0) {
      return texture;
    }
  }
  Texture texture = load_texture_from_png(std::filesystem::path(MADS_GUI_SOURCE_IMAGES_DIR) / "logo_white.png");
  if (texture.id != 0) {
    return texture;
  }
  return Texture{};
}

void free_texture(Texture &texture) {
  if (texture.id != 0) {
    glDeleteTextures(1, &texture.id);
  }
  texture = Texture{};
}

float status_footer_height() {
  const float line_height = ImGui::GetTextLineHeightWithSpacing();
  const float image_height = line_height * 2.0F;
  const float content_height = std::max(image_height, line_height * 2.0F);
  return content_height + ImGui::GetStyle().FramePadding.y * 4.0F + ImGui::GetStyle().ItemSpacing.y;
}

void draw_status_footer(const Texture &logo, const std::string &line1, const std::string &line2) {
  ImGui::Separator();
  const float line_height = ImGui::GetTextLineHeightWithSpacing();
  const float image_height = line_height * 2.0F;
  const float content_height = std::max(image_height, line_height * 2.0F);
  const float footer_height = content_height + ImGui::GetStyle().FramePadding.y * 4.0F;
  ImGui::BeginChild("status_footer", ImVec2(-FLT_MIN, footer_height), false, ImGuiWindowFlags_NoScrollbar);

  if (logo.id != 0 && logo.width > 0 && logo.height > 0) {
    const float image_width = image_height * static_cast<float>(logo.width) / static_cast<float>(logo.height);
    ImGui::Image(static_cast<ImTextureID>(static_cast<uintptr_t>(logo.id)), ImVec2(image_width, image_height));
    ImGui::SameLine();
  }

  ImGui::BeginGroup();
  ImGui::Text("%s", line1.c_str());
  ImGui::Text("%s", line2.c_str());
  ImGui::EndGroup();

  ImGui::EndChild();
}

} // namespace MadsGui
