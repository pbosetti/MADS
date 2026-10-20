#include "node_canvas.hpp"

#include "ini_document.hpp"
#include "topic_match.hpp"

#include <imgui.h>
#include <imnodes.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace MadsIni {

namespace {

// Attribute ids: node_id * kPinStride + slot. Slots 0..kMaxInputs-1 are the
// declared sub_topic entries, then the "+ subscribe" pin, then the output.
constexpr int kPinStride = 64;
constexpr int kMaxInputs = 62;
constexpr int kSubscribeSlot = 62;
constexpr int kOutputSlot = 63;

constexpr const char *kNodeMenu = "mads_ini_node_menu";
constexpr const char *kCanvasMenu = "mads_ini_canvas_menu";
constexpr const char *kRenamePopup = "Rename node";

// Palette, matching MadsGui::apply_style (dark blue-grey, teal-blue accent).
constexpr ImU32 kGridBackground = IM_COL32(18, 20, 25, 255);
constexpr ImU32 kGridLine = IM_COL32(48, 54, 64, 40);
constexpr ImU32 kGridLinePrimary = IM_COL32(64, 72, 86, 70);
constexpr ImU32 kNodeBackground = IM_COL32(33, 38, 46, 255);
constexpr ImU32 kNodeBackgroundHovered = IM_COL32(40, 46, 56, 255);
constexpr ImU32 kNodeBackgroundSelected = IM_COL32(46, 54, 66, 255);
constexpr ImU32 kNodeOutline = IM_COL32(70, 82, 98, 255);
constexpr ImU32 kPinColor = IM_COL32(89, 160, 200, 255);
constexpr ImU32 kPinHovered = IM_COL32(140, 210, 245, 255);

constexpr ImU32 kLinkExact = IM_COL32(90, 165, 230, 230);
constexpr ImU32 kLinkPrefix = IM_COL32(240, 180, 50, 230);
constexpr ImU32 kLinkWildcard = IM_COL32(70, 215, 215, 230);
constexpr ImU32 kLinkImplicit = IM_COL32(140, 145, 152, 200);
constexpr ImU32 kLinkCatchAll = IM_COL32(97, 102, 110, 150);

const ImVec4 kTextDim(0.55F, 0.58F, 0.62F, 1.0F);
const ImVec4 kTextWarning(1.0F, 0.65F, 0.2F, 1.0F);
const ImVec4 kTextWildcard(0.45F, 0.85F, 0.92F, 1.0F);

struct TitleColors {
  ImU32 normal, hovered, selected;
};

/// Header colors by node role ("source", "filter", "sink", "any", or
/// "custom" for sections without a known type).
TitleColors title_colors(const std::string &role) {
  if (role == "source") {
    return {IM_COL32(40, 120, 90, 255), IM_COL32(52, 145, 110, 255), IM_COL32(64, 170, 130, 255)};
  }
  if (role == "filter") {
    return {IM_COL32(48, 92, 140, 255), IM_COL32(62, 114, 168, 255), IM_COL32(78, 136, 196, 255)};
  }
  if (role == "sink") {
    return {IM_COL32(140, 88, 44, 255), IM_COL32(166, 108, 58, 255), IM_COL32(192, 128, 72, 255)};
  }
  if (role == "custom") {
    return {IM_COL32(106, 70, 128, 255), IM_COL32(128, 88, 152, 255), IM_COL32(150, 106, 176, 255)};
  }
  return {IM_COL32(80, 86, 100, 255), IM_COL32(100, 108, 124, 255), IM_COL32(120, 130, 148, 255)};
}

/// Brightens a link color for the hovered/selected states.
ImU32 brighten(ImU32 color, float amount) {
  ImVec4 c = ImGui::ColorConvertU32ToFloat4(color);
  c.x = std::min(1.0F, c.x + amount);
  c.y = std::min(1.0F, c.y + amount);
  c.z = std::min(1.0F, c.z + amount);
  c.w = 1.0F;
  return ImGui::ColorConvertFloat4ToU32(c);
}

ImU32 link_color(const Mads::TopologyEdge &edge) {
  if (edge.catch_all) {
    return kLinkCatchAll;
  }
  if (edge.implicit_pub) {
    return kLinkImplicit;
  }
  switch (edge.kind) {
  case Mads::SubMatch::Prefix:
    return kLinkPrefix;
  case Mads::SubMatch::Wildcard:
    return kLinkWildcard;
  default:
    return kLinkExact;
  }
}

std::string pattern_label(const std::string &entry) {
  return entry.empty() ? "(all)" : entry;
}

int input_attr(int node_id, int slot) {
  return node_id * kPinStride + slot;
}

/// One drawn input pin.
struct InputPin {
  std::string entry;
  std::string label;
  bool dangling = false;
  bool wildcard = false;
};

} // namespace

