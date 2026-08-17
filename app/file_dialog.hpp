// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The ImRmfMapEditor Authors

#pragma once

#include <string>

namespace imrmf::map_editor {

enum class FileKind {
  BuildingYaml, // *.building.yaml, with its images alongside it
  MapBundle,    // *.rmfmap, the whole map in one file
};

// False where no OS picker is wired up, and the in-app browser is used.
bool has_native_file_picker();

// Opens the OS file picker. Empty string if cancelled or unsupported.
std::string pick_open_path(FileKind kind);

// Same, for naming a file to write. Empty if cancelled or unsupported.
std::string pick_save_path(FileKind kind, const std::string &default_name);

} // namespace imrmf::map_editor
