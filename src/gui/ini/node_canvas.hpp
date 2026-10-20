// Node-graph view of a mads.ini document (imnodes): one node per agent
// section, links derived from the declared topics.
#pragma once

#include "ui_state.hpp"

#include "ini_editor.hpp"

#include <map>
#include <string>

namespace MadsIni {

class NodeCanvas {
public:
  NodeCanvas(Mads::Ini::Editor &editor, UiState &ui);
  ~NodeCanvas();
  /// Draws the editor filling the current content region. Must be called
  /// between ImGui::NewFrame() and ImGui::Render().
  void draw();
  /// Canvas (grid-space) coordinates of the visible center: where the
  /// palette drops new nodes.
  void view_center(float &x, float &y) const;

private:
  struct Impl;
  Impl *_impl;
};

} // namespace MadsIni
