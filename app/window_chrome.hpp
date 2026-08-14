// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The ImRmfMapEditor Authors

#pragma once

struct GLFWwindow;

namespace imrmf::map_editor {

// Hides the system title bar but keeps the window, so the shadow and rounded
// corners survive. A borderless window loses both.
bool has_custom_titlebar();
void use_custom_titlebar(GLFWwindow *window);

// Cursor in screen coordinates. GLFW only reports it per window, which is no
// use for dragging that window.
bool global_cursor_position(double *x, double *y);

} // namespace imrmf::map_editor
