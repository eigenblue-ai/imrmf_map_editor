#include "ui/schema_ui.hpp"
#include "imgui/imgui.h"
#include "ui/widgets.hpp"
#include <algorithm>
#include <vector>

// Property entry, ordered by the schema's x-ui-order.
struct PropertyEntry {
  std::string name;
  int order;
  const nlohmann::json *schema;
};

static std::vector<PropertyEntry>
get_sorted_properties(const nlohmann::json &schema) {
  std::vector<PropertyEntry> entries;

  if (!schema.contains("properties"))
    return entries;

  const auto &props = schema["properties"];
  for (auto it = props.begin(); it != props.end(); ++it) {
    PropertyEntry entry;
    entry.name = it.key();
    entry.order = it.value().value("x-ui-order", 9999);
    entry.schema = &it.value();
    entries.push_back(entry);
  }

  std::sort(entries.begin(), entries.end(),
            [](const PropertyEntry &a, const PropertyEntry &b) {
              return a.order < b.order;
            });

  return entries;
}

static bool render_slider_int(const std::string &label, const std::string &id,
                              nlohmann::json &values,
                              const std::string &prop_name,
                              const nlohmann::json &prop_schema) {
  int val = values.value(prop_name, prop_schema.value("default", 0));
  int min_val = prop_schema.value("minimum", 0);
  int max_val = prop_schema.value("maximum", 100);
  std::string format = prop_schema.value("x-ui-format", std::string("%d"));

  ImGui::SetNextItemWidth(ImGuiWidgets::FormControlWidth());
  if (ImGui::SliderInt(id.c_str(), &val, min_val, max_val, format.c_str())) {
    values[prop_name] = val;
    return true;
  }
  return false;
}

static bool render_slider_float(const std::string &label, const std::string &id,
                                nlohmann::json &values,
                                const std::string &prop_name,
                                const nlohmann::json &prop_schema) {
  float val = values.value(prop_name, prop_schema.value("default", 0.0f));
  float min_val = prop_schema.value("minimum", 0.0f);
  float max_val = prop_schema.value("maximum", 100.0f);
  std::string format = prop_schema.value("x-ui-format", std::string("%.1f"));

  ImGui::SetNextItemWidth(ImGuiWidgets::FormControlWidth());
  if (ImGui::SliderFloat(id.c_str(), &val, min_val, max_val, format.c_str())) {
    values[prop_name] = val;
    return true;
  }
  return false;
}

static bool render_button_group(const std::string &label, const std::string &id,
                                nlohmann::json &values,
                                const std::string &prop_name,
                                const nlohmann::json &prop_schema) {
  if (!prop_schema.contains("enum"))
    return false;

  std::string current_val =
      values.value(prop_name, prop_schema.value("default", std::string("")));

  const auto &enum_vals = prop_schema["enum"];

  // Labels come from x-ui-labels, falling back to the enum values.
  std::vector<std::string> labels;
  if (prop_schema.contains("x-ui-labels")) {
    for (const auto &lbl : prop_schema["x-ui-labels"]) {
      labels.push_back(lbl.get<std::string>());
    }
  } else {
    for (const auto &ev : enum_vals) {
      labels.push_back(ev.get<std::string>());
    }
  }

  // ImGui needs unique widget IDs, so suffix each label with ##id_i.
  std::vector<std::string> labeled;
  for (size_t i = 0; i < labels.size(); ++i) {
    labeled.push_back(labels[i] + "##" + id + "_" + std::to_string(i));
  }

  int current_idx = 0;
  for (size_t i = 0; i < enum_vals.size(); ++i) {
    if (enum_vals[i].get<std::string>() == current_val) {
      current_idx = static_cast<int>(i);
      break;
    }
  }

  int result = ImGuiWidgets::ButtonGroupSelector(labeled, current_idx);

  if (result >= 0 && result < static_cast<int>(enum_vals.size())) {
    values[prop_name] = enum_vals[result].get<std::string>();
    return true;
  }
  return false;
}

static bool render_combo(const std::string &label, const std::string &id,
                         nlohmann::json &values, const std::string &prop_name,
                         const nlohmann::json &prop_schema) {
  if (!prop_schema.contains("enum"))
    return false;

  std::string current_val =
      values.value(prop_name, prop_schema.value("default", std::string("")));

  const auto &enum_vals = prop_schema["enum"];

  // ImGui::Combo wants the items as one null-separated string.
  std::string items;
  int current_idx = 0;
  for (size_t i = 0; i < enum_vals.size(); ++i) {
    std::string ev = enum_vals[i].get<std::string>();
    if (ev == current_val)
      current_idx = static_cast<int>(i);
    items += ev;
    items += '\0';
  }
  items += '\0';

  ImGui::SetNextItemWidth(ImGuiWidgets::FormControlWidth());
  if (ImGui::Combo(id.c_str(), &current_idx, items.c_str())) {
    if (current_idx >= 0 && current_idx < static_cast<int>(enum_vals.size())) {
      values[prop_name] = enum_vals[current_idx].get<std::string>();
      return true;
    }
  }
  return false;
}

static bool render_checkbox(const std::string &label, const std::string &id,
                            nlohmann::json &values,
                            const std::string &prop_name,
                            const nlohmann::json &prop_schema) {
  bool val = values.value(prop_name, prop_schema.value("default", false));

  // Label lives in the form's left column; the box itself carries an id only.
  if (ImGui::Checkbox(id.c_str(), &val)) {
    values[prop_name] = val;
    return true;
  }
  return false;
}

bool render_schema_ui(const nlohmann::json &schema, nlohmann::json &values,
                      const std::string &id_prefix) {

  bool any_changed = false;

  auto entries = get_sorted_properties(schema);
  if (entries.empty())
    return false;

  // One 2-column form table: labels left, controls right, both scale with the
  // panel.
  if (!ImGuiWidgets::BeginFormTable(("##form" + id_prefix).c_str()))
    return false;

  for (const auto &entry : entries) {
    const auto &prop_schema = *entry.schema;
    std::string label = prop_schema.value("description", entry.name);
    std::string widget = prop_schema.value("x-ui-widget", std::string(""));
    std::string type = prop_schema.value("type", std::string(""));
    std::string id = "##" + id_prefix + entry.name;

    ImGuiWidgets::FormRow(label.c_str());

    if (widget == "slider") {
      if (type == "integer") {
        any_changed |=
            render_slider_int(label, id, values, entry.name, prop_schema);
      } else if (type == "number") {
        any_changed |=
            render_slider_float(label, id, values, entry.name, prop_schema);
      }
    } else if (widget == "button_group") {
      any_changed |=
          render_button_group(label, id, values, entry.name, prop_schema);
    } else if (widget == "combo") {
      any_changed |= render_combo(label, id, values, entry.name, prop_schema);
    } else if (widget == "checkbox") {
      any_changed |=
          render_checkbox(label, id, values, entry.name, prop_schema);
    }
  }

  ImGui::EndTable();
  return any_changed;
}
