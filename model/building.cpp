// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The ImRmfMapEditor Authors

#include "model/building.hpp"

#include <cmath>
#include <string>
#include <unordered_map>
#include <vector>

namespace imrmf::map_editor {

namespace {

double direct_mpp(const Level &level) {
  if (!level.passthrough || !level.passthrough.IsMap())
    return 0.0;
  YAML::Node meas = level.passthrough["measurements"];
  if (!meas || !meas.IsSequence() || meas.size() == 0)
    return 0.0;
  double sum = 0.0;
  int count = 0;
  for (const auto &m : meas) {
    if (!m.IsSequence() || m.size() < 3)
      continue;
    int v0 = m[0].as<int>(-1);
    int v1 = m[1].as<int>(-1);
    if (v0 < 0 || v1 < 0 || v0 >= (int)level.vertices.size() ||
        v1 >= (int)level.vertices.size())
      continue;
    YAML::Node dist = m[2]["distance"];
    if (!dist || !dist.IsSequence() || dist.size() < 2)
      continue;
    double d_m = dist[1].as<double>(0.0);
    if (d_m <= 0.0)
      continue;
    double dx = level.vertices[v0].x - level.vertices[v1].x;
    double dy = level.vertices[v0].y - level.vertices[v1].y;
    double d_px = std::sqrt(dx * dx + dy * dy);
    if (d_px <= 0.0)
      continue;
    sum += d_m / d_px;
    ++count;
  }
  return count > 0 ? sum / count : 0.0;
}

double fiducial_distance_ratio(const Level &ref, const Level &other) {
  if (ref.fiducials.size() < 2 || other.fiducials.size() < 2)
    return 0.0;
  std::unordered_map<std::string, const Fiducial *> ref_by_name;
  for (const Fiducial &p : ref.fiducials)
    if (!p.name.empty())
      ref_by_name[p.name] = &p;
  std::vector<std::pair<const Fiducial *, const Fiducial *>> pairs;
  for (const Fiducial &p : other.fiducials) {
    auto it = ref_by_name.find(p.name);
    if (it != ref_by_name.end())
      pairs.push_back({it->second, &p});
  }
  if (pairs.size() < 2)
    return 0.0;
  double sum = 0.0;
  int n = 0;
  for (size_t i = 0; i < pairs.size(); ++i) {
    for (size_t j = i + 1; j < pairs.size(); ++j) {
      double rdx = pairs[i].first->x - pairs[j].first->x;
      double rdy = pairs[i].first->y - pairs[j].first->y;
      double odx = pairs[i].second->x - pairs[j].second->x;
      double ody = pairs[i].second->y - pairs[j].second->y;
      double rdist = std::sqrt(rdx * rdx + rdy * rdy);
      double odist = std::sqrt(odx * odx + ody * ody);
      if (rdist > 0.0 && odist > 0.0) {
        sum += rdist / odist;
        ++n;
      }
    }
  }
  return n > 0 ? sum / n : 0.0;
}

} // namespace

double compute_level_mpp(const Building &building, int level_idx) {
  if (level_idx < 0 || level_idx >= (int)building.levels.size())
    return 0.0;
  const Level &self = building.levels[level_idx];

  double direct = direct_mpp(self);
  if (direct > 0.0)
    return direct;

  for (size_t i = 0; i < building.levels.size(); ++i) {
    if ((int)i == level_idx)
      continue;
    const Level &other = building.levels[i];
    double other_mpp = direct_mpp(other);
    if (other_mpp <= 0.0)
      continue;
    double ratio = fiducial_distance_ratio(other, self);
    if (ratio > 0.0)
      return other_mpp * ratio;
  }

  return 0.0;
}

std::vector<int> lanes_referencing_vertex(const Level &level, int vertex_idx) {
  std::vector<int> out;
  for (int i = 0; i < (int)level.lanes.size(); ++i) {
    const Lane &l = level.lanes[i];
    if (l.start_idx == vertex_idx || l.end_idx == vertex_idx) {
      out.push_back(i);
    }
  }
  return out;
}

void delete_vertex(Level &level, int vertex_idx) {
  if (vertex_idx < 0 || vertex_idx >= (int)level.vertices.size())
    return;

  std::vector<Lane> kept;
  kept.reserve(level.lanes.size());
  for (const Lane &l : level.lanes) {
    if (l.start_idx == vertex_idx || l.end_idx == vertex_idx)
      continue;
    Lane copy = l;
    if (copy.start_idx > vertex_idx)
      --copy.start_idx;
    if (copy.end_idx > vertex_idx)
      --copy.end_idx;
    kept.push_back(std::move(copy));
  }
  level.lanes = std::move(kept);
  level.vertices.erase(level.vertices.begin() + vertex_idx);
}

void delete_lane(Level &level, int lane_idx) {
  if (lane_idx < 0 || lane_idx >= (int)level.lanes.size())
    return;
  level.lanes.erase(level.lanes.begin() + lane_idx);
}

FloorTransform compute_floor_transform(const std::vector<Fiducial> &ref,
                                       const std::vector<Fiducial> &target,
                                       double default_scale) {
  FloorTransform out;
  out.scale = default_scale;
  std::vector<std::pair<const Fiducial *, const Fiducial *>> pairs;
  for (const Fiducial &r : ref) {
    if (r.name.empty()) continue;
    for (const Fiducial &t : target) {
      if (t.name == r.name) {
        pairs.push_back({&r, &t});
        break;
      }
    }
  }
  out.matched = (int)pairs.size();
  if (pairs.empty()) return out;
  if (pairs.size() == 1) {
    out.tx = pairs[0].first->x - default_scale * pairs[0].second->x;
    out.ty = pairs[0].first->y - default_scale * pairs[0].second->y;
    return out;
  }
  double rcx = 0, rcy = 0, tcx = 0, tcy = 0;
  for (auto &p : pairs) {
    rcx += p.first->x; rcy += p.first->y;
    tcx += p.second->x; tcy += p.second->y;
  }
  double n = (double)pairs.size();
  rcx /= n; rcy /= n; tcx /= n; tcy /= n;
  double A = 0, B = 0, sum_t_sq = 0;
  for (auto &p : pairs) {
    double rx = p.first->x - rcx, ry = p.first->y - rcy;
    double tx = p.second->x - tcx, ty = p.second->y - tcy;
    A += rx * tx + ry * ty;
    B += ry * tx - rx * ty;
    sum_t_sq += tx * tx + ty * ty;
  }
  out.yaw = std::atan2(B, A);
  if (sum_t_sq > 1e-12)
    out.scale = std::sqrt(A * A + B * B) / sum_t_sq;
  double cy = std::cos(out.yaw), sy = std::sin(out.yaw);
  out.tx = rcx - out.scale * (cy * tcx - sy * tcy);
  out.ty = rcy - out.scale * (sy * tcx + cy * tcy);
  return out;
}

std::pair<double, double> tgt_to_ref(const FloorTransform &xf, double ix,
                                     double iy) {
  double cy = std::cos(xf.yaw), sy = std::sin(xf.yaw);
  double a = ix * xf.scale, b = iy * xf.scale;
  return {xf.tx + cy * a - sy * b, xf.ty + sy * a + cy * b};
}

std::pair<double, double> ref_to_tgt(const FloorTransform &xf, double rx,
                                     double ry) {
  if (xf.scale < 1e-12) return {0.0, 0.0};
  double cy = std::cos(xf.yaw), sy = std::sin(xf.yaw);
  double dx = rx - xf.tx, dy = ry - xf.ty;
  double ix = (cy * dx + sy * dy) / xf.scale;
  double iy = (-sy * dx + cy * dy) / xf.scale;
  return {ix, iy};
}

} // namespace imrmf::map_editor
