// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The ImRmfMapEditor Authors

#include "app/window_chrome.hpp"

#define GLFW_EXPOSE_NATIVE_COCOA
#include "GLFW/glfw3.h"
#include "GLFW/glfw3native.h"

#import <AppKit/AppKit.h>

namespace imrmf::map_editor {

bool has_custom_titlebar() { return true; }

void use_custom_titlebar(GLFWwindow *window) {
  NSWindow *win = glfwGetCocoaWindow(window);
  if (!win)
    return;

  // Content runs under the title bar, and the title bar itself paints nothing.
  win.styleMask |= NSWindowStyleMaskFullSizeContentView;
  win.titlebarAppearsTransparent = YES;
  win.titleVisibility = NSWindowTitleHidden;

  // The window buttons stay the system's, only the bar around them is ours.
  // The GL view eats mouse events, so the app does the dragging.
}

bool global_cursor_position(double *x, double *y) {
  NSScreen *primary = [NSScreen screens].firstObject;
  if (!primary || !x || !y)
    return false;
  // AppKit measures from bottom-left, GLFW from top-left.
  const NSPoint p = [NSEvent mouseLocation];
  *x = p.x;
  *y = primary.frame.size.height - p.y;
  return true;
}

} // namespace imrmf::map_editor
