#pragma once

#include "imgui/imgui.h"

#include <fstream>
#include <string>
#include <vector>

namespace egb {

// A tool window managed by WindowManager. Implement draw() with the window
// body only, the manager owns Begin/End and the visibility flag.
class WindowInterface {
public:
  virtual ~WindowInterface() = default;
  virtual void draw() = 0;
  virtual const char *get_name() = 0;
  bool visible = true;
};

// Draws registered windows, renders the Window visibility menu, and persists
// visibility as name=0|1 lines. Windows are borrowed, not owned. Keep them
// alive for the manager's lifetime.
class WindowManager {
public:
  void add_window(WindowInterface *window) { windows_.push_back(window); }

  void draw_all() {
    for (WindowInterface *w : windows_) {
      if (!w->visible)
        continue;
      ImGui::Begin(w->get_name(), &w->visible);
      w->draw();
      ImGui::End();
    }
  }

  // MenuItem per window, for a "Window" menu.
  void draw_window_menu() {
    for (WindowInterface *w : windows_)
      ImGui::MenuItem(w->get_name(), nullptr, &w->visible);
  }

  void save_visibility_state(const std::string &filepath) {
    std::ofstream file(filepath);
    if (!file.is_open())
      return;
    for (WindowInterface *w : windows_)
      file << w->get_name() << "=" << (w->visible ? "1" : "0") << "\n";
  }

  void load_visibility_state(const std::string &filepath) {
    std::ifstream file(filepath);
    if (!file.is_open()) // no state saved yet
      return;
    std::string line;
    while (std::getline(file, line)) {
      const size_t pos = line.find('=');
      if (pos == std::string::npos)
        continue;
      const std::string name = line.substr(0, pos);
      const bool visible = line.substr(pos + 1) == "1";
      for (WindowInterface *w : windows_) {
        if (name == w->get_name()) {
          w->visible = visible;
          break;
        }
      }
    }
  }

private:
  std::vector<WindowInterface *> windows_;
};

} // namespace egb
