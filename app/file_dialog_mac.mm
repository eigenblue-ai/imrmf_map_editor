// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The ImRmfMapEditor Authors

#include "app/file_dialog.hpp"

#import <AppKit/AppKit.h>

namespace imrmf::map_editor {

bool has_native_file_picker() { return true; }

std::string pick_building_yaml() {
  @autoreleasepool {
    NSOpenPanel *panel = [NSOpenPanel openPanel];
    panel.canChooseFiles = YES;
    panel.canChooseDirectories = NO;
    panel.allowsMultipleSelection = NO;
    panel.message = @"Open a building.yaml";
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    panel.allowedFileTypes = @[ @"yaml" ];
#pragma clang diagnostic pop

    // GLFW leaves the window in front, so pull the panel above it.
    [NSApp activateIgnoringOtherApps:YES];
    if ([panel runModal] != NSModalResponseOK)
      return {};
    NSURL *url = panel.URLs.firstObject;
    return url ? std::string(url.fileSystemRepresentation) : std::string();
  }
}

std::string pick_new_building_yaml() {
  @autoreleasepool {
    NSSavePanel *panel = [NSSavePanel savePanel];
    panel.message = @"Name the new building";
    panel.nameFieldStringValue = @"untitled.building.yaml";
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    panel.allowedFileTypes = @[ @"yaml" ];
#pragma clang diagnostic pop

    [NSApp activateIgnoringOtherApps:YES];
    if ([panel runModal] != NSModalResponseOK)
      return {};
    NSURL *url = panel.URL;
    return url ? std::string(url.fileSystemRepresentation) : std::string();
  }
}

} // namespace imrmf::map_editor
