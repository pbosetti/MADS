// View state of the mads-ini editor shared by the shell, the canvas and the
// inspector. The document itself lives in Mads::Ini::Editor.
#pragma once

#include <string>

namespace MadsIni {

struct UiState {
  /// Selected section name; "" selects the document settings
  /// ([agents]/[broker]) in the inspector.
  std::string selected;
  /// Draw one link per catch-all (sub_topic = [""]) subscription instead of
  /// a badge on the subscriber.
  bool show_catch_all = false;
  bool show_minimap = true;
  /// Set by the shell when a node was added, so that the canvas scrolls it
  /// into view on the next frame; cleared by the canvas.
  std::string focus_request;

  /// Transient message shown in the status footer.
  void notify(const std::string &message, bool error = false);
  std::string toast;
  bool toast_error = false;
  double toast_until = 0.0; // ImGui::GetTime() deadline
};

} // namespace MadsIni
