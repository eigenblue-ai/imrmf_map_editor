#pragma once

#include "imgui/imgui.h"

namespace ImGuiWidgets {

// Fullscreen undecorated host window with a DockSpace over the main viewport.
// Returns the dockspace id (0 if docking is disabled in io.ConfigFlags). Put
// menu content in a BeginMenuBar()/EndMenuBar() pair inside, then close with
// EndDockSpaceHost().
inline ImGuiID BeginDockSpaceHost(const char *title = "##dockspace_host",
                                  bool with_menu_bar = true) {
  const ImGuiViewport *vp = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(vp->WorkPos);
  ImGui::SetNextWindowSize(vp->WorkSize);
  ImGui::SetNextWindowViewport(vp->ID);
  ImGuiWindowFlags flags =
      ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoTitleBar |
      ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize |
      ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus |
      ImGuiWindowFlags_NoNavFocus;
  if (with_menu_bar)
    flags |= ImGuiWindowFlags_MenuBar;
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
  ImGui::Begin(title, nullptr, flags);
  ImGui::PopStyleVar(3);
  ImGuiID dockspace_id = 0;
  if (ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_DockingEnable) {
    dockspace_id = ImGui::GetID("##dockspace");
    ImGui::DockSpace(dockspace_id, ImVec2(0.0f, 0.0f), ImGuiDockNodeFlags_None);
  }
  return dockspace_id;
}

inline void EndDockSpaceHost() { ImGui::End(); }

} // namespace ImGuiWidgets
