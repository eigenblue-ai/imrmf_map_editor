// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The ImRmfMapEditor Authors

#include "model/edit_history.hpp"

#include <utility>

namespace imrmf::map_editor {
namespace {

// A map node holding a string key and a ParamValue, near enough.
constexpr std::size_t kParamBytes = 96;

std::size_t params_bytes(const std::map<std::string, ParamValue> &p) {
  return p.size() * kParamBytes;
}

template <typename T> std::size_t vec_bytes(const std::vector<T> &v) {
  return v.capacity() * sizeof(T);
}

} // namespace

std::size_t approx_footprint(const Building &b) {
  std::size_t n = sizeof(Building) + b.name.capacity() +
                  b.coordinate_system.capacity() +
                  b.reference_level_name.capacity() + vec_bytes(b.levels);
  for (const Level &l : b.levels) {
    n += l.name.capacity() + l.drawing_filename.capacity();
    n += vec_bytes(l.vertices) + vec_bytes(l.lanes) + vec_bytes(l.walls) +
         vec_bytes(l.doors) + vec_bytes(l.measurements) + vec_bytes(l.floors) +
         vec_bytes(l.fiducials) + vec_bytes(l.layers);
    for (const Vertex &v : l.vertices)
      n += v.name.capacity() + params_bytes(v.params);
    for (const Lane &e : l.lanes)
      n += params_bytes(e.params);
    for (const Wall &e : l.walls)
      n += params_bytes(e.params);
    for (const Door &e : l.doors)
      n += params_bytes(e.params);
    for (const Measurement &e : l.measurements)
      n += params_bytes(e.params);
    for (const Floor &f : l.floors) {
      n += vec_bytes(f.vertices) + vec_bytes(f.holes) + params_bytes(f.params);
      for (const std::vector<int> &hole : f.holes)
        n += vec_bytes(hole);
    }
    for (const Fiducial &f : l.fiducials)
      n += f.name.capacity();
    for (const Layer &lay : l.layers)
      n += lay.name.capacity() + lay.filename.capacity();
  }
  return n;
}

EditHistory::EditHistory(const Building &initial, std::size_t limit,
                         std::size_t budget_bytes)
    : current_(initial), limit_(limit ? limit : 1),
      budget_bytes_(budget_bytes) {}

void EditHistory::reset(const Building &initial) {
  current_ = initial;
  past_.clear();
  future_.clear();
  past_bytes_ = 0;
  future_bytes_ = 0;
}

void EditHistory::commit(const Building &now) {
  const std::size_t bytes = approx_footprint(current_);
  past_.push_back({std::move(current_), bytes});
  past_bytes_ += bytes;
  current_ = now;
  // A fresh edit is a new branch, so anything undone is no longer reachable.
  future_.clear();
  future_bytes_ = 0;
  trim();
}

void EditHistory::trim() {
  while (past_.size() > limit_ ||
         (past_.size() > kMinSteps && past_bytes_ > budget_bytes_)) {
    past_bytes_ -= past_.front().bytes;
    past_.pop_front();
  }
}

bool EditHistory::undo(Building &out) {
  if (past_.empty())
    return false;
  const std::size_t bytes = approx_footprint(current_);
  future_.push_back({std::move(current_), bytes});
  future_bytes_ += bytes;
  past_bytes_ -= past_.back().bytes;
  current_ = std::move(past_.back().map);
  past_.pop_back();
  out = current_;
  return true;
}

bool EditHistory::redo(Building &out) {
  if (future_.empty())
    return false;
  const std::size_t bytes = approx_footprint(current_);
  past_.push_back({std::move(current_), bytes});
  past_bytes_ += bytes;
  future_bytes_ -= future_.back().bytes;
  current_ = std::move(future_.back().map);
  future_.pop_back();
  out = current_;
  return true;
}

} // namespace imrmf::map_editor