struct NodeCanvas::Impl {
  Impl(Mads::Ini::Editor &editor_ref, UiState &ui_ref) : editor(editor_ref), ui(ui_ref) {}

  ~Impl() {
    if (context != nullptr) {
      ImNodes::EditorContextFree(context);
    }
  }

  // ---- per-frame steps (all called from draw(), in this order)
  void setup_context();
  int node_id(const std::string &name);
  void sync_positions();
  void draw_node(const std::string &name, int id);
  void draw_links();
  void handle_link_events();
  void handle_selection_after_editor();
  void handle_drag_end();
  void handle_delete_keys();
  void handle_focus_request();
  void handle_context_menus();
  void draw_rename_popup();
  void disconnect_link(const std::string &from, const std::string &to);
  const Mads::TopologyEdge *find_edge(const std::string &from, const std::string &to) const;
  std::string section_of_attr(int attr, int &slot) const;
  void remove_nodes(const std::vector<std::string> &names);

  Mads::Ini::Editor &editor;
  UiState &ui;

  ImNodesEditorContext *context = nullptr;
  bool styled = false;

  // Stable imnodes ids: never reused within a session, kept across renames.
  std::map<std::string, int> ids;
  int next_id = 1;
  std::map<int, std::string> names_by_id; // rebuilt every frame

  // Positions the canvas last wrote to / read back from imnodes, to tell a
  // user drag from an external change (undo, reload, auto layout).
  std::map<std::string, std::pair<float, float>> last_written;

  // Selection sync: the value of ui.selected the canvas last reconciled.
  std::string synced_selected;

  // Per-frame derived data.
  std::set<int> connected_attrs;
  std::map<std::string, int> hidden_catch_all; // subscriber -> hidden publishers

  // Geometry of the last frame (for view_center and the menus).
  ImVec2 canvas_origin{0.0F, 0.0F};
  ImVec2 canvas_size{0.0F, 0.0F};
  ImVec2 view_center_grid{0.0F, 0.0F};
  ImVec2 menu_grid_pos{0.0F, 0.0F};

  // Context menu / rename state.
  std::string menu_node;
  bool want_rename = false;
  char rename_buffer[256] = {};
  std::string rename_error;
};

void NodeCanvas::Impl::setup_context() {
  if (context == nullptr) {
    context = ImNodes::EditorContextCreate();
  }
  ImNodes::EditorContextSet(context);

  ImNodes::GetIO().LinkDetachWithModifierClick.Modifier = &ImGui::GetIO().KeyCtrl;

  if (styled) {
    return;
  }
  styled = true;
  ImNodesStyle &style = ImNodes::GetStyle();
  style.NodeCornerRounding = 6.0F;
  style.NodePadding = ImVec2(9.0F, 7.0F);
  style.NodeBorderThickness = 1.0F;
  style.LinkThickness = 2.5F;
  style.PinCircleRadius = 4.5F;
  style.PinOffset = 0.0F;
  style.GridSpacing = 36.0F;
  style.Colors[ImNodesCol_GridBackground] = kGridBackground;
  style.Colors[ImNodesCol_GridLine] = kGridLine;
  style.Colors[ImNodesCol_GridLinePrimary] = kGridLinePrimary;
  style.Colors[ImNodesCol_NodeBackground] = kNodeBackground;
  style.Colors[ImNodesCol_NodeBackgroundHovered] = kNodeBackgroundHovered;
  style.Colors[ImNodesCol_NodeBackgroundSelected] = kNodeBackgroundSelected;
  style.Colors[ImNodesCol_NodeOutline] = kNodeOutline;
  style.Colors[ImNodesCol_Pin] = kPinColor;
  style.Colors[ImNodesCol_PinHovered] = kPinHovered;
  style.Colors[ImNodesCol_BoxSelector] = IM_COL32(60, 110, 140, 50);
  style.Colors[ImNodesCol_BoxSelectorOutline] = IM_COL32(90, 165, 210, 160);
  style.Colors[ImNodesCol_MiniMapBackground] = IM_COL32(23, 26, 31, 200);
  style.Colors[ImNodesCol_MiniMapBackgroundHovered] = IM_COL32(23, 26, 31, 240);
  style.Colors[ImNodesCol_MiniMapOutline] = IM_COL32(70, 82, 98, 200);
  style.Colors[ImNodesCol_MiniMapOutlineHovered] = IM_COL32(90, 165, 210, 255);
  style.Colors[ImNodesCol_MiniMapNodeBackground] = IM_COL32(70, 82, 98, 255);
  style.Colors[ImNodesCol_MiniMapNodeBackgroundHovered] = IM_COL32(100, 118, 140, 255);
  style.Colors[ImNodesCol_MiniMapNodeBackgroundSelected] = IM_COL32(90, 165, 210, 255);
  style.Colors[ImNodesCol_MiniMapNodeOutline] = IM_COL32(40, 46, 56, 255);
  style.Colors[ImNodesCol_MiniMapLink] = IM_COL32(110, 130, 150, 160);
  style.Colors[ImNodesCol_MiniMapLinkSelected] = IM_COL32(240, 180, 50, 255);
  style.Colors[ImNodesCol_MiniMapCanvas] = IM_COL32(200, 200, 200, 40);
  style.Colors[ImNodesCol_MiniMapCanvasOutline] = IM_COL32(200, 200, 200, 120);
}

