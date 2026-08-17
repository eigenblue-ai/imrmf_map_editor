#pragma once
#include "imgui/imgui.h"
#include "imgui/imgui_internal.h" // BeginOverlayCard cursor/content restore
#include "ui/theme.hpp"
#include <algorithm>
#include <cstdarg>
#include <string>
#include <vector>

namespace ImGuiWidgets {

// Tooltip on the previous item.
inline void ItemTooltip(const char *fmt, ...) {
  if (!ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
    return;
  va_list args;
  va_start(args, fmt);
  ImGui::SetTooltipV(fmt, args);
  va_end(args);
}

// Right-align the next item of the given width in the current row.
inline void SameLineRight(float item_width) {
  ImGui::SameLine();
  const float x =
      ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - item_width;
  if (x > ImGui::GetCursorPosX())
    ImGui::SetCursorPosX(x);
}

// 2-column label/control form table that scales with the panel. Pair rows with
// FormRow + FormControlWidth, then ImGui::EndTable().
inline bool BeginFormTable(const char *id) {
  if (!ImGui::BeginTable(
          id, 2, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_PadOuterX))
    return false;
  ImGui::TableSetupColumn("##label", ImGuiTableColumnFlags_WidthStretch, 0.42f);
  ImGui::TableSetupColumn("##control", ImGuiTableColumnFlags_WidthStretch,
                          0.58f);
  return true;
}

// One-line text, ellipsized to fit. Hover shows the full text.
inline void TextEllipsis(const char *text) {
  const float avail = ImGui::GetContentRegionAvail().x;
  if (ImGui::CalcTextSize(text).x <= avail) {
    ImGui::TextUnformatted(text);
    return;
  }
  const char *ell = "\xE2\x80\xA6"; // …
  const float budget = avail - ImGui::CalcTextSize(ell).x;
  std::string s(text);
  size_t n = s.size();
  while (n > 0) {
    if (ImGui::CalcTextSize(s.c_str(), s.c_str() + n).x <= budget)
      break;
    --n;
    while (n > 0 && (((unsigned char)s[n]) & 0xC0) == 0x80) // keep utf-8 whole
      --n;
  }
  std::string shown = s.substr(0, n) + ell;
  ImGui::TextUnformatted(shown.c_str());
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip("%s", text);
}

// Icon + text, both frame-aligned so the icon doesn't ride high. Optional tint.
inline void IconText(const char *icon, const char *text,
                     const ImVec4 *icon_color = nullptr) {
  ImGui::AlignTextToFramePadding();
  if (icon_color)
    ImGui::PushStyleColor(ImGuiCol_Text, *icon_color);
  ImGui::TextUnformatted(icon);
  if (icon_color)
    ImGui::PopStyleColor();
  ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted(text);
}

// Icon + text with the icon tinted by semantic state (a status pill). Pass
// tint_text to color the label too.
inline void StatusText(const char *icon, const char *text, theme::Signal s,
                       bool tint_text = false) {
  const ImVec4 col = theme::signal_color(s);
  if (tint_text)
    ImGui::PushStyleColor(ImGuiCol_Text, col);
  IconText(icon, text, &col);
  if (tint_text)
    ImGui::PopStyleColor();
}

// Wrapped status line for async ops, colored by state.
inline void StatusLine(theme::Signal s, const char *text) {
  ImGui::PushStyleColor(ImGuiCol_Text, theme::signal_color(s));
  ImGui::TextWrapped("%s", text);
  ImGui::PopStyleColor();
}

// Destructive-action button: the standard hover ramp, in danger red.
inline bool DangerButton(const char *label, const ImVec2 &size = ImVec2(0, 0)) {
  ImGui::PushStyleColor(ImGuiCol_Button,
                        theme::with_alpha(theme::palette::danger, 0.75f));
  ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                        theme::with_alpha(theme::palette::danger, 0.90f));
  ImGui::PushStyleColor(ImGuiCol_ButtonActive, theme::palette::danger);
  const bool pressed = ImGui::Button(label, size);
  ImGui::PopStyleColor(3);
  return pressed;
}

// Icon button that reads as pressed-in while active. Returns true on click.
// Does not SameLine itself.
inline bool ToolbarToggle(const char *icon, const char *tooltip, bool active) {
  if (active)
    ImGui::PushStyleColor(ImGuiCol_Button, theme::palette::blue);
  const bool clicked = ImGui::Button(icon);
  if (active)
    ImGui::PopStyleColor();
  if (tooltip)
    ItemTooltip("%s", tooltip);
  return clicked;
}

// Muted vertical divider between toolbar groups. Leaves the cursor on the
// same line.
inline void ToolbarSeparator() {
  ImGui::AlignTextToFramePadding();
  ImGui::TextDisabled("|");
  ImGui::SameLine();
}

// Form row: ellipsized label in the left column, cursor left in the right.
inline void FormRow(const char *label) {
  ImGui::TableNextRow();
  ImGui::TableNextColumn();
  ImGui::AlignTextToFramePadding();
  TextEllipsis(label);
  ImGui::TableNextColumn();
}

// Control width in a form row: fills the cell up to a readable cap.
inline float FormControlWidth(float max_width = 220.0f) {
  const float avail = ImGui::GetContentRegionAvail().x;
  const float w = avail < max_width ? avail : max_width;
  return w < 1.0f ? 1.0f : w;
}

// Vertical gap under a control group, sized to the theme's item spacing.
inline void SectionGap() {
  ImGui::Dummy(ImVec2(0.0f, ImGui::GetStyle().ItemSpacing.y));
}

// Centered title for a window whose system title bar is transparent. The
// system's close/minimise/zoom buttons draw over the left of it, so it has to
// stay tall enough to hold them. Returns true while held, for the caller to
// drag its own window.
inline bool WindowTitleBar(const char *title) {
  // Window space, not content space. The content origin is inset by
  // WindowPadding, so a bar drawn from there stops short of the edges.
  const ImVec2 window_pos = ImGui::GetWindowPos();
  const float window_width = ImGui::GetWindowSize().x;
  const ImVec2 padding = ImGui::GetStyle().WindowPadding;
  const float height = ImMax(28.0f, ImGui::GetFrameHeight() + 8.0f);

  ImDrawList *dl = ImGui::GetWindowDrawList();
  const ImVec2 bottom_right(window_pos.x + window_width, window_pos.y + height);
  dl->AddRectFilled(window_pos, bottom_right,
                    ImGui::GetColorU32(theme::palette::surface));
  dl->AddLine(ImVec2(window_pos.x, bottom_right.y), bottom_right,
              ImGui::GetColorU32(theme::palette::border));

  const ImVec2 text_size = ImGui::CalcTextSize(title);
  dl->AddText(ImVec2(window_pos.x + (window_width - text_size.x) * 0.5f,
                     window_pos.y + (height - text_size.y) * 0.5f),
              ImGui::GetColorU32(theme::palette::text), title);

  // Hit target for dragging. The system buttons live above this view, so they
  // take their own clicks first.
  ImGui::SetCursorScreenPos(window_pos);
  ImGui::InvisibleButton("##titlebar_drag", ImVec2(window_width, height));
  const bool held = ImGui::IsItemActive();

  // Resume the body below the bar, with the window's normal padding.
  ImGui::SetCursorScreenPos(
      ImVec2(window_pos.x + padding.x, window_pos.y + height + padding.y));
  return held;
}

// Centered, brand-styled modal. OpenPopup(id) to show, wrap the body in
// BeginModal/EndModal, close via ModalActions.
// fill_host is for when the modal IS the window (a launcher dialog): anchored
// top-left, no dim backdrop, height still tracking the content so the caller
// can size its OS window to match.
// `dismissable` adds click-outside and Escape to close, neither of which ImGui
// gives modals on its own. Ignored when fill_host, since that modal IS the
// window and closing it would leave an empty app.
inline bool BeginModal(const char *id, float width = 360.0f,
                       bool fill_host = false, bool dismissable = true) {
  ImGuiViewport *vp = ImGui::GetMainViewport();
  if (fill_host) {
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(
        ImVec2(width > 0.0f ? width : vp->WorkSize.x, 0.0f), ImGuiCond_Always);
  } else {
    ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Appearing,
                            ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(width, 0.0f), ImGuiCond_Always);
  }
  // Rounded bordered card, centered title. Title uses a SOLID header-blue (0.45
  // over bg), not a translucent one: ImGui fills the title rect with only that
  // color, so a translucent title would show the dimmed backdrop through it.
  const ImVec4 title_col = theme::over(
      theme::with_alpha(theme::palette::blue, 0.45f), theme::palette::bg);
  // The OS window rounds its own corners, so a rounded ImGui window would only
  // show background through them.
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding,
                      fill_host ? 0.0f : theme::metrics::rounding);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize,
                      theme::metrics::border_size);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowTitleAlign, ImVec2(0.5f, 0.5f));
  ImGui::PushStyleColor(ImGuiCol_PopupBg, theme::palette::bg);
  ImGui::PushStyleColor(ImGuiCol_TitleBg, title_col);
  ImGui::PushStyleColor(ImGuiCol_TitleBgActive, title_col);
  ImGui::PushStyleColor(
      ImGuiCol_ModalWindowDimBg,
      fill_host ? ImVec4(0.0f, 0.0f, 0.0f, 0.0f)
                : ImGui::GetStyleColorVec4(ImGuiCol_ModalWindowDimBg));
  ImGuiWindowFlags flags =
      ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove;
  // The host's own title bar is the dialog's, so this would be a second one.
  // No scrollbar, since it steals content width, which re-wraps the text and
  // changes the height the host is fitting itself to. That loop never settles.
  if (fill_host) {
    flags |= ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoTitleBar |
             ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
  }
  bool open = ImGui::BeginPopupModal(id, nullptr, flags);
  if (!open) {
    ImGui::PopStyleColor(4);
    ImGui::PopStyleVar(3);
    return false;
  }
  if (dismissable && !fill_host) {
    // AnyWindow, so a combo or nested popup opened from the modal does not
    // count as clicking the backdrop.
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
        !ImGui::IsWindowHovered(ImGuiHoveredFlags_AnyWindow))
      ImGui::CloseCurrentPopup();
    // Escape deactivates an edited field first, so only close once nothing is.
    if (ImGui::IsKeyPressed(ImGuiKey_Escape) && !ImGui::IsAnyItemActive())
      ImGui::CloseCurrentPopup();
  }
  return true;
}

