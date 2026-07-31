#pragma once

#include "imgui/imgui.h"
#include <filesystem>
#include <initializer_list>

namespace theme {

// Load bundled Inter at size_px, trying each candidate path. Falls back to
// ImGui's built-in font if none resolve.
inline ImFont *
load_default_font(float size_px = 15.0f,
                  std::initializer_list<const char *> candidates = {
                      "egb_imgui/fonts/Inter.ttf"}) {
  ImGuiIO &io = ImGui::GetIO();
  for (const char *path : candidates) {
    if (path && std::filesystem::exists(path))
      return io.Fonts->AddFontFromFileTTF(path, size_px);
  }
  return io.Fonts->AddFontDefault();
}

} // namespace theme