int NodeCanvas::Impl::node_id(const std::string &name) {
  auto it = ids.find(name);
  if (it != ids.end()) {
    return it->second;
  }
  const int id = next_id++;
  ids.emplace(name, id);
  return id;
}

std::string NodeCanvas::Impl::section_of_attr(int attr, int &slot) const {
  slot = attr % kPinStride;
  auto it = names_by_id.find(attr / kPinStride);
  return it == names_by_id.end() ? std::string() : it->second;
}

const Mads::TopologyEdge *NodeCanvas::Impl::find_edge(const std::string &from, const std::string &to) const {
  for (const auto &edge : editor.edges()) {
    if (edge.from == from && edge.to == to) {
      return &edge;
    }
  }
  return nullptr;
}

// Pushes meta positions into imnodes for nodes that are new or whose meta no
// longer matches what the canvas last wrote (undo, reload, auto layout).
void NodeCanvas::Impl::sync_positions() {
  for (const auto &name : editor.nodes()) {
    const Mads::Ini::Section *section = editor.doc().find(name);
    if (section == nullptr || !section->meta.x || !section->meta.y) {
      continue;
    }
    const std::pair<float, float> wanted{*section->meta.x, *section->meta.y};
    auto it = last_written.find(name);
    if (it == last_written.end() || it->second != wanted) {
      ImNodes::SetNodeGridSpacePos(node_id(name), ImVec2(wanted.first, wanted.second));
      last_written[name] = wanted;
    }
  }
}

