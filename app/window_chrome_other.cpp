// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The ImRmfMapEditor Authors

#include "app/window_chrome.hpp"

namespace imrmf::map_editor {

// Not wired up outside macOS yet, so the system title bar stays.
bool has_custom_titlebar() { return false; }

void use_custom_titlebar(GLFWwindow *) {}

bool global_cursor_position(double *, double *) { return false; }

} // namespace imrmf::map_editor
