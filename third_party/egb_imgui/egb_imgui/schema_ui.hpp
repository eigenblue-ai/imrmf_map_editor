#pragma once

#include <nlohmann/json.hpp>
#include <string>

// Render ImGui controls dynamically from a JSON Schema + values object.
// Returns true if any value changed this frame.
//
// Widget mapping (via x-ui-widget):
//   "slider"       + integer   -> ImGui::SliderInt
//   "slider"       + number    -> ImGui::SliderFloat
//   "button_group" + string    -> ImGuiWidgets::ButtonGroupSelector
//   "combo"        + string    -> ImGui::Combo
//   "checkbox"     + boolean   -> ImGui::Checkbox
//
// Properties are sorted by x-ui-order. Description is used as label.
bool render_schema_ui(const nlohmann::json &schema, nlohmann::json &values,
                      const std::string &id_prefix = "");