void NodeCanvas::Impl::draw_node(const std::string &name, int id) {
  const Mads::Ini::NodeType *type = editor.schema().type(editor.type_of(name));
  const std::string type_id = editor.type_of(name);
  const std::string role = type != nullptr ? type->role : "custom";
  const std::string exec_line = (type != nullptr && !type->executable.empty()) ? type->executable : type_id;

  Mads::AgentTopicInfo info;
  if (auto it = editor.topics().find(name); it != editor.topics().end()) {
    info = it->second;
  }

  // Input pins, one per declared sub_topic entry.
  const std::set<std::string> *dead_subs = nullptr;
  if (auto it = editor.dangling().sub.find(name); it != editor.dangling().sub.end()) {
    dead_subs = &it->second;
  }
  std::vector<InputPin> inputs;
  const int hidden = hidden_catch_all.count(name) != 0 ? hidden_catch_all[name] : 0;
  for (const auto &entry : info.sub_topic) {
    if (static_cast<int>(inputs.size()) >= kMaxInputs) {
      break;
    }
    InputPin pin;
    pin.entry = entry;
    pin.dangling = dead_subs != nullptr && dead_subs->count(entry) != 0;
    pin.wildcard = Mads::has_wildcard(entry);
    pin.label = (pin.dangling ? "! " : "") + pattern_label(entry);
    if (entry.empty() && hidden > 0) {
      pin.label += "  <- " + std::to_string(hidden);
    }
    inputs.push_back(std::move(pin));
  }
  const bool has_subscribe_pin = role != "source";

  // Output pin.
  const bool pub_dangling = editor.dangling().pub.count(name) != 0;
  const bool has_output = !info.pub_topic.empty() && !(role == "sink" && info.pub_implicit);
  const std::string out_label = (pub_dangling ? "! " : "") + info.pub_topic;

  // Content width, so that the output label can be right-aligned.
  const float pin_pad = 22.0F;
  float width = 110.0F;
  width = std::max(width, ImGui::CalcTextSize(name.c_str()).x);
  width = std::max(width, ImGui::CalcTextSize(exec_line.c_str()).x);
  for (const auto &pin : inputs) {
    width = std::max(width, ImGui::CalcTextSize(pin.label.c_str()).x + pin_pad);
  }
  if (has_subscribe_pin) {
    width = std::max(width, ImGui::CalcTextSize("+ subscribe").x + pin_pad);
  }
  if (has_output) {
    width = std::max(width, ImGui::CalcTextSize(out_label.c_str()).x + pin_pad);
  }

  const TitleColors colors = title_colors(role);
  ImNodes::PushColorStyle(ImNodesCol_TitleBar, colors.normal);
  ImNodes::PushColorStyle(ImNodesCol_TitleBarHovered, colors.hovered);
  ImNodes::PushColorStyle(ImNodesCol_TitleBarSelected, colors.selected);
  ImNodes::BeginNode(id);

  ImNodes::BeginNodeTitleBar();
  ImGui::TextUnformatted(name.c_str());
  ImNodes::EndNodeTitleBar();
  ImGui::TextColored(kTextDim, "%s", exec_line.c_str());
  ImGui::Dummy(ImVec2(width, 2.0F));

  for (size_t k = 0; k < inputs.size(); ++k) {
    const InputPin &pin = inputs[k];
    const int attr = input_attr(id, static_cast<int>(k));
    ImNodes::BeginInputAttribute(attr, connected_attrs.count(attr) != 0 ? ImNodesPinShape_CircleFilled : ImNodesPinShape_Circle);
    if (pin.dangling) {
      ImGui::TextColored(kTextWarning, "%s", pin.label.c_str());
    } else if (pin.wildcard) {
      ImGui::TextColored(kTextWildcard, "%s", pin.label.c_str());
    } else {
      ImGui::TextUnformatted(pin.label.c_str());
    }
    ImNodes::EndInputAttribute();
  }
  if (has_subscribe_pin) {
    ImNodes::BeginInputAttribute(input_attr(id, kSubscribeSlot), ImNodesPinShape_Circle);
    ImGui::TextColored(kTextDim, "+ subscribe");
    ImNodes::EndInputAttribute();
  }
  if (has_output) {
    const int attr = input_attr(id, kOutputSlot);
    ImNodes::BeginOutputAttribute(attr, connected_attrs.count(attr) != 0 ? ImNodesPinShape_CircleFilled : ImNodesPinShape_Circle);
    const float indent = std::max(0.0F, width - ImGui::CalcTextSize(out_label.c_str()).x - pin_pad + 8.0F);
    ImGui::Indent(indent);
    if (pub_dangling) {
      ImGui::TextColored(kTextWarning, "%s", out_label.c_str());
    } else if (info.pub_implicit) {
      ImGui::TextColored(kTextDim, "%s", out_label.c_str());
    } else {
      ImGui::TextUnformatted(out_label.c_str());
    }
    ImGui::Unindent(indent);
    ImNodes::EndOutputAttribute();
  }

  ImNodes::EndNode();
  ImNodes::PopColorStyle();
  ImNodes::PopColorStyle();
  ImNodes::PopColorStyle();
}

// Submits one imnodes link per drawn edge; the link id is the edge index.
void NodeCanvas::Impl::draw_links() {
  const auto &edges = editor.edges();
  for (size_t i = 0; i < edges.size(); ++i) {
    const Mads::TopologyEdge &edge = edges[i];
    if (edge.catch_all && !ui.show_catch_all) {
      continue;
    }
    auto from_id = ids.find(edge.from);
    auto to_id = ids.find(edge.to);
    auto to_info = editor.topics().find(edge.to);
    auto from_info = editor.topics().find(edge.from);
    if (from_id == ids.end() || to_id == ids.end() || to_info == editor.topics().end() || from_info == editor.topics().end()) {
      continue;
    }
    // The publisher must show an output pin.
    const Mads::Ini::NodeType *from_type = editor.schema().type(editor.type_of(edge.from));
    if (from_type != nullptr && from_type->role == "sink" && from_info->second.pub_implicit) {
      continue;
    }
    // Target: the first input pin whose entry matches the topic.
    int slot = -1;
    const auto &subs = to_info->second.sub_topic;
    for (size_t k = 0; k < subs.size() && k < static_cast<size_t>(kMaxInputs); ++k) {
      if (Mads::subscription_match(subs[k], edge.topic) != Mads::SubMatch::None) {
        slot = static_cast<int>(k);
        break;
      }
    }
    if (slot < 0) {
      continue;
    }
    const ImU32 color = link_color(edge);
    ImNodes::PushColorStyle(ImNodesCol_Link, color);
    ImNodes::PushColorStyle(ImNodesCol_LinkHovered, brighten(color, 0.25F));
    ImNodes::PushColorStyle(ImNodesCol_LinkSelected, brighten(color, 0.4F));
    ImNodes::Link(static_cast<int>(i), input_attr(from_id->second, kOutputSlot), input_attr(to_id->second, slot));
    ImNodes::PopColorStyle();
    ImNodes::PopColorStyle();
    ImNodes::PopColorStyle();
  }
}

