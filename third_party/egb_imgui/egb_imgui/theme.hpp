#pragma once

#include "imgui/imgui.h"

// Eigenblue brand theme, the single source for the UI palette. Dark mode with
// the electric accent for focus/selection. Use theme::palette, not literals.
namespace theme {

// 0xRRGGBB -> ImVec4 (8-bit sRGB, as ImGui expects).
inline ImVec4 rgb(unsigned int hex, float a = 1.0f) {
  return ImVec4(((hex >> 16) & 0xFF) / 255.0f, ((hex >> 8) & 0xFF) / 255.0f,
                (hex & 0xFF) / 255.0f, a);
}

// The same color at a different alpha.
inline ImVec4 with_alpha(const ImVec4 &c, float a) {
  return ImVec4(c.x, c.y, c.z, a);
}

// Composite fg (with its alpha) over opaque bg, returned opaque. Gives the flat
// color a translucent fill would show on a known background.
inline ImVec4 over(const ImVec4 &fg, const ImVec4 &bg) {
  const float a = fg.w;
  return ImVec4(fg.x * a + bg.x * (1.0f - a), fg.y * a + bg.y * (1.0f - a),
                fg.z * a + bg.z * (1.0f - a), 1.0f);
}

// Brand palette.
namespace palette {
inline const ImVec4 blue = rgb(0x5581B0);    // secondary interactive
inline const ImVec4 green = rgb(0x53684C);   // tertiary / calm accents
inline const ImVec4 plum = rgb(0x3F2F3A);    // deep neutral
inline const ImVec4 accent = rgb(0xE7FF00);  // primary accent: focus/selection
inline const ImVec4 bg = rgb(0x020617);      // window background
inline const ImVec4 surface = rgb(0x0F172A); // panels, inputs, popups
inline const ImVec4 border = rgb(0x292E42);  // separators, frame borders
inline const ImVec4 text = rgb(0xEDE6EA);    // body text
inline const ImVec4 muted = rgb(0x9E86A6);   // disabled / secondary text
// Signal colors, high-chroma so they pop, distinct from the accent.
inline const ImVec4 success = rgb(0x2BE06B); // ok / available - vivid green
inline const ImVec4 warning = rgb(0xFFB020); // pending / caution - vivid amber
inline const ImVec4 danger = rgb(0xFF453A);  // error / stop - vivid red
inline const ImVec4 info = rgb(0x40B5FF);    // queued / highlight - vivid azure
} // namespace palette

// One source for spacing, rounding, borders. Use instead of literals.
namespace metrics {
inline constexpr float rounding = 6.0f;      // windows, popups, frames, cards
inline constexpr float border_size = 1.0f;   // frame / window / popup borders
inline constexpr float window_pad_x = 10.0f; // window / child inner padding
inline constexpr float window_pad_y = 8.0f;
inline constexpr float item_spacing_x = 8.0f; // gap between items
inline constexpr float item_spacing_y = 6.0f;
inline constexpr float overlay_edge = 8.0f;  // gap from the canvas edge
inline constexpr float overlay_pad_x = 9.0f; // inside map-overlay cards
inline constexpr float overlay_pad_y = 6.0f;
} // namespace metrics

// Apply the brand dark theme to `style` (the current style when null). Call
// once after ImGui::CreateContext().
inline void apply(ImGuiStyle *style = nullptr) {
  using namespace palette;
  ImGuiStyle &s = style ? *style : ImGui::GetStyle();
  ImGui::StyleColorsDark(&s); // sane baseline, then brand overrides
  ImVec4 *c = s.Colors;

  c[ImGuiCol_Text] = text;
  c[ImGuiCol_TextDisabled] = muted;
  c[ImGuiCol_WindowBg] = bg;
  c[ImGuiCol_ChildBg] = ImVec4(0, 0, 0, 0); // inherit parent background
  c[ImGuiCol_PopupBg] = surface;
  c[ImGuiCol_Border] = border;
  c[ImGuiCol_BorderShadow] = ImVec4(0, 0, 0, 0);

  c[ImGuiCol_FrameBg] = surface;
  c[ImGuiCol_FrameBgHovered] = with_alpha(blue, 0.45f);
  c[ImGuiCol_FrameBgActive] = with_alpha(blue, 0.65f);

  c[ImGuiCol_TitleBg] = surface;
  c[ImGuiCol_TitleBgActive] = border;
  c[ImGuiCol_TitleBgCollapsed] = bg;
  c[ImGuiCol_MenuBarBg] = surface;

  c[ImGuiCol_ScrollbarBg] = bg;
  c[ImGuiCol_ScrollbarGrab] = border;
  c[ImGuiCol_ScrollbarGrabHovered] = with_alpha(blue, 0.80f);
  c[ImGuiCol_ScrollbarGrabActive] = blue;

  c[ImGuiCol_CheckMark] = accent; // selection indicator - accent belongs here
  c[ImGuiCol_SliderGrab] = with_alpha(blue, 0.80f);
  c[ImGuiCol_SliderGrabActive] = blue;

  // Controls ramp brightness within one hue. Accent is reserved for selection.
  c[ImGuiCol_Button] = with_alpha(blue, 0.45f);
  c[ImGuiCol_ButtonHovered] = with_alpha(blue, 0.70f);
  c[ImGuiCol_ButtonActive] = blue;

  c[ImGuiCol_Header] = with_alpha(blue, 0.45f);
  c[ImGuiCol_HeaderHovered] = with_alpha(blue, 0.70f);
  c[ImGuiCol_HeaderActive] = with_alpha(blue, 0.90f);

  c[ImGuiCol_Separator] = border;
  c[ImGuiCol_SeparatorHovered] = with_alpha(blue, 0.70f);
  c[ImGuiCol_SeparatorActive] = blue;

  c[ImGuiCol_ResizeGrip] = with_alpha(blue, 0.30f);
  c[ImGuiCol_ResizeGripHovered] = with_alpha(blue, 0.65f);
  c[ImGuiCol_ResizeGripActive] = blue;

  c[ImGuiCol_Tab] = surface;
  c[ImGuiCol_TabHovered] = with_alpha(blue, 0.80f);
  c[ImGuiCol_TabSelected] = with_alpha(blue, 0.60f);
  c[ImGuiCol_TabDimmed] = bg;
  c[ImGuiCol_TabDimmedSelected] = surface;

  c[ImGuiCol_DockingPreview] = with_alpha(accent, 0.55f);
  c[ImGuiCol_DockingEmptyBg] = bg;

  c[ImGuiCol_PlotLines] = accent;
  c[ImGuiCol_PlotLinesHovered] = info;
  c[ImGuiCol_PlotHistogram] = blue;
  c[ImGuiCol_PlotHistogramHovered] = accent;

  c[ImGuiCol_TableHeaderBg] = surface;
  c[ImGuiCol_TableBorderStrong] = border;
  c[ImGuiCol_TableBorderLight] = with_alpha(border, 0.5f);
  c[ImGuiCol_TableRowBg] = ImVec4(0, 0, 0, 0);
  c[ImGuiCol_TableRowBgAlt] = with_alpha(surface, 0.40f);

  c[ImGuiCol_TextSelectedBg] = with_alpha(accent, 0.35f);
  c[ImGuiCol_NavCursor] = accent;
  c[ImGuiCol_DragDropTarget] = accent;
  c[ImGuiCol_ModalWindowDimBg] = with_alpha(bg, 0.60f);

  // Spacing/rounding/borders from metrics. Layout stays as the app sets it.
  s.WindowPadding = ImVec2(metrics::window_pad_x, metrics::window_pad_y);
  s.ItemSpacing = ImVec2(metrics::item_spacing_x, metrics::item_spacing_y);
  s.FrameRounding = metrics::rounding;
  s.GrabRounding = metrics::rounding;
  s.PopupRounding = metrics::rounding;
  // ChildRounding 0: layout bars stay square. Cards push their own rounding.
  s.ScrollbarRounding = metrics::rounding;
  s.TabRounding = metrics::rounding;
  s.FrameBorderSize = metrics::border_size;
}

} // namespace theme
