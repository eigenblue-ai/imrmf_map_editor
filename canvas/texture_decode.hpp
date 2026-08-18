// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The ImRmfMapEditor Authors

#pragma once

#include "canvas/canvas.hpp"

#include <cstddef>
#include <vector>

namespace imrmf::map_editor::canvas {

unsigned int upload_rgba(const unsigned char *data, int w, int h);

// Grayscale floorplan-style image -> tinted RGBA. Dark pixels take the tint,
// near-white goes transparent, mid-tones stay faint.
std::vector<unsigned char> colorize_rgba(const unsigned char *gray, int w,
                                         int h, double cr, double cg,
                                         double cb);

// Sparse sample: true if any pixel has unequal channels.
bool detect_color(const unsigned char *rgba, int w, int h);

// Re-tints an already-decoded grayscale layer in place and re-uploads it.
void regenerate_colorize(LayerTexture &tex, double cr, double cg, double cb);

// The CPU half of a decode: everything except the GL calls, so it can run off
// the render thread.
struct DecodedPixels {
  bool ok = false;
  int width = 0, height = 0;
  bool is_color = false;
  std::vector<unsigned char> rgba;
  std::vector<unsigned char> rgba_inv;
  std::vector<unsigned char> grayscale; // empty for a colour image
};

DecodedPixels decode_pixels(const unsigned char *bytes, std::size_t len,
                            double tint_r, double tint_g, double tint_b);

// The GL half. Must run on the render thread.
void upload_decoded(LayerTexture &out, const DecodedPixels &px);

// Decodes image bytes and fills `out`: dimensions, the normal and inverted GL
// textures, the grayscale copy re-tinting needs, and status.
void decode_into_texture(LayerTexture &out, const unsigned char *bytes,
                         std::size_t len, double tint_r, double tint_g,
                         double tint_b);

} // namespace imrmf::map_editor::canvas