// Which input/output pins carry a link, and how many catch-all publishers
// hide behind each subscriber's "(all)" pin. Needed before drawing nodes.
static void collect_link_state(const Mads::Ini::Editor &editor, const UiState &ui, const std::map<std::string, int> &ids,
                               std::set<int> &connected, std::map<std::string, int> &hidden) {
  connected.clear();
  hidden.clear();
  for (const auto &edge : editor.edges()) {
    if (edge.catch_all && !ui.show_catch_all) {
      ++hidden[edge.to];
      continue;
    }
    auto from_id = ids.find(edge.from);
    auto to_id = ids.find(edge.to);
    auto to_info = editor.topics().find(edge.to);
    if (from_id == ids.end() || to_id == ids.end() || to_info == editor.topics().end()) {
      continue;
    }
    const auto &subs = to_info->second.sub_topic;
    for (size_t k = 0; k < subs.size() && k < static_cast<size_t>(kMaxInputs); ++k) {
      if (Mads::subscription_match(subs[k], edge.topic) != Mads::SubMatch::None) {
        connected.insert(input_attr(to_id->second, static_cast<int>(k)));
        connected.insert(input_attr(from_id->second, kOutputSlot));
        break;
      }
    }
  }
}

void NodeCanvas::Impl::disconnect_link(const std::string &from, const std::string &to) {
  // The edge is looked up before the call: a successful disconnect commits
  // and replaces the edge list.
  std::string topic;
  std::string pattern;
  if (const Mads::TopologyEdge *edge = find_edge(from, to); edge != nullptr) {
    topic = edge->topic;
    if (!edge->via_patterns.empty()) {
      pattern = pattern_label(edge->via_patterns.front());
    }
  }
  const auto result = editor.disconnect(from, to);
  if (result == Mads::Ini::Editor::DisconnectResult::ViaPattern) {
    ui.notify("'" + to + "' receives '" + topic + "' via pattern '" + pattern + "': edit its sub_topic", true);
  }
}

void NodeCanvas::Impl::handle_link_events() {
  // New link: output pin -> input pin of another node (either drag direction).
  int start = 0;
  int end = 0;
  if (ImNodes::IsLinkCreated(&start, &end)) {
    int start_slot = 0;
    int end_slot = 0;
    std::string start_name = section_of_attr(start, start_slot);
    std::string end_name = section_of_attr(end, end_slot);
    if (!start_name.empty() && !end_name.empty()) {
      if (start_slot != kOutputSlot && end_slot == kOutputSlot) {
        std::swap(start_name, end_name);
        std::swap(start_slot, end_slot);
      }
      if (start_slot == kOutputSlot && end_slot != kOutputSlot) {
        const std::string &from = start_name;
        const std::string &to = end_name;
        std::string via;
        std::string topic;
        if (auto it = editor.topics().find(from); it != editor.topics().end()) {
          topic = it->second.pub_topic;
        }
        switch (editor.connect(from, to, &via)) {
        case Mads::Ini::Editor::ConnectResult::Added:
          break;
        case Mads::Ini::Editor::ConnectResult::AlreadyMatched:
          ui.notify("'" + to + "' already receives '" + topic + "' via '" + pattern_label(via) + "'");
          break;
        case Mads::Ini::Editor::ConnectResult::Invalid:
          ui.notify("Cannot connect '" + from + "' to '" + to + "'", true);
          break;
        }
      }
    }
  }

  // Detached link (Ctrl+click on a pin): same as deleting it.
  int destroyed = 0;
  if (ImNodes::IsLinkDestroyed(&destroyed)) {
    const auto &edges = editor.edges();
    if (destroyed >= 0 && static_cast<size_t>(destroyed) < edges.size()) {
      const std::string from = edges[destroyed].from;
      const std::string to = edges[destroyed].to;
      disconnect_link(from, to);
    }
  }
}