inline void EndModal() {
  ImGui::EndPopup();
  ImGui::PopStyleColor(4);
  ImGui::PopStyleVar(3);
}

// Right-aligned footer for a modal. Returns 1 if the primary button was
// pressed, 2 if the secondary, otherwise 0. The primary is disabled when
// !primary_enabled, and drawn in the danger colour when primary_danger.
inline int ModalActions(const char *primary, const char *secondary = nullptr,
                        bool primary_enabled = true,
                        bool primary_danger = false) {
  ImGui::Spacing();
  ImGui::Separator();
  ImGui::Spacing();
  const float bw = 110.0f;
  const float spacing = ImGui::GetStyle().ItemSpacing.x;
  const float total = bw + (secondary ? bw + spacing : 0.0f);
  const float avail = ImGui::GetContentRegionAvail().x;
  if (avail > total)
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (avail - total));
  int result = 0;
  if (secondary) {
    if (ImGui::Button(secondary, ImVec2(bw, 0.0f)))
      result = 2;
    ImGui::SameLine();
  }
  ImGui::BeginDisabled(!primary_enabled);
  if (primary_danger) {
    ImGui::PushStyleColor(ImGuiCol_Button,
                          theme::with_alpha(theme::palette::danger, 0.75f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, theme::palette::danger);
  }
  if (ImGui::Button(primary, ImVec2(bw, 0.0f)))
    result = 1;
  if (primary_danger)
    ImGui::PopStyleColor(2);
  ImGui::EndDisabled();
  return result;
}

