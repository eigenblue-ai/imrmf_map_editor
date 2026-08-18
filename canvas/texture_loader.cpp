// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The ImRmfMapEditor Authors

#include "canvas/texture_loader.hpp"

#include <algorithm>

namespace imrmf::map_editor::canvas {

namespace {

// Enough to cover a floor's layers without fighting the render thread for
// cores. Decoding is memory bound well before it is core bound.
constexpr unsigned kMaxWorkers = 3;

} // namespace

AsyncTextureLoader::AsyncTextureLoader()
    : shared_(std::make_shared<Shared>()) {}

AsyncTextureLoader::~AsyncTextureLoader() {
  {
    std::lock_guard<std::mutex> lock(shared_->mu);
    shared_->stop = true;
    shared_->queue.clear();
  }
  shared_->cv.notify_all();
  // Detached rather than joined: a worker can be parked in a network timeout,
  // and closing a map should not wait for it.
  for (std::thread &t : workers_) {
    if (t.joinable())
      t.detach();
  }
}

void AsyncTextureLoader::ensure_workers() {
  const unsigned hw = std::max(1u, std::thread::hardware_concurrency());
  const unsigned want = std::min(kMaxWorkers, hw);
  while (workers_.size() < want) {
    std::shared_ptr<Shared> shared = shared_;
    workers_.emplace_back([shared]() {
      for (;;) {
        Job job;
        {
          std::unique_lock<std::mutex> lock(shared->mu);
          shared->cv.wait(lock, [&shared]() {
            return shared->stop || !shared->queue.empty();
          });
          if (shared->stop)
            return;
          job = std::move(shared->queue.front());
          shared->queue.pop_front();
        }

        Done done;
        done.cache_key = std::move(job.cache_key);
        std::vector<unsigned char> bytes;
        if (job.fetch && job.fetch(&bytes) && !bytes.empty()) {
          done.pixels =
              decode_pixels(bytes.data(), bytes.size(), job.tr, job.tg, job.tb);
        }

        std::lock_guard<std::mutex> lock(shared->mu);
        if (shared->stop)
          return;
        shared->done.push_back(std::move(done));
      }
    });
  }
}

void AsyncTextureLoader::submit(std::string cache_key, Fetch fetch, double tr,
                                double tg, double tb) {
  {
    std::lock_guard<std::mutex> lock(shared_->mu);
    if (shared_->stop)
      return;
    shared_->queue.push_back(
        Job{std::move(cache_key), std::move(fetch), tr, tg, tb});
  }
  ensure_workers();
  shared_->cv.notify_one();
}

void AsyncTextureLoader::drain(const Apply &apply) {
  std::vector<Done> ready;
  {
    std::lock_guard<std::mutex> lock(shared_->mu);
    ready.swap(shared_->done);
  }
  for (const Done &d : ready)
    apply(d.cache_key, d.pixels);
}

} // namespace imrmf::map_editor::canvas
