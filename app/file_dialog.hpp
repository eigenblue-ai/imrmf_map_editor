// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The ImRmfMapEditor Authors

#pragma once

#include <string>

namespace imrmf::map_editor {

// False where no OS picker is wired up, and the in-app browser is used.
bool has_native_file_picker();

// Opens the OS file picker. Empty string if cancelled or unsupported.
std::string pick_building_yaml();

// Same, but for naming a new file. Empty string if cancelled or unsupported.
std::string pick_new_building_yaml();

} // namespace imrmf::map_editor