// imnodes -> ui.selected: a single selected node becomes the selection. A
// click on the empty canvas keeps the last selection on purpose.
void NodeCanvas::Impl::handle_selection_after_editor() {
  if (ImNodes::NumSelectedNodes() != 1) {
    return;
  }
  int node = 0;
  ImNodes::GetSelectedNodes(&node);
  auto it = names_by_id.find(node);
  if (it != names_by_id.end() && it->second != ui.selected) {
    ui.selected = it->second;
    synced_selected = ui.selected;
  }
}

// On mouse release: nodes whose grid position differs from what the canvas
// last wrote were dragged; store them in the meta, one undo step.
void NodeCanvas::Impl::handle_drag_end() {
  if (!ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
    return;
  }
  bool moved = false;
  for (const auto &name : editor.nodes()) {
    auto it = last_written.find(name);
    auto id = ids.find(name);
    if (it == last_written.end() || id == ids.end()) {
      continue;
    }
    const ImVec2 pos = ImNodes::GetNodeGridSpacePos(id->second);
    if (std::fabs(pos.x - it->second.first) < 0.5F && std::fabs(pos.y - it->second.second) < 0.5F) {
      continue;
    }
    // Written back through the document, so the meta now equals this value.
    last_written[name] = {pos.x, pos.y};
    editor.set_position(name, pos.x, pos.y, false);
    moved = true;
  }
  if (moved) {
    editor.commit("Move nodes");
  }
}

void NodeCanvas::Impl::remove_nodes(const std::vector<std::string> &names) {
  for (const auto &name : names) {
    if (editor.delete_node(name) && ui.selected == name) {
      ui.selected.clear();
      synced_selected.clear();
    }
  }
}

void NodeCanvas::Impl::handle_delete_keys() {
  const bool in_canvas = ImNodes::IsEditorHovered() || ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows);
  if (!in_canvas || ImGui::GetIO().WantTextInput) {
    return;
  }
  if (!ImGui::IsKeyPressed(ImGuiKey_Delete, false) && !ImGui::IsKeyPressed(ImGuiKey_Backspace, false)) {
    return;
  }

  // Resolve the selection to names before touching the document.
  std::vector<std::pair<std::string, std::string>> link_pairs;
  const int link_count = ImNodes::NumSelectedLinks();
  if (link_count > 0) {
    std::vector<int> selected_links(static_cast<size_t>(link_count));
    ImNodes::GetSelectedLinks(selected_links.data());
    for (int link : selected_links) {
      if (link >= 0 && static_cast<size_t>(link) < editor.edges().size()) {
        link_pairs.emplace_back(editor.edges()[link].from, editor.edges()[link].to);
      }
    }
  }
  std::vector<std::string> node_names;
  const int node_count = ImNodes::NumSelectedNodes();
  if (node_count > 0) {
    std::vector<int> selected_nodes(static_cast<size_t>(node_count));
    ImNodes::GetSelectedNodes(selected_nodes.data());
    for (int node : selected_nodes) {
      if (auto it = names_by_id.find(node); it != names_by_id.end()) {
        node_names.push_back(it->second);
      }
    }
  }
  if (link_pairs.empty() && node_names.empty()) {
    return;
  }

  for (const auto &[from, to] : link_pairs) {
    disconnect_link(from, to);
  }
  remove_nodes(node_names);
  ImNodes::ClearLinkSelection();
  ImNodes::ClearNodeSelection();
}

// ui.focus_request: pan so that the node is centered, and select it.
void NodeCanvas::Impl::handle_focus_request() {
  if (ui.focus_request.empty()) {
    return;
  }
  auto it = ids.find(ui.focus_request);
  if (it != ids.end() && editor.doc().find(ui.focus_request) != nullptr) {
    const ImVec2 pos = ImNodes::GetNodeGridSpacePos(it->second);
    const ImVec2 dims = ImNodes::GetNodeDimensions(it->second);
    const ImVec2 center(pos.x + dims.x * 0.5F, pos.y + dims.y * 0.5F);
    ImNodes::EditorContextResetPanning(ImVec2(canvas_size.x * 0.5F - center.x, canvas_size.y * 0.5F - center.y));
    ImNodes::ClearNodeSelection();
    ImNodes::SelectNode(it->second);
    ui.selected = ui.focus_request;
    synced_selected = ui.selected;
  }
  ui.focus_request.clear();
}

