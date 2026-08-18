// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The ImRmfMapEditor Authors
//
// Round-trip + cascade tests for the geometry that Phase 1 lifts out of
// `passthrough`: walls, doors, measurements, floors.

#include "model/building.hpp"
#include "model/yaml_io.hpp"

#include "gtest/gtest.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace imrmf::map_editor;

namespace {

// Mirrors the shape of a real building.yaml level (cf. H12 + door_madness):
// walls, a measurement, a floor with parameters, and two doors.
const char *kYaml = R"(name: test_building
coordinate_system: reference_image
levels:
  L1:
    elevation: 0
    drawing:
      filename: floor.png
    vertices:
      - [0, 0, 0, ""]
      - [100, 0, 0, ""]
      - [100, 100, 0, ""]
      - [0, 100, 0, ""]
      - [50, 50, 0, charger, {is_charger: [4, true]}]
    lanes:
      - [0, 1, {bidirectional: [4, false], graph_idx: [2, 0]}]
    walls:
      - [0, 1, {texture_name: [1, default], texture_height: [3, 2.5], alpha: [3, 1]}]
      - [1, 2, {texture_name: [1, default], texture_height: [3, 2.5], alpha: [3, 1]}]
      - [2, 3, {texture_name: [1, default]}]
    doors:
      - [0, 3, {motion_axis: [1, start], motion_degrees: [3, 90], motion_direction: [2, -1], name: [1, d1], type: [1, double_hinged]}]
    measurements:
      - [0, 1, {distance: [3, 5]}]
    floors:
      - parameters: {texture_name: [1, blue_linoleum], texture_rotation: [3, 0], texture_scale: [3, 1]}
        vertices: [0, 1, 2, 3]
)";

TEST(YamlIo, RoundTripParses) {
  Building b = parse_building(kYaml);
  EXPECT_EQ(b.levels.size(), 1);
  const Level &l = b.levels[0];
  EXPECT_EQ(l.vertices.size(), 5);
  EXPECT_EQ(l.lanes.size(), 1);
  EXPECT_EQ(l.walls.size(), 3);
  EXPECT_EQ(l.doors.size(), 1);
  EXPECT_EQ(l.measurements.size(), 1);
  EXPECT_EQ(l.floors.size(), 1);

  // Walls keep their endpoints + params.
  EXPECT_EQ(l.walls[0].start_idx, 0);
  EXPECT_EQ(l.walls[0].end_idx, 1);
  EXPECT_EQ(l.walls[0].params.count("texture_name"), 1);
  EXPECT_EQ(l.walls[0].params.at("texture_name").s, "default");

  // Door params survive with correct types.
  EXPECT_EQ(l.doors[0].params.at("type").s, "double_hinged");
  EXPECT_EQ(l.doors[0].params.at("motion_direction").type, ParamType::INT);
  EXPECT_EQ(l.doors[0].params.at("motion_direction").i, -1);

  // Floor: closed loop + parameters under the "parameters" key.
  EXPECT_EQ(l.floors[0].vertices.size(), 4);
  EXPECT_EQ(l.floors[0].vertices[2], 2);
  EXPECT_EQ(l.floors[0].params.at("texture_name").s, "blue_linoleum");

  // Measurement feeds mpp: 5 m over 100 px => 0.05 m/px.
  double mpp = compute_level_mpp(b, 0);
  EXPECT_TRUE(std::abs(mpp - 0.05) < 1e-9);
}

// parse -> serialize -> parse must preserve every typed field.
TEST(YamlIo, RoundTripIsStable) {
  Building b1 = parse_building(kYaml);
  std::string out = serialize_building(b1);
  Building b2 = parse_building(out);
  const Level &a = b1.levels[0];
  const Level &c = b2.levels[0];
  EXPECT_EQ(a.walls.size(), c.walls.size());
  EXPECT_EQ(a.doors.size(), c.doors.size());
  EXPECT_EQ(a.measurements.size(), c.measurements.size());
  EXPECT_EQ(a.floors.size(), c.floors.size());
  for (size_t i = 0; i < a.walls.size(); ++i) {
    EXPECT_EQ(a.walls[i].start_idx, c.walls[i].start_idx);
    EXPECT_EQ(a.walls[i].end_idx, c.walls[i].end_idx);
    EXPECT_EQ(a.walls[i].params.size(), c.walls[i].params.size());
  }
  EXPECT_EQ(a.floors[0].vertices, c.floors[0].vertices);
  EXPECT_EQ(c.doors[0].params.at("type").s, "double_hinged");
  EXPECT_TRUE(std::abs(compute_level_mpp(b2, 0) - 0.05) < 1e-9);
}