// Segmented control: one bar, shared dividers, outer corners rounded.
// Horizontal if it fits, else stacked. Returns the clicked index, or -1.
inline int ButtonGroupSelector(const std::vector<std::string> &options,
                               int current_selection,
                               const ImVec2 &button_size = ImVec2(0, 0)) {
  const int n = static_cast<int>(options.size());
  if (n == 0)
    return -1;

  ImDrawList *dl = ImGui::GetWindowDrawList();
  const float rounding = ImGui::GetStyle().FrameRounding;
  const float pad_x = 14.0f;
  const float h =
      button_size.y > 0.0f ? button_size.y : ImGui::GetFontSize() + 10.0f;

  // ImGui label convention: the text before "##" is shown, the rest is ID only.
  // Callers append a "##uniqueid" suffix so each selector's IDs stay unique.
  std::vector<std::string> disp(n);
  std::vector<float> seg_w(n);
  float total = 0.0f, widest = 0.0f;
  for (int i = 0; i < n; ++i) {
    disp[i] = options[i].substr(0, options[i].find("##"));
    seg_w[i] = ImGui::CalcTextSize(disp[i].c_str()).x + pad_x * 2.0f;
    total += seg_w[i];
    widest = std::max(widest, seg_w[i]);
  }
  const float avail = ImGui::GetContentRegionAvail().x;
  const bool horizontal = (n == 1) || (total <= avail);
  const float vwidth = std::max(widest, avail); // full-width when stacked

  const ImU32 c_border = ImGui::GetColorU32(theme::palette::border);
  const ImU32 c_text = ImGui::GetColorU32(theme::palette::text);
  auto fill_of = [](bool sel, bool hov, bool act) -> ImU32 {
    if (sel)
      return ImGui::GetColorU32(
          act   ? theme::palette::blue
          : hov ? theme::with_alpha(theme::palette::blue, 0.85f)
                : theme::with_alpha(theme::palette::blue, 0.60f));
    return ImGui::GetColorU32(
        act   ? theme::palette::border
        : hov ? theme::with_alpha(theme::palette::border, 0.90f)
              : theme::palette::surface);
  };

  int clicked = -1;
  std::vector<ImVec2> smin(n), smax(n);

  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
  for (int i = 0; i < n; ++i) {
    if (horizontal && i > 0)
      ImGui::SameLine(0.0f, 0.0f);
    // Full string (incl. any "##id" suffix) is the unique widget ID.
    if (ImGui::InvisibleButton(options[i].c_str(),
                               ImVec2(horizontal ? seg_w[i] : vwidth, h)))
      clicked = i;
    const bool hov = ImGui::IsItemHovered();
    const bool act = ImGui::IsItemActive();
    smin[i] = ImGui::GetItemRectMin();
    smax[i] = ImGui::GetItemRectMax();

    ImDrawFlags fl;
    if (n == 1)
      fl = ImDrawFlags_RoundCornersAll;
    else if (horizontal)
      fl = i == 0 ? ImDrawFlags_RoundCornersLeft
                  : (i == n - 1 ? ImDrawFlags_RoundCornersRight
                                : ImDrawFlags_RoundCornersNone);
    else
      fl = i == 0 ? ImDrawFlags_RoundCornersTop
                  : (i == n - 1 ? ImDrawFlags_RoundCornersBottom
                                : ImDrawFlags_RoundCornersNone);

    dl->AddRectFilled(smin[i], smax[i],
                      fill_of(i == current_selection, hov, act), rounding, fl);
    const ImVec2 ts = ImGui::CalcTextSize(disp[i].c_str());
    dl->AddText(ImVec2(smin[i].x + (smax[i].x - smin[i].x - ts.x) * 0.5f,
                       smin[i].y + (smax[i].y - smin[i].y - ts.y) * 0.5f),
                c_text, disp[i].c_str());
  }
  ImGui::PopStyleVar();

  // One rounded outer border, plus flat shared dividers between segments.
  dl->AddRect(smin[0], smax[n - 1], c_border, rounding,
              ImDrawFlags_RoundCornersAll, 1.0f);
  for (int i = 1; i < n; ++i) {
    if (horizontal)
      dl->AddLine(ImVec2(smin[i].x, smin[0].y), ImVec2(smin[i].x, smax[0].y),
                  c_border, 1.0f);
    else
      dl->AddLine(ImVec2(smin[0].x, smin[i].y), ImVec2(smax[0].x, smin[i].y),
                  c_border, 1.0f);
  }
  return clicked;
}

