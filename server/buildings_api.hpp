// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The ImRmfMapEditor Authors

#pragma once

#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace imrmf::map_editor {

std::string
http_response(int status, const std::string &status_text,
              const std::vector<std::pair<std::string, std::string>> &headers,
              const std::string &body);

std::string header_value(const std::string &headers, const std::string &name);

// Reads from fd until accum has at least `target` bytes. False on disconnect.
bool read_until(int fd, std::string &accum, size_t target);

std::string handle_building_get(const std::filesystem::path &maps_root,
                                const std::string &id);
std::string handle_building_post(const std::filesystem::path &maps_root,
                                 const std::string &id,
                                 const std::string &if_match,
                                 const std::string &body);

bool is_buildings_request(const std::string &request);

std::string dispatch_buildings_request(int fd,
                                       const std::string &initial_request,
                                       const std::filesystem::path &maps_root);

} // namespace imrmf::map_editor
