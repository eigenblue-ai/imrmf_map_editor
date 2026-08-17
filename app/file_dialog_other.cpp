// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The ImRmfMapEditor Authors

#include "app/file_dialog.hpp"

namespace imrmf::map_editor {

// No picker outside macOS yet, the launcher shows its own browser instead.
bool has_native_file_picker() { return false; }

std::string pick_open_path(FileKind) { return {}; }

std::string pick_save_path(FileKind, const std::string &) { return {}; }

} // namespace imrmf::map_editor
