// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The ImRmfMapEditor Authors

#pragma once

#include "model/building.hpp"

#include <cstddef>
#include <deque>

namespace imrmf::map_editor {

// Roughly what a snapshot of this map occupies. Approximate on purpose, it
// bounds the history rather than accounting for it.
std::size_t approx_footprint(const Building &b);

// Undo/redo for a session with no server behind it.
//
// A file session used to keep a CRDT document only so undo had something to
// rewind, which made every edit serialize the whole map to yaml and reconcile
// it back. A snapshot is a struct copy, so the cost stops scaling with the map.
//
// Building holds YAML::Node passthrough members, which copy by reference rather
// than by value. Nothing in the editor mutates those (written at parse, read at
// serialize), so snapshots sharing them is safe.
class EditHistory {
public:
  // A snapshot of a large map runs to a megabyte or two, so depth alone is a
  // poor bound. Oldest steps fall off over budget, never below kMinSteps.
  static constexpr std::size_t kDefaultLimit = 100;
  static constexpr std::size_t kDefaultBudgetBytes = 64u * 1024 * 1024;
  static constexpr std::size_t kMinSteps = 8;

  EditHistory() = default;
  explicit EditHistory(const Building &initial,
                       std::size_t limit = kDefaultLimit,
                       std::size_t budget_bytes = kDefaultBudgetBytes);

  // Starts over from `initial`, dropping every step.
  void reset(const Building &initial);

  // Records `now` as current, the state before it becoming an undo step. Call
  // once per burst of edits, so a drag is one step rather than one per frame.
  void commit(const Building &now);

  bool can_undo() const { return !past_.empty(); }
  bool can_redo() const { return !future_.empty(); }

  // Moves `out` one step. False when there is nowhere to go, `out` untouched.
  bool undo(Building &out);
  bool redo(Building &out);

  std::size_t undo_depth() const { return past_.size(); }
  std::size_t redo_depth() const { return future_.size(); }
  std::size_t bytes_held() const { return past_bytes_ + future_bytes_; }

private:
  struct Step {
    Building map;
    std::size_t bytes = 0;
  };

  void trim();

  Building current_;
  std::deque<Step> past_;
  std::deque<Step> future_;
  std::size_t past_bytes_ = 0;
  std::size_t future_bytes_ = 0;
  std::size_t limit_ = kDefaultLimit;
  std::size_t budget_bytes_ = kDefaultBudgetBytes;
};

} // namespace imrmf::map_editor
