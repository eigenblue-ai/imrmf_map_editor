// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The ImRmfMapEditor Authors
//
// The file session's undo/redo stack.

#include "model/edit_history.hpp"

#include "gtest/gtest.h"

#include <cstddef>
#include <string>

using namespace imrmf::map_editor;

namespace {

// A map identified by its name, all these tests need to tell states apart.
Building named(const std::string &name) {
  Building b;
  b.name = name;
  b.levels.emplace_back();
  b.levels.back().name = "L1";
  return b;
}

TEST(EditHistory, UndoAndRedoWalkTheSteps) {
  EditHistory h(named("a"));
  EXPECT_FALSE(h.can_undo());
  EXPECT_FALSE(h.can_redo());

  h.commit(named("b"));
  h.commit(named("c"));
  EXPECT_TRUE(h.can_undo());
  EXPECT_FALSE(h.can_redo());

  Building out = named("c");
  EXPECT_TRUE(h.undo(out));
  EXPECT_EQ(out.name, "b");
  EXPECT_TRUE(h.undo(out));
  EXPECT_EQ(out.name, "a");
  EXPECT_FALSE(h.can_undo());

  // Nowhere further back, and the map is left as it was.
  EXPECT_FALSE(h.undo(out));
  EXPECT_EQ(out.name, "a");

  EXPECT_TRUE(h.redo(out));
  EXPECT_EQ(out.name, "b");
  EXPECT_TRUE(h.redo(out));
  EXPECT_EQ(out.name, "c");
  EXPECT_FALSE(h.redo(out));
  EXPECT_EQ(out.name, "c");
}

TEST(EditHistory, AnEditAfterUndoDropsTheRedoBranch) {
  EditHistory h(named("a"));
  h.commit(named("b"));

  Building out;
  EXPECT_TRUE(h.undo(out));
  EXPECT_EQ(out.name, "a");
  EXPECT_TRUE(h.can_redo());

  h.commit(named("c"));
  EXPECT_FALSE(h.can_redo());
  EXPECT_TRUE(h.undo(out));
  EXPECT_EQ(out.name, "a");
}

TEST(EditHistory, TheOldestStepFallsOffAtTheLimit) {
  EditHistory h(named("s0"), /*limit=*/3);
  for (int i = 1; i <= 6; ++i)
    h.commit(named("s" + std::to_string(i)));
  EXPECT_EQ(h.undo_depth(), 3);

  // Three steps back from s6 reaches s3, and no further.
  Building out;
  EXPECT_TRUE(h.undo(out));
  EXPECT_EQ(out.name, "s5");
  EXPECT_TRUE(h.undo(out));
  EXPECT_EQ(out.name, "s4");
  EXPECT_TRUE(h.undo(out));
  EXPECT_EQ(out.name, "s3");
  EXPECT_FALSE(h.undo(out));
}

// Each step of a large map is expensive, so old steps drop off at the budget.
TEST(EditHistory, StaysInsideItsMemoryBudget) {
  Building big = named("big");
  big.levels.back().vertices.resize(20000);
  const std::size_t step = approx_footprint(big);
  EXPECT_TRUE(step > 0);

  // Room for about four steps, which is under the floor of kMinSteps.
  EditHistory h(big, EditHistory::kDefaultLimit, step * 4);
  for (int i = 0; i < 40; ++i) {
    big.levels.back().vertices[i].name = "v" + std::to_string(i);
    h.commit(big);
  }
  EXPECT_EQ(h.undo_depth(), EditHistory::kMinSteps);
  EXPECT_LE(h.bytes_held(), step * (EditHistory::kMinSteps + 1));

  // Undo still walks back through the steps that were kept.
  Building out;
  for (std::size_t i = 0; i < EditHistory::kMinSteps; ++i)
    EXPECT_TRUE(h.undo(out));
  EXPECT_FALSE(h.undo(out));
}

TEST(EditHistory, ResetForgetsEverything) {
  EditHistory h(named("a"));
  h.commit(named("b"));
  Building out;
  EXPECT_TRUE(h.undo(out));

  h.reset(named("z"));
  EXPECT_FALSE(h.can_undo());
  EXPECT_FALSE(h.can_redo());
  EXPECT_FALSE(h.undo(out));
}

// The map itself round trips, not just the name it is identified by.
TEST(EditHistory, AStepRestoresTheWholeMap) {
  Building before = named("m");
  before.levels.back().vertices.resize(2);
  before.levels.back().vertices[0].name = "v0";

  EditHistory h(before);
  Building after = before;
  after.levels.back().vertices.resize(3);
  after.levels.back().vertices[2].name = "added";
  h.commit(after);

  Building out = after;
  EXPECT_TRUE(h.undo(out));
  EXPECT_EQ(out.levels.back().vertices.size(), 2);
  EXPECT_EQ(out.levels.back().vertices[0].name, "v0");
  EXPECT_TRUE(h.redo(out));
  EXPECT_EQ(out.levels.back().vertices.size(), 3);
  EXPECT_EQ(out.levels.back().vertices[2].name, "added");
}

} // namespace
