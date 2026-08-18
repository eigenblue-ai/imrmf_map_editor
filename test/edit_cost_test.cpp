// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The ImRmfMapEditor Authors
//
// What one edit is allowed to cost.
//
// A file session used to record every edit by serializing the whole map to yaml
// and reconciling it into a CRDT document, only so undo had something to
// rewind. That is work in proportion to the map on every click. It is a
// snapshot now.
//
// Timing tests, so they assert ratios with room to spare rather than
// millisecond budgets, and a loaded machine scales both sides alike. They print
// what they measured, so a run doubles as a benchmark.

#include "model/building.hpp"
#include "model/edit_history.hpp"
#include "model/yaml_io.hpp"

#include "gtest/gtest.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <string>
#include <vector>

using namespace imrmf::map_editor;

namespace {

using Clock = std::chrono::steady_clock;

// Keeps an optimized build from discarding work whose result goes unused.
volatile std::size_t g_sink = 0;
void sink(std::size_t v) { g_sink = v; }

std::string map_yaml(int vertices) {
  std::string y = "name: bench\ncoordinate_system: reference_image\n"
                  "levels:\n  L1:\n    elevation: 0\n    vertices:\n";
  for (int i = 0; i < vertices; ++i)
    y += "      - [" + std::to_string(i) + ".5, " + std::to_string(i) +
         ".25, 0, \"v" + std::to_string(i) + "\"]\n";
  y += "    lanes:\n";
  for (int i = 0; i + 1 < vertices; ++i)
    y += "      - [" + std::to_string(i) + ", " + std::to_string(i + 1) +
         ", {bidirectional: [4, false], graph_idx: [2, 0]}]\n";
  return y;
}

// Median of `runs`, so one scheduling hiccup does not decide the result.
template <typename F> double median_ms(int runs, F &&body) {
  std::vector<double> samples;
  samples.reserve(runs);
  for (int i = 0; i < runs; ++i) {
    const Clock::time_point t0 = Clock::now();
    body(i);
    samples.push_back(
        std::chrono::duration<double, std::milli>(Clock::now() - t0).count());
  }
  std::sort(samples.begin(), samples.end());
  return samples[samples.size() / 2];
}

constexpr int kVertices = 2000;
constexpr int kRuns = 9;

// The regression this file exists for. A yaml round trip is what recording an
// edit used to cost, a snapshot is what it costs now. Anything close to parity
// means the old path is back.
TEST(EditCost, ASnapshotIsOrdersCheaperThanAYamlRoundTrip) {
  const std::string yaml = map_yaml(kVertices);
  Building b = parse_building(yaml);

  EditHistory history(b);
  const double snapshot = median_ms(kRuns, [&](int i) {
    b.levels[0].vertices[i].name = "moved" + std::to_string(i);
    history.commit(b);
  });

  const double round_trip = median_ms(kRuns, [&](int) {
    const std::string out = serialize_building(b);
    Building parsed = parse_building(out);
    sink(parsed.levels.size());
  });

  std::printf("%d vertices: snapshot %.2fms, yaml round trip %.2fms (%.0fx)\n",
              kVertices, snapshot, round_trip, round_trip / snapshot);
  EXPECT_LT(snapshot * 20.0, round_trip);
}

// A snapshot grows with the map but nowhere near fast enough to be felt. The
// bound is loose, an unoptimized build measures about a millisecond here.
TEST(EditCost, RecordingAnEditStaysOffTheFrame) {
  Building b = parse_building(map_yaml(kVertices));
  EditHistory history(b);
  const double snapshot = median_ms(kRuns, [&](int i) {
    b.levels[0].vertices[i].name = "moved" + std::to_string(i);
    history.commit(b);
  });
  std::printf("%d vertices: recording one edit takes %.2fms\n", kVertices,
              snapshot);
  EXPECT_LT(snapshot, 50.0);
}

// Undo has to be as cheap as the edit that made the step, or holding Ctrl+Z
// stalls the way editing used to.
TEST(EditCost, UndoCostsAboutWhatAnEditDoes) {
  Building b = parse_building(map_yaml(kVertices));
  EditHistory history(b);
  for (int i = 0; i < kRuns + 1; ++i) {
    b.levels[0].vertices[i].name = "moved" + std::to_string(i);
    history.commit(b);
  }
  Building out = b;
  const double undo = median_ms(kRuns, [&](int) { history.undo(out); });
  std::printf("%d vertices: one undo takes %.2fms\n", kVertices, undo);
  EXPECT_LT(undo, 50.0);
}

} // namespace
