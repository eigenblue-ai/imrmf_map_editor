#pragma once

#include "imgui/imgui.h"
#include "ui/IconsMaterialDesignIcons.h"
#include "ui/theme.hpp"

#include <algorithm>
#include <cstddef>
#include <deque>
#include <string>
#include <utility>

// Transient messages, stacked in the bottom right. Anything can post one from
// anywhere, so an outcome no longer has to be reported next to whatever
// produced it. Post from the frame thread, the queue takes no lock. Needs
// CHECK_CIRCLE, ALERT, CLOSE_CIRCLE and INFORMATION in the atlas.
namespace ImGuiWidgets {

struct Notification {
  theme::Signal signal = theme::Signal::info;
  std::string text;
  int count = 1; // repeats collapsed onto one card
  // ImGui time it first appeared. 0 until then, so one posted before the first
  // frame still gets its full lifetime.
  double posted = 0.0;
};

namespace detail {

inline std::deque<Notification> &notifications() {
  static std::deque<Notification> queue;
  return queue;
}

constexpr float kNotifyWidth = 340.0f;
constexpr float kNotifyMargin = 14.0f;
constexpr float kNotifyGap = 8.0f;
constexpr float kNotifyPad = 10.0f;
constexpr float kNotifyStripe = 4.0f; // severity bar down the left edge
constexpr std::size_t kNotifyMax = 5;

inline const char *notification_icon(theme::Signal s) {
  switch (s) {
  case theme::Signal::success:
    return ICON_MDI_CHECK_CIRCLE;
  case theme::Signal::warning:
    return ICON_MDI_ALERT;
  case theme::Signal::danger:
    return ICON_MDI_CLOSE_CIRCLE;
  default:
    return ICON_MDI_INFORMATION;
  }
}

// Errors linger, they are the ones worth reading.
inline double notification_seconds(theme::Signal s) {
  return s == theme::Signal::danger ? 12.0 : 6.0;
}

inline std::string notification_label(const Notification &n) {
  return n.count > 1 ? n.text + "  (x" + std::to_string(n.count) + ")" : n.text;
}

inline float notification_width() {
  const ImGuiViewport *vp = ImGui::GetMainViewport();
  const float fits = vp->WorkSize.x - kNotifyMargin * 2.0f;
  return std::max(120.0f, std::min(kNotifyWidth, fits));
}

// [stripe][pad][icon][pad][text][pad]
inline float notification_wrap(const Notification &n, float card_w) {
  const float icon_w = ImGui::CalcTextSize(notification_icon(n.signal)).x;
  return card_w - kNotifyStripe - kNotifyPad * 3.0f - icon_w;
}

inline float notification_height(const Notification &n, float card_w) {
  const std::string label = notification_label(n);
  const float text_h = ImGui::CalcTextSize(label.c_str(), nullptr, false,
                                           notification_wrap(n, card_w))
                           .y;
  return std::max(text_h, ImGui::GetTextLineHeight()) + kNotifyPad * 2.0f;
}

// A server payload can be a whole HTML error page. A card is not a document.
inline std::string one_line(const std::string &text) {
  constexpr std::size_t kMaxChars = 240;
  std::string out;
  out.reserve(std::min(text.size(), kMaxChars + 1));
  bool space = false;
  for (char c : text) {
    const bool ws = c == ' ' || c == '\t' || c == '\n' || c == '\r';
    if (ws) {
      space = !out.empty();
      continue;
    }
    if (space) {
      out += ' ';
      space = false;
    }
    if (out.size() >= kMaxChars) {
      out += "\xE2\x80\xA6"; // utf-8 ellipsis
      break;
    }
    out += c;
  }
  return out;
}

} // namespace detail

inline void Notify(theme::Signal signal, std::string text) {
  text = detail::one_line(text);
  if (text.empty())
    return;
  auto &queue = detail::notifications();
  // A repeat bumps the counter, or a per-frame failure buries the rest.
  for (Notification &n : queue) {
    if (n.signal == signal && n.text == text) {
      ++n.count;
      n.posted = 0.0;
      return;
    }
  }
  queue.push_back({signal, std::move(text), 1, 0.0});
  // Bounded: nobody reads the bottom of a tall stack.
  while (queue.size() > detail::kNotifyMax)
    queue.pop_front();
}

// Space the stack needs, so a self-sizing host can leave room for it.
inline float NotificationsHeight() {
  const auto &queue = detail::notifications();
  if (queue.empty())
    return 0.0f;
  const float card_w = detail::notification_width();
  float total = detail::kNotifyMargin;
  for (const Notification &n : queue)
    total += detail::notification_height(n, card_w) + detail::kNotifyGap;
  return total;
}

// A surface that treats a click as an edit (the canvas) asks first, so
// dismissing a card cannot also drop a vertex under it.
inline bool NotificationsHovered() {
  const auto &queue = detail::notifications();
  if (queue.empty())
    return false;
  const ImGuiViewport *vp = ImGui::GetMainViewport();
  const float card_w = detail::notification_width();
  const float right = vp->WorkPos.x + vp->WorkSize.x - detail::kNotifyMargin;
  const float bottom = vp->WorkPos.y + vp->WorkSize.y - detail::kNotifyMargin;
  float top = bottom;
  for (const Notification &n : queue)
    top -= detail::notification_height(n, card_w) + detail::kNotifyGap;
  const ImVec2 m = ImGui::GetIO().MousePos;
  return m.x >= right - card_w && m.x <= right && m.y >= top && m.y <= bottom;
}

// Call once per frame. On the foreground list, since a plain window would end
// up behind the modals.
inline void DrawNotifications() {
  auto &queue = detail::notifications();
  const double now = ImGui::GetTime();
  // Every card, not just the oldest: lifetimes differ and hovering holds one,
  // so they do not expire in order.
  for (auto it = queue.begin(); it != queue.end();) {
    if (it->posted > 0.0 &&
        now - it->posted > detail::notification_seconds(it->signal))
      it = queue.erase(it);
    else
      ++it;
  }
  if (queue.empty())
    return;

  const ImGuiViewport *vp = ImGui::GetMainViewport();
  const float card_w = detail::notification_width();
  const float right = vp->WorkPos.x + vp->WorkSize.x - detail::kNotifyMargin;
  float bottom = vp->WorkPos.y + vp->WorkSize.y - detail::kNotifyMargin;

  ImDrawList *dl = ImGui::GetForegroundDrawList();
  ImFont *font = ImGui::GetFont();
  const float font_size = ImGui::GetFontSize();
  const ImVec2 mouse = ImGui::GetIO().MousePos;
  const bool clicked = ImGui::IsMouseClicked(ImGuiMouseButton_Left);
  const ImU32 bg = ImGui::GetColorU32(theme::palette::surface);
  const ImU32 border = ImGui::GetColorU32(theme::palette::border);
  const ImU32 text_col = ImGui::GetColorU32(theme::palette::text);

  int index = (int)queue.size();
  int dismissed = -1;
  // Newest nearest the corner.
  for (auto it = queue.rbegin(); it != queue.rend(); ++it) {
    Notification &n = *it;
    --index;
    if (n.posted <= 0.0)
      n.posted = now;

    const float h = detail::notification_height(n, card_w);
    const ImVec2 tl(right - card_w, bottom - h);
    const ImVec2 br(right, bottom);
    const ImU32 accent = ImGui::GetColorU32(theme::signal_color(n.signal));
    const char *icon = detail::notification_icon(n.signal);
    const std::string label = detail::notification_label(n);

    dl->AddRectFilled(tl, br, bg, theme::metrics::rounding);
    dl->AddRect(tl, br, border, theme::metrics::rounding);
    dl->AddRectFilled(tl, ImVec2(tl.x + detail::kNotifyStripe, br.y), accent,
                      theme::metrics::rounding, ImDrawFlags_RoundCornersLeft);

    const ImVec2 icon_pos(tl.x + detail::kNotifyStripe + detail::kNotifyPad,
                          tl.y + detail::kNotifyPad);
    dl->AddText(icon_pos, accent, icon);
    const ImVec2 text_pos(icon_pos.x + ImGui::CalcTextSize(icon).x +
                              detail::kNotifyPad,
                          tl.y + detail::kNotifyPad);
    dl->AddText(font, font_size, text_pos, text_col, label.c_str(), nullptr,
                detail::notification_wrap(n, card_w));

    if (mouse.x >= tl.x && mouse.x <= br.x && mouse.y >= tl.y &&
        mouse.y <= br.y) {
      n.posted = now; // held while it is being read
      if (clicked)
        dismissed = index;
    }
    bottom = tl.y - detail::kNotifyGap;
  }

  if (dismissed >= 0)
    queue.erase(queue.begin() + dismissed);
}

} // namespace ImGuiWidgets
