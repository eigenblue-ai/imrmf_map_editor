// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The ImRmfMapEditor Authors

#include "app/file_dialog.hpp"

#import <AppKit/AppKit.h>

namespace imrmf::map_editor {

namespace {

NSArray<NSString *> *extensions_for(FileKind kind) {
  return kind == FileKind::MapBundle ? @[ @"rmfmap" ] : @[ @"yaml" ];
}

NSString *message_for(FileKind kind, bool saving) {
  if (kind == FileKind::MapBundle)
    return saving ? @"Save the map as one file" : @"Open a map bundle";
  return saving ? @"Name the new building" : @"Open a building.yaml";
}

} // namespace

bool has_native_file_picker() { return true; }

std::string pick_open_path(FileKind kind) {
  @autoreleasepool {
    NSOpenPanel *panel = [NSOpenPanel openPanel];
    panel.canChooseFiles = YES;
    panel.canChooseDirectories = NO;
    panel.allowsMultipleSelection = NO;
    panel.message = message_for(kind, /*saving=*/false);
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    panel.allowedFileTypes = extensions_for(kind);
#pragma clang diagnostic pop

    // GLFW leaves the window in front, so pull the panel above it.
    [NSApp activateIgnoringOtherApps:YES];
    if ([panel runModal] != NSModalResponseOK)
      return {};
    NSURL *url = panel.URLs.firstObject;
    return url ? std::string(url.fileSystemRepresentation) : std::string();
  }
}

std::string pick_save_path(FileKind kind, const std::string &default_name) {
  @autoreleasepool {
    NSSavePanel *panel = [NSSavePanel savePanel];
    panel.message = message_for(kind, /*saving=*/true);
    panel.nameFieldStringValue =
        [NSString stringWithUTF8String:default_name.c_str()];
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    panel.allowedFileTypes = extensions_for(kind);
#pragma clang diagnostic pop

    [NSApp activateIgnoringOtherApps:YES];
    if ([panel runModal] != NSModalResponseOK)
      return {};
    NSURL *url = panel.URL;
    return url ? std::string(url.fileSystemRepresentation) : std::string();
  }
}

} // namespace imrmf::map_editor