// Keys we don't model (models, flattened_*) must round-trip untouched.
TEST(YamlIo, PassthroughIsPreserved) {
  const char *with_extra = R"(name: t
levels:
  L1:
    elevation: 0
    vertices: [[0,0,0,""],[10,0,0,""],[10,10,0,""]]
    floors:
      - parameters: {}
        vertices: [0, 1, 2]
    models:
      - {model_name: VendingMachine, name: vm, static: true, x: 1, y: 2, yaw: 0, z: 0}
    flattened_x_offset: 3
)";
  Building b = parse_building(with_extra);
  std::string out = serialize_building(b);
  Building b2 = parse_building(out);
  EXPECT_TRUE(b2.levels[0].passthrough["models"]);
  EXPECT_TRUE(b2.levels[0].passthrough["flattened_x_offset"]);
  EXPECT_EQ(b2.levels[0].floors.size(), 1);
}

// Deleting a vertex must reindex/cull every geometry kind, not just lanes.
TEST(YamlIo, DeleteVertexCascades) {
  Building b = parse_building(kYaml);
  Level &l = b.levels[0];
  // Delete vertex 1 (used by lane 0->1, wall 0->1 & 1->2, measurement, door,
  // and floor loop).
  delete_vertex(l, 1);
  EXPECT_EQ(l.vertices.size(), 4);
  // Lane 0->1 referenced v1 => dropped.
  EXPECT_TRUE(l.lanes.empty());
  // walls 0-1 and 1-2 touched v1 so they drop, 2-3 survives and reindexes to
  // 1-2
  EXPECT_EQ(l.walls.size(), 1);
  EXPECT_EQ(l.walls[0].start_idx, 1);
  EXPECT_EQ(l.walls[0].end_idx, 2);
  // Measurement 0-1 referenced v1 => dropped.
  EXPECT_TRUE(l.measurements.empty());
  // Door 0-3 didn't touch v1 but index 3 shifts to 2.
  EXPECT_EQ(l.doors.size(), 1);
  EXPECT_EQ(l.doors[0].start_idx, 0);
  EXPECT_EQ(l.doors[0].end_idx, 2);
  // Floor loop [0,1,2,3] -> remove 1, shift => [0,1,2], still >= 3 vertices.
  EXPECT_EQ(l.floors.size(), 1);
  EXPECT_TRUE((l.floors[0].vertices == std::vector<int>{0, 1, 2}));
}

} // namespace

int roundtrip_file(const char *path) {
  std::FILE *fp = std::fopen(path, "rb");
  if (!fp) {
    std::printf("cannot open %s\n", path);
    return 1;
  }
  std::string src;
  char buf[8192];
  size_t n;
  while ((n = std::fread(buf, 1, sizeof(buf), fp)) > 0)
    src.append(buf, n);
  std::fclose(fp);
  Building b = parse_building(src);
  std::string s1 = serialize_building(b);
  Building b2 = parse_building(s1);
  int rc = 0;
  if (serialize_building(b2) != s1) {
    std::printf("  NOT IDEMPOTENT: re-serialization differs\n");
    rc = 1;
  }
  for (size_t i = 0; i < b.levels.size(); ++i) {
    const Level &a = b.levels[i];
    const Level &c = b2.levels[i];
    std::printf("  %-16s verts=%zu lanes=%zu walls=%zu doors=%zu meas=%zu "
                "floors=%zu fids=%zu\n",
                a.name.c_str(), a.vertices.size(), a.lanes.size(),
                a.walls.size(), a.doors.size(), a.measurements.size(),
                a.floors.size(), a.fiducials.size());
    if (a.walls.size() != c.walls.size() || a.doors.size() != c.doors.size() ||
        a.measurements.size() != c.measurements.size() ||
        a.floors.size() != c.floors.size() ||
        a.vertices.size() != c.vertices.size()) {
      std::printf("  MISMATCH after round-trip on level %s\n", a.name.c_str());
      rc = 1;
    }
  }
  return rc;
}

// Ad-hoc entry point: `yaml_io_test --roundtrip <file.building.yaml>...`
// round-trips real maps and reports geometry counts, instead of running the
// test cases. `bazel test` passes no such flag and takes the normal path.
int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  if (argc > 2 && std::string(argv[1]) == "--roundtrip") {
    int rc = 0;
    for (int i = 2; i < argc; ++i) {
      std::printf("== %s ==\n", argv[i]);
      rc |= roundtrip_file(argv[i]);
    }
    return rc;
  }
  return RUN_ALL_TESTS();
}
