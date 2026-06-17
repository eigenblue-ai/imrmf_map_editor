// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The ImRmfMapEditor Authors

#include "view/canvas_controls.hpp"

#include <algorithm>

namespace imrmf::map_editor {

void ControlRegistry::add(std::string chord, std::string desc,
                          std::function<bool(const ControlContext &)> active) {
  controls_.emplace_back(std::move(chord), std::move(desc), std::move(active));
}

std::vector<const CanvasControl *>
ControlRegistry::active(const ControlContext &ctx) const {
  std::vector<const CanvasControl *> out;
  for (const CanvasControl &c : controls_)
    if (c.active(ctx))
      out.push_back(&c);
  return out;
}

namespace {

std::function<bool(const ControlContext &)> in_modes(std::vector<Mode> modes) {
  return [modes = std::move(modes)](const ControlContext &c) {
    return !c.layer_align &&
           std::find(modes.begin(), modes.end(), c.mode) != modes.end();
  };
}
bool editing(const ControlContext &c) { return !c.layer_align; }
bool aligning(const ControlContext &c) { return c.layer_align; }

ControlRegistry build() {
  using M = Mode;
  ControlRegistry r;
  r.add("Click", "select item", in_modes({M::Pan}));
  r.add("Click", "place / select", in_modes({M::Vertex}));
  r.add("Click", "chain lanes", in_modes({M::Lane}));
  r.add("Click", "add / connect", in_modes({M::Wall, M::Door, M::Measurement}));
  r.add("Click", "add point", in_modes({M::Floor, M::Hole}));
  r.add("Shift+Click", "add / toggle", in_modes({M::Pan, M::Vertex}));
  r.add("Drag", "marquee / move", in_modes({M::Pan}));
  r.add("Drag", "move vertex", in_modes({M::Vertex}));
  r.add("Drag", "split crossed lanes", in_modes({M::Lane}));
  r.add("Shift+Drag", "snap H/V/45", in_modes({M::Pan, M::Vertex}));
  r.add("Shift", "snap new point",
        in_modes({M::Lane, M::Wall, M::Door, M::Measurement, M::Floor,
                  M::Hole}));
  r.add("1st pt / Enter", "close polygon", in_modes({M::Floor, M::Hole}));
  r.add("Esc", "break chain",
        in_modes({M::Lane, M::Wall, M::Door, M::Measurement}));
  r.add("Esc", "cancel polygon", in_modes({M::Floor, M::Hole}));
  r.add("", "goes on the selected floor", in_modes({M::Hole}));
  r.add("Del", "delete selection", in_modes({M::Pan, M::Vertex}));
  r.add("Drag", "move layer", aligning);
  r.add("Wheel", "scale layer", aligning);
  r.add("Ctrl", "fine step", aligning);
  r.add("Alt+drag", "pan / zoom view", aligning);
  r.add("Mid-drag", "pan", editing);
  r.add("Wheel", "zoom", editing);
  return r;
}

} // namespace

const ControlRegistry &canvas_controls() {
  static const ControlRegistry r = build();
  return r;
}

std::string control_context_title(const ControlContext &ctx) {
  if (ctx.layer_align)
    return "Align layer";
  switch (ctx.mode) {
  case Mode::Pan:
    return "Select";
  case Mode::Vertex:
    return "Vertex";
  case Mode::Lane:
    return "Lane";
  case Mode::Wall:
    return "Wall";
  case Mode::Door:
    return "Door";
  case Mode::Measurement:
    return "Measurement";
  case Mode::Floor:
    return "Floor";
  case Mode::Hole:
    return "Hole";
  }
  return "";
}

} // namespace imrmf::map_editor