// Two-option overload driven by a bool reference. Returns true if the value
// changed.
inline bool ButtonGroupSelector(const std::string &option1,
                                const std::string &option2,
                                bool &is_option2_selected,
                                const ImVec2 &button_size = ImVec2(0, 0)) {
  int current_selection = is_option2_selected ? 1 : 0;
  int result =
      ButtonGroupSelector({option1, option2}, current_selection, button_size);

  if (result >= 0) {
    is_option2_selected = (result == 1);
    return true;
  }

  return false;
}

namespace detail {
struct OverlaySaved {
  ImVec2 cursor, cursor_max, ideal_max;
};
// ImGui is single-threaded per context, so a plain stack is fine here.
inline std::vector<OverlaySaved> &overlay_saved_stack() {
  static std::vector<OverlaySaved> s;
  return s;
}
} // namespace detail

// Floating card over a canvas, anchored top-left at `screen_pos` (use
// theme::metrics::overlay_edge as the gap from the canvas edge).
// EndOverlayCard restores the cursor and content extents, so the card does
// not disturb the canvas flow or grow the parent window.
inline void
BeginOverlayCard(const char *id, ImVec2 screen_pos, ImVec2 size = ImVec2(0, 0),
                 ImGuiChildFlags extra_flags = ImGuiChildFlags_AutoResizeX |
                                               ImGuiChildFlags_AutoResizeY) {
  ImGuiWindow *win = ImGui::GetCurrentWindow();
  detail::overlay_saved_stack().push_back(
      {win->DC.CursorPos, win->DC.CursorMaxPos, win->DC.IdealMaxPos});
  win->DC.CursorPos = screen_pos;
  ImGui::PushStyleColor(ImGuiCol_ChildBg,
                        theme::with_alpha(theme::palette::surface, 0.92f));
  ImGui::PushStyleColor(ImGuiCol_Border, theme::palette::border);
  ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, theme::metrics::rounding);
  ImGui::PushStyleVar(
      ImGuiStyleVar_WindowPadding,
      ImVec2(theme::metrics::overlay_pad_x, theme::metrics::overlay_pad_y));
  ImGui::BeginChild(id, size, extra_flags | ImGuiChildFlags_Border,
                    ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav |
                        ImGuiWindowFlags_NoScrollbar |
                        ImGuiWindowFlags_NoScrollWithMouse);
}

