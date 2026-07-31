#pragma once

#include <cstring>
#include <vector>

namespace ImageUtils {

// Flood-fill white inward from the edges and set those pixels transparent.
// RGBA, 4 bytes/pixel, threshold is the 0-255 white cutoff.
inline void remove_white_background(unsigned char *image_data, int width,
                                    int height, unsigned char threshold = 245) {
  if (!image_data || width <= 0 || height <= 0)
    return;

  std::vector<bool> visited(width * height, false);
  std::vector<std::pair<int, int>> queue;

  auto is_white = [&](int x, int y) {
    int idx = (y * width + x) * 4;
    unsigned char r = image_data[idx];
    unsigned char g = image_data[idx + 1];
    unsigned char b = image_data[idx + 2];
    return (r >= threshold && g >= threshold && b >= threshold);
  };

  auto flood_fill = [&](int start_x, int start_y) {
    if (visited[start_y * width + start_x] || !is_white(start_x, start_y))
      return;

    queue.clear();
    queue.push_back({start_x, start_y});
    visited[start_y * width + start_x] = true;

    while (!queue.empty()) {
      auto [x, y] = queue.back();
      queue.pop_back();

      int idx = (y * width + x) * 4;
      image_data[idx + 3] = 0;

      // 4-connected neighbors.
      int dx[] = {-1, 1, 0, 0};
      int dy[] = {0, 0, -1, 1};
      for (int i = 0; i < 4; i++) {
        int nx = x + dx[i];
        int ny = y + dy[i];
        if (nx >= 0 && nx < width && ny >= 0 && ny < height) {
          int nidx = ny * width + nx;
          if (!visited[nidx] && is_white(nx, ny)) {
            visited[nidx] = true;
            queue.push_back({nx, ny});
          }
        }
      }
    }
  };

  // Seed the fill from every edge pixel.
  for (int x = 0; x < width; x++) {
    flood_fill(x, 0);
    flood_fill(x, height - 1);
  }
  for (int y = 0; y < height; y++) {
    flood_fill(0, y);
    flood_fill(width - 1, y);
  }
}

} // namespace ImageUtils
