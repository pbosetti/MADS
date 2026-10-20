#include "ui_state.hpp"

#include <imgui.h>

namespace MadsIni {

void UiState::notify(const std::string &message, bool error) {
  toast = message;
  toast_error = error;
  toast_until = ImGui::GetTime() + 4.0;
}

} // namespace MadsIni
