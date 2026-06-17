// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The ImRmfMapEditor Authors

#pragma once

#include "view/editor_view.hpp"

#include <functional>
#include <string>
#include <vector>

namespace imrmf::map_editor {

struct ControlContext {
  Mode mode = Mode::Pan;
  bool layer_align = false;
};

// One mouse/modifier control. Shows in the canvas HUD while active() holds.
class CanvasControl {
public:
  CanvasControl(std::string chord, std::string desc,
                std::function<bool(const ControlContext &)> active)
      : chord_(std::move(chord)), desc_(std::move(desc)),
        active_(std::move(active)) {}

  bool active(const ControlContext &ctx) const { return active_(ctx); }
  const std::string &chord() const { return chord_; }
  const std::string &description() const { return desc_; }

private:
  std::string chord_;
  std::string desc_;
  std::function<bool(const ControlContext &)> active_;
};

class ControlRegistry {
public:
  void add(std::string chord, std::string desc,
           std::function<bool(const ControlContext &)> active);
  std::vector<const CanvasControl *> active(const ControlContext &ctx) const;

private:
  std::vector<CanvasControl> controls_;
};

// Process-wide registry, populated on first use.
const ControlRegistry &canvas_controls();

std::string control_context_title(const ControlContext &ctx);

} // namespace imrmf::map_editor
