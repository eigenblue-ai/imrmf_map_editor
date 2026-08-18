// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The ImRmfMapEditor Authors

#pragma once

#include "canvas/texture_decode.hpp"

#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace imrmf::map_editor::canvas {

// Fetch and decode on worker threads, upload on the render thread. The browser
// gets this from a JS worker. Doing it inline instead stalls the frame for as
// long as the read and the stb decode take, which is what made the first switch
// to a floor lock up the window.
class AsyncTextureLoader {
public:
  // Runs on a worker. Returns false when the bytes cannot be had.
  using Fetch = std::function<bool(std::vector<unsigned char> *)>;
  using Apply = std::function<void(const std::string &, const DecodedPixels &)>;

  AsyncTextureLoader();
  ~AsyncTextureLoader();

  void submit(std::string cache_key, Fetch fetch, double tint_r, double tint_g,
              double tint_b);

  // Render thread. Hands every finished job to `apply` and clears the queue.
  void drain(const Apply &apply);

private:
  struct Job {
    std::string cache_key;
    Fetch fetch;
    double tr = 1.0, tg = 1.0, tb = 1.0;
  };
  struct Done {
    std::string cache_key;
    DecodedPixels pixels;
  };

  // Outlives the loader, so closing a map does not wait on a fetch parked in a
  // network timeout. The workers are detached and finish against this.
  struct Shared {
    std::mutex mu;
    std::condition_variable cv;
    std::deque<Job> queue;
    std::vector<Done> done;
    bool stop = false;
  };

  void ensure_workers();

  std::shared_ptr<Shared> shared_;
  std::vector<std::thread> workers_;
};

} // namespace imrmf::map_editor::canvas