void NodeCanvas::Impl::handle_context_menus() {
  int hovered_node = 0;
  int hovered_link_id = 0;
  const bool node_hovered = ImNodes::IsNodeHovered(&hovered_node);
  const bool link_hovered = ImNodes::IsLinkHovered(&hovered_link_id);
  if (ImGui::IsMouseReleased(ImGuiMouseButton_Right) && ImNodes::IsEditorHovered()) {
    if (node_hovered) {
      if (auto it = names_by_id.find(hovered_node); it != names_by_id.end()) {
        menu_node = it->second;
        ImGui::OpenPopup(kNodeMenu);
      }
    } else if (!link_hovered) {
      const ImVec2 mouse = ImGui::GetMousePos();
      const ImVec2 pan = ImNodes::EditorContextGetPanning();
      menu_grid_pos = ImVec2(mouse.x - canvas_origin.x - pan.x, mouse.y - canvas_origin.y - pan.y);
      ImGui::OpenPopup(kCanvasMenu);
    }
  }

  if (ImGui::BeginPopup(kNodeMenu)) {
    if (ImGui::MenuItem("Rename...")) {
      want_rename = true;
    }
    if (ImGui::MenuItem("Duplicate")) {
      const std::string copy = editor.duplicate_node(menu_node);
      if (copy.empty()) {
        ui.notify("Cannot duplicate [" + menu_node + "]", true);
      } else {
        ui.selected = copy;
      }
    }
    if (ImGui::MenuItem("Delete")) {
      remove_nodes({menu_node});
    }
    ImGui::EndPopup();
  }
  // Opened outside the menu so that the popup lives in the canvas ID scope.
  if (want_rename) {
    want_rename = false;
    std::snprintf(rename_buffer, sizeof(rename_buffer), "%s", menu_node.c_str());
    rename_error.clear();
    ImGui::OpenPopup(kRenamePopup);
  }

  if (ImGui::BeginPopup(kCanvasMenu)) {
    if (ImGui::BeginMenu("Add node")) {
      std::vector<std::string> categories;
      for (const auto &type : editor.schema().types()) {
        if (!type.document && std::find(categories.begin(), categories.end(), type.category) == categories.end()) {
          categories.push_back(type.category);
        }
      }
      for (const auto &category : categories) {
        if (!ImGui::BeginMenu(category.c_str())) {
          continue;
        }
        for (const auto &type : editor.schema().types()) {
          if (type.document || type.category != category) {
            continue;
          }
          if (ImGui::MenuItem(type.label.c_str())) {
            const std::string name = editor.add_node(type.id, menu_grid_pos.x, menu_grid_pos.y);
            if (!name.empty()) {
              ui.selected = name;
            }
          }
        }
        ImGui::EndMenu();
      }
      ImGui::EndMenu();
    }
    if (ImGui::MenuItem("Auto layout")) {
      editor.auto_layout(true);
    }
    ImGui::MenuItem("Show catch-all links", nullptr, &ui.show_catch_all);
    ImGui::EndPopup();
  }
}

void NodeCanvas::Impl::draw_rename_popup() {
  if (!ImGui::BeginPopupModal(kRenamePopup, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
    return;
  }
  ImGui::Text("Rename [%s]", menu_node.c_str());
  if (ImGui::IsWindowAppearing()) {
    ImGui::SetKeyboardFocusHere();
  }
  const bool entered = ImGui::InputText("##new_name", rename_buffer, sizeof(rename_buffer), ImGuiInputTextFlags_EnterReturnsTrue);
  const std::string new_name = rename_buffer;
  bool apply = entered;
  if (!rename_error.empty()) {
    ImGui::TextColored(kTextWarning, "%s", rename_error.c_str());
  }
  if (ImGui::Button("Rename")) {
    apply = true;
  }
  ImGui::SameLine();
  const bool cancel = ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape);

  if (apply) {
    if (new_name == menu_node) {
      ImGui::CloseCurrentPopup();
    } else if (!Mads::Ini::is_bare_key(new_name)) {
      rename_error = "Not a valid section name (letters, digits, '_' and '-')";
    } else if (editor.doc().find(new_name) != nullptr) {
      rename_error = "A section named '" + new_name + "' exists already";
    } else if (!editor.rename_node(menu_node, new_name)) {
      rename_error = "Cannot rename [" + menu_node + "]";
    } else {
      // The node keeps its imnodes id and its position.
      if (auto it = ids.find(menu_node); it != ids.end()) {
        const int id = it->second;
        ids.erase(it);
        ids[new_name] = id;
      }
      if (auto it = last_written.find(menu_node); it != last_written.end()) {
        last_written[new_name] = it->second;
        last_written.erase(menu_node);
      }
      if (ui.selected == menu_node) {
        ui.selected = new_name;
        synced_selected = new_name;
      }
      ImGui::CloseCurrentPopup();
    }
  } else if (cancel) {
    ImGui::CloseCurrentPopup();
  }
  ImGui::EndPopup();
}

