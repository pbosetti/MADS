#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#endif

#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>
#include <imnodes.h>

#include <GLFW/glfw3.h>

#include "gui_style.hpp"

#include "ini_cli.hpp"

#include <algorithm>
#include <iostream>
#include <optional>
#include <filesystem>
#include <string>
#include <vector>

namespace {

constexpr const char *kWindowTitle = "MADS ini editor";

// Fixed attribute ids; each node owns at most one input and one output.
constexpr int kSourceNodeId = 1;
constexpr int kFilterNodeId = 2;
constexpr int kSinkNodeId = 3;
constexpr int kSourceOutputId = 10;
constexpr int kFilterInputId = 20;
constexpr int kFilterOutputId = 21;
constexpr int kSinkInputId = 30;

struct DemoNode {
  int id = 0;
  std::string title;
  int input_id = -1;  // -1 means no input attribute
  int output_id = -1; // -1 means no output attribute
  ImVec2 position{0.0F, 0.0F};
  bool placed = false;
};

struct DemoLink {
  int id = 0;
  int start_attr = 0;
  int end_attr = 0;
};

struct EditorState {
  std::vector<DemoNode> nodes;
  std::vector<DemoLink> links;
  int next_link_id = 100;
};

EditorState make_editor_state() {
  EditorState state;
  state.nodes = {
      DemoNode{kSourceNodeId, "source", -1, kSourceOutputId, ImVec2(60.0F, 120.0F), false},
      DemoNode{kFilterNodeId, "filter", kFilterInputId, kFilterOutputId, ImVec2(340.0F, 120.0F), false},
      DemoNode{kSinkNodeId, "sink", kSinkInputId, -1, ImVec2(620.0F, 120.0F), false},
  };
  return state;
}

void draw_node(DemoNode &node) {
  if (!node.placed) {
    ImNodes::SetNodeEditorSpacePos(node.id, node.position);
    node.placed = true;
  }

  ImNodes::BeginNode(node.id);
  ImNodes::BeginNodeTitleBar();
  ImGui::TextUnformatted(node.title.c_str());
  ImNodes::EndNodeTitleBar();

  if (node.input_id >= 0) {
    ImNodes::BeginInputAttribute(node.input_id);
    ImGui::TextUnformatted("in");
    ImNodes::EndInputAttribute();
  }
  if (node.output_id >= 0) {
    ImNodes::BeginOutputAttribute(node.output_id);
    ImGui::TextUnformatted("out");
    ImNodes::EndOutputAttribute();
  }

  ImNodes::EndNode();
}

void draw_editor(EditorState &state) {
  ImNodes::BeginNodeEditor();

  for (auto &node : state.nodes) {
    draw_node(node);
  }
  for (const auto &link : state.links) {
    ImNodes::Link(link.id, link.start_attr, link.end_attr);
  }

  ImNodes::MiniMap(0.2F, ImNodesMiniMapLocation_BottomRight);
  ImNodes::EndNodeEditor();

  // Link events and selection are only valid after EndNodeEditor().
  int started_attr = 0;
  int ended_attr = 0;
  if (ImNodes::IsLinkCreated(&started_attr, &ended_attr)) {
    state.links.push_back(DemoLink{state.next_link_id++, started_attr, ended_attr});
  }

  int destroyed_id = 0;
  if (ImNodes::IsLinkDestroyed(&destroyed_id)) {
    std::erase_if(state.links, [destroyed_id](const DemoLink &link) { return link.id == destroyed_id; });
  }

  if (ImGui::IsKeyPressed(ImGuiKey_Delete) && ImNodes::NumSelectedLinks() > 0) {
    std::vector<int> selected(static_cast<size_t>(ImNodes::NumSelectedLinks()));
    ImNodes::GetSelectedLinks(selected.data());
    std::erase_if(state.links, [&selected](const DemoLink &link) {
      return std::find(selected.begin(), selected.end(), link.id) != selected.end();
    });
    ImNodes::ClearLinkSelection();
  }
}

} // namespace

int run_gui(std::optional<std::filesystem::path> file) {
  if (!glfwInit()) {
    std::cerr << "mads ini: cannot start the GUI (no display?). Use "
                 "`mads ini --print` or `mads ini --check FILE` instead.\n";
    return 1;
  }

  const char *glsl_version = MadsGui::setup_gl_hints();

  std::string title = kWindowTitle;
  if (file) {
    title += " - " + file->string();
  }
  GLFWwindow *window = glfwCreateWindow(MadsGui::kWindowWidth, MadsGui::kWindowHeight, title.c_str(), nullptr, nullptr);
  if (window == nullptr) {
    glfwTerminate();
    std::cerr << "mads ini: cannot open a window (no display?). Use "
                 "`mads ini --print` or `mads ini --check FILE` instead.\n";
    return 1;
  }

  glfwMakeContextCurrent(window);
  glfwSwapInterval(1);

  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImNodes::CreateContext();
  ImGuiIO &io = ImGui::GetIO();
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
  io.IniFilename = nullptr;

  MadsGui::apply_style();

  ImGui_ImplGlfw_InitForOpenGL(window, true);
  ImGui_ImplOpenGL3_Init(glsl_version);

  EditorState editor = make_editor_state();

  while (!glfwWindowShouldClose(window)) {
    glfwPollEvents();

    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();

    ImGui::SetNextWindowPos(ImVec2(0.0F, 0.0F));
    ImGui::SetNextWindowSize(io.DisplaySize);

    ImGui::Begin("MainWindow", nullptr, MadsGui::kMainWindowFlags);
    draw_editor(editor);
    ImGui::End();

    ImGui::Render();
    int display_width = 0;
    int display_height = 0;
    glfwGetFramebufferSize(window, &display_width, &display_height);
    glViewport(0, 0, display_width, display_height);
    glClearColor(MadsGui::kClearColor.x, MadsGui::kClearColor.y, MadsGui::kClearColor.z, MadsGui::kClearColor.w);
    glClear(GL_COLOR_BUFFER_BIT);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    glfwSwapBuffers(window);
  }

  ImGui_ImplOpenGL3_Shutdown();
  ImGui_ImplGlfw_Shutdown();
  ImNodes::DestroyContext();
  ImGui::DestroyContext();
  glfwDestroyWindow(window);
  glfwTerminate();
  return 0;
}

int main(int argc, char **argv) {
  return Mads::Ini::run_cli(argc, argv, run_gui);
}