inline void EndOverlayCard() {
  ImGui::EndChild();
  ImGui::PopStyleVar(2);
  ImGui::PopStyleColor(2);
  ImGuiWindow *win = ImGui::GetCurrentWindow();
  const detail::OverlaySaved &saved = detail::overlay_saved_stack().back();
  win->DC.CursorPos = saved.cursor;
  win->DC.CursorMaxPos = saved.cursor_max;
  win->DC.IdealMaxPos = saved.ideal_max;
  detail::overlay_saved_stack().pop_back();
}

// Auto-refresh state for a data panel. tick() advances by io.DeltaTime and
// fires when the interval elapses.
struct RefreshTimer {
  float interval = 5.0f;
  float elapsed = 0.0f;
  bool auto_refresh = true;

  bool tick() {
    if (!auto_refresh)
      return false;
    elapsed += ImGui::GetIO().DeltaTime;
    if (elapsed < interval)
      return false;
    elapsed = 0.0f;
    return true;
  }
};

// Refresh button + auto-refresh checkbox + countdown. Returns true when the
// caller should reload (button pressed or interval elapsed).
inline bool RefreshControls(RefreshTimer &timer,
                            const char *label = "Refresh") {
  bool fire = timer.tick();
  if (ImGui::Button(label)) {
    fire = true;
    timer.elapsed = 0.0f;
  }
  ImGui::SameLine();
  ImGui::Checkbox("Auto-refresh", &timer.auto_refresh);
  if (timer.auto_refresh) {
    ImGui::SameLine();
    ImGui::Text("(%.1fs)", timer.interval - timer.elapsed);
  }
  return fire;
}

} // namespace ImGuiWidgets