NodeCanvas::NodeCanvas(Mads::Ini::Editor &editor, UiState &ui) : _impl(new Impl(editor, ui)) {}

NodeCanvas::~NodeCanvas() {
  delete _impl;
}

void NodeCanvas::draw() {
  Impl &s = *_impl;
  s.setup_context();

  // Nodes without a position get one before anything is drawn.
  for (const auto &name : s.editor.nodes()) {
    const Mads::Ini::Section *section = s.editor.doc().find(name);
    if (section != nullptr && (!section->meta.x || !section->meta.y)) {
      s.editor.auto_layout(false);
      break;
    }
  }

  // Ids and per-frame maps.
  s.names_by_id.clear();
  for (const auto &name : s.editor.nodes()) {
    s.names_by_id[s.node_id(name)] = name;
  }
  collect_link_state(s.editor, s.ui, s.ids, s.connected_attrs, s.hidden_catch_all);

  s.canvas_origin = ImGui::GetCursorScreenPos();
  s.canvas_size = ImGui::GetContentRegionAvail();

  ImNodes::BeginNodeEditor();
  s.sync_positions();
  for (const auto &name : s.editor.nodes()) {
    s.draw_node(name, s.ids[name]);
  }
  s.draw_links();

  // ui.selected -> imnodes, only when it changed elsewhere and differs from
  // what imnodes already shows (so the user's own clicks are not fought).
  if (s.ui.selected != s.synced_selected) {
    s.synced_selected = s.ui.selected;
    auto it = s.ids.find(s.ui.selected);
    const bool is_node = !s.ui.selected.empty() && it != s.ids.end() && s.names_by_id.count(it->second) != 0;
    if (is_node) {
      int current = -1;
      if (ImNodes::NumSelectedNodes() == 1) {
        ImNodes::GetSelectedNodes(&current);
      }
      if (current != it->second) {
        ImNodes::ClearNodeSelection();
        ImNodes::SelectNode(it->second);
      }
    } else if (s.ui.selected.empty()) {
      ImNodes::ClearNodeSelection();
    }
  }

  if (s.ui.show_minimap) {
    ImNodes::MiniMap(0.2F, ImNodesMiniMapLocation_BottomRight);
  }
  ImNodes::EndNodeEditor();

  // Visible center in grid space, for the palette.
  const ImVec2 pan = ImNodes::EditorContextGetPanning();
  s.view_center_grid = ImVec2(s.canvas_size.x * 0.5F - pan.x, s.canvas_size.y * 0.5F - pan.y);

  s.handle_link_events();
  s.handle_selection_after_editor();
  s.handle_drag_end();

  // Link tooltip.
  int link = -1;
  if (ImNodes::IsLinkHovered(&link) && link >= 0 && static_cast<size_t>(link) < s.editor.edges().size()) {
    const Mads::TopologyEdge &edge = s.editor.edges()[link];
    if (ImGui::BeginTooltip()) {
      ImGui::Text("'%s'", edge.topic.c_str());
      if (edge.kind != Mads::SubMatch::Exact) {
        for (const auto &pattern : edge.via_patterns) {
          ImGui::TextColored(kTextDim, "via %s", pattern_label(pattern).c_str());
        }
      }
      if (edge.implicit_pub) {
        ImGui::TextColored(kTextDim, "implicit pub_topic");
      }
      ImGui::EndTooltip();
    }
  }

  s.handle_focus_request();
  s.handle_delete_keys();
  s.handle_context_menus();
  s.draw_rename_popup();
}

void NodeCanvas::view_center(float &x, float &y) const {
  x = _impl->view_center_grid.x;
  y = _impl->view_center_grid.y;
}

} // namespace MadsIni
