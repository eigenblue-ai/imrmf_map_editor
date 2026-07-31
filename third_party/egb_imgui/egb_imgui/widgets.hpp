#pragma once
#include "egb_imgui/theme.hpp"
#include "imgui/imgui.h"
#include <algorithm>
#include <string>
#include <vector>

namespace ImGuiWidgets {

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

// Centered, brand-styled modal. OpenPopup(id) to show, wrap the body in
// BeginModal/EndModal, close via ModalActions.
inline bool BeginModal(const char *id, float width = 360.0f) {
  ImGuiViewport *vp = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Appearing,
                          ImVec2(0.5f, 0.5f));
  ImGui::SetNextWindowSize(ImVec2(width, 0.0f), ImGuiCond_Always);
  // Rounded bordered card, centered title. Title uses a SOLID header-blue (0.45
  // over bg), not a translucent one: ImGui fills the title rect with only that
  // color, so a translucent title would show the dimmed backdrop through it.
  const ImVec4 title_col = theme::over(
      theme::with_alpha(theme::palette::blue, 0.45f), theme::palette::bg);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, theme::metrics::rounding);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize,
                      theme::metrics::border_size);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowTitleAlign, ImVec2(0.5f, 0.5f));
  ImGui::PushStyleColor(ImGuiCol_PopupBg, theme::palette::bg);
  ImGui::PushStyleColor(ImGuiCol_TitleBg, title_col);
  ImGui::PushStyleColor(ImGuiCol_TitleBgActive, title_col);
  bool open = ImGui::BeginPopupModal(
      id, nullptr, ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove);
  if (!open) {
    ImGui::PopStyleColor(3);
    ImGui::PopStyleVar(3);
  }
  return open;
}

inline void EndModal() {
  ImGui::EndPopup();
  ImGui::PopStyleColor(3);
  ImGui::PopStyleVar(3);
}

// Right-aligned footer for a modal. Returns 1 if the primary button was
// pressed, 2 if the secondary, otherwise 0. The primary is disabled when
// !primary_enabled.
inline int ModalActions(const char *primary, const char *secondary = nullptr,
                        bool primary_enabled = true) {
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
  if (ImGui::Button(primary, ImVec2(bw, 0.0f)))
    result = 1;
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

} // namespace ImGuiWidgets
