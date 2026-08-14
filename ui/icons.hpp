#pragma once

#include "imgui/imgui.h"
#include "ui/IconsMaterialDesignIcons.h"
#include <filesystem>
#include <initializer_list>

// Material Design Icons (Pictogrammers, Apache-2.0), used via ICON_MDI_*
// macros. Needs IMGUI_USE_WCHAR32 (codepoints above U+FFFF) or the glyphs
// render blank.
namespace theme {

// Merge the MDI glyphs in `used` into the atlas at size_px. Call after the text
// font. Returns null if the .ttf isn't found.
inline ImFont *load_icons(float size_px,
                          std::initializer_list<const char *> used,
                          std::initializer_list<const char *> candidates = {
                              "materialdesignicons-webfont.ttf",
                              "ui/fonts/materialdesignicons-webfont.ttf"}) {
  const char *path = nullptr;
  for (const char *p : candidates) {
    if (p && std::filesystem::exists(p)) {
      path = p;
      break;
    }
  }
  if (!path)
    return nullptr;

  // The ranges must outlive the atlas build, so keep them here. Call once.
  static ImVector<ImWchar> ranges;
  ImFontGlyphRangesBuilder builder;
  for (const char *ic : used)
    builder.AddText(ic);
  ranges.clear();
  builder.BuildRanges(&ranges);

  ImFontConfig cfg;
  cfg.MergeMode = true;
  cfg.PixelSnapH = true;
  cfg.GlyphMinAdvanceX = size_px; // put the icons on a square advance
  // MDI glyphs sit high on the em box, nudge down so they line up with the
  // text baseline when drawn inline with labels.
  cfg.GlyphOffset.y = 2.0f;
  ImGuiIO &io = ImGui::GetIO();
  return io.Fonts->AddFontFromFileTTF(path, size_px, &cfg, ranges.Data);
}

} // namespace theme
