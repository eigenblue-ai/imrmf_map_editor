// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The ImRmfMapEditor Authors

// Loads a building, syncs, pushes an edit back. Needs a running server:
//   bazel run //client_rust:smoke_test -- http://localhost:30011 test

#include "client_rust/client.h"

#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

namespace {

std::string take(char *raw) {
  if (!raw)
    return "<null>";
  std::string s(raw);
  rmf_client_string_free(raw);
  return s;
}

bool wait_for_yaml(std::string *out, int attempts = 50) {
  for (int i = 0; i < attempts; ++i) {
    *out = take(rmf_client_snapshot_yaml());
    if (!out->empty())
      return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  return false;
}

} // namespace

int main(int argc, char **argv) {
  if (argc < 3) {
    std::fprintf(stderr, "usage: smoke_test <server_url> <building_id>\n");
    return 2;
  }
  const std::string server = argv[1];
  const std::string id = argv[2];

  std::printf(
      "load: %s\n",
      take(rmf_client_load_building(server.c_str(), id.c_str())).c_str());

  std::string ws = server;
  if (ws.rfind("http://", 0) == 0)
    ws = "ws://" + ws.substr(7);
  ws += "/ws/" + id;
  std::printf("connect: %s\n", take(rmf_client_connect(ws.c_str())).c_str());

  std::string yaml;
  if (!wait_for_yaml(&yaml)) {
    std::fprintf(stderr, "FAIL: no yaml after sync\n");
    return 1;
  }
  std::printf("synced=%d dirty=%d\nyaml:\n%s\n", rmf_client_is_synced(),
              rmf_client_remote_dirty(), yaml.c_str());

  const std::string edited = yaml + "\nsmoke_test_marker: true\n";
  std::printf("push: %s\n", take(rmf_client_push_yaml(edited.c_str())).c_str());
  std::this_thread::sleep_for(std::chrono::milliseconds(500));

  const std::string after = take(rmf_client_snapshot_yaml());
  const bool kept = after.find("smoke_test_marker") != std::string::npos;
  std::printf("marker survived round trip: %s\n", kept ? "yes" : "no");

  rmf_client_disconnect();
  return kept ? 0 : 1;
}
