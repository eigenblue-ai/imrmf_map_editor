// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The ImRmfMapEditor Authors
//
// The point of the loader is that submitting never blocks the render thread.

#include "canvas/texture_loader.hpp"

#include "gtest/gtest.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <thread>

using imrmf::map_editor::canvas::AsyncTextureLoader;
using imrmf::map_editor::canvas::DecodedPixels;

namespace {

using Clock = std::chrono::steady_clock;

long long ms_since(Clock::time_point t0) {
  return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() -
                                                               t0)
      .count();
}

long long us_since(Clock::time_point t0) {
  return std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() -
                                                               t0)
      .count();
}

// Uncompressed zlib blocks, so the test needs no deflate implementation.
std::vector<unsigned char> make_png(int w, int h) {
  auto be32 = [](std::vector<unsigned char> &v, unsigned x) {
    v.push_back((unsigned char)(x >> 24));
    v.push_back((unsigned char)(x >> 16));
    v.push_back((unsigned char)(x >> 8));
    v.push_back((unsigned char)x);
  };
  std::vector<unsigned char> raw;
  raw.reserve((size_t)h * (w * 3 + 1));
  for (int y = 0; y < h; ++y) {
    raw.push_back(0);
    for (int x = 0; x < w; ++x) {
      raw.push_back((unsigned char)(x % 251));
      raw.push_back((unsigned char)(y % 241));
      raw.push_back((unsigned char)((x + y) % 239));
    }
  }
  std::vector<unsigned char> z;
  z.push_back(0x78);
  z.push_back(0x01);
  size_t off = 0;
  while (off < raw.size()) {
    const size_t n = std::min<size_t>(65535, raw.size() - off);
    z.push_back(off + n >= raw.size() ? 1 : 0);
    z.push_back((unsigned char)(n & 0xff));
    z.push_back((unsigned char)(n >> 8));
    z.push_back((unsigned char)(~n & 0xff));
    z.push_back((unsigned char)((~n >> 8) & 0xff));
    z.insert(z.end(), raw.begin() + off, raw.begin() + off + n);
    off += n;
  }
  unsigned a1 = 1, b1 = 0;
  for (unsigned char c : raw) {
    a1 = (a1 + c) % 65521;
    b1 = (b1 + a1) % 65521;
  }
  be32(z, (b1 << 16) | a1);

  auto crc = [](const std::vector<unsigned char> &v) {
    static unsigned table[256];
    static bool built = false;
    if (!built) {
      for (unsigned i = 0; i < 256; ++i) {
        unsigned c = i;
        for (int k = 0; k < 8; ++k)
          c = (c & 1) ? 0xedb88320u ^ (c >> 1) : c >> 1;
        table[i] = c;
      }
      built = true;
    }
    unsigned c = 0xffffffffu;
    for (unsigned char x : v)
      c = table[(c ^ x) & 0xff] ^ (c >> 8);
    return c ^ 0xffffffffu;
  };
  auto chunk = [&](std::vector<unsigned char> &out, const char *type,
                   const std::vector<unsigned char> &data) {
    be32(out, (unsigned)data.size());
    std::vector<unsigned char> body(type, type + 4);
    body.insert(body.end(), data.begin(), data.end());
    out.insert(out.end(), body.begin(), body.end());
    be32(out, crc(body));
  };

  std::vector<unsigned char> out = {0x89, 0x50, 0x4e, 0x47,
                                    0x0d, 0x0a, 0x1a, 0x0a};
  std::vector<unsigned char> ihdr;
  be32(ihdr, (unsigned)w);
  be32(ihdr, (unsigned)h);
  ihdr.push_back(8);
  ihdr.push_back(2);
  ihdr.push_back(0);
  ihdr.push_back(0);
  ihdr.push_back(0);
  chunk(out, "IHDR", ihdr);
  chunk(out, "IDAT", z);
  chunk(out, "IEND", {});
  return out;
}

// A 1x1 png, the smallest thing stb will decode.
const unsigned char kPng[] = {
    0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d,
    0x49, 0x48, 0x44, 0x52, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01,
    0x08, 0x06, 0x00, 0x00, 0x00, 0x1f, 0x15, 0xc4, 0x89, 0x00, 0x00, 0x00,
    0x0d, 0x49, 0x44, 0x41, 0x54, 0x78, 0x9c, 0x63, 0x60, 0x60, 0x60, 0x60,
    0x00, 0x00, 0x00, 0x05, 0x00, 0x01, 0x87, 0xa1, 0x5f, 0xd6, 0x00, 0x00,
    0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82};

TEST(AsyncTextureLoader, SubmitDoesNotWaitForTheFetch) {
  AsyncTextureLoader loader;
  const auto t0 = Clock::now();
  loader.submit(
      "slow",
      [](std::vector<unsigned char> *out) {
        std::this_thread::sleep_for(std::chrono::milliseconds(400));
        out->assign(kPng, kPng + sizeof(kPng));
        return true;
      },
      1.0, 1.0, 1.0);
  const long long submit_ms = ms_since(t0);
  EXPECT_TRUE(submit_ms < 100);

  // Nothing is ready yet, and asking must not block either.
  int drained = 0;
  loader.drain([&](const std::string &, const DecodedPixels &) { ++drained; });
  EXPECT_EQ(drained, 0);
  EXPECT_TRUE(ms_since(t0) < 100);

  for (int i = 0; i < 100 && drained == 0; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    loader.drain([&](const std::string &key, const DecodedPixels &px) {
      ++drained;
      EXPECT_EQ(key, "slow");
      EXPECT_TRUE(px.ok);
      EXPECT_EQ(px.width, 1);
      EXPECT_EQ(px.height, 1);
    });
  }
  EXPECT_EQ(drained, 1);
}

TEST(AsyncTextureLoader, FailedFetchComesBackUnusable) {
  AsyncTextureLoader loader;
  loader.submit(
      "gone", [](std::vector<unsigned char> *) { return false; }, 1.0, 1.0,
      1.0);

  bool seen = false;
  for (int i = 0; i < 100 && !seen; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    loader.drain([&](const std::string &key, const DecodedPixels &px) {
      seen = true;
      EXPECT_EQ(key, "gone");
      // The provider turns this into LoadStatus::Failed, which is what puts the
      // missing-file badge in the Layers panel.
      EXPECT_FALSE(px.ok);
    });
  }
  EXPECT_TRUE(seen);
}

TEST(AsyncTextureLoader, GarbageBytesDoNotDecode) {
  AsyncTextureLoader loader;
  loader.submit(
      "junk",
      [](std::vector<unsigned char> *out) {
        out->assign(64, 0x41);
        return true;
      },
      1.0, 1.0, 1.0);

  bool seen = false;
  for (int i = 0; i < 100 && !seen; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    loader.drain([&](const std::string &, const DecodedPixels &px) {
      seen = true;
      EXPECT_FALSE(px.ok);
    });
  }
  EXPECT_TRUE(seen);
}

// Dropping the loader with work still queued must not hang or crash.
TEST(AsyncTextureLoader, DestructionWithWorkInFlight) {
  {
    AsyncTextureLoader loader;
    for (int i = 0; i < 8; ++i) {
      loader.submit(
          "j" + std::to_string(i),
          [](std::vector<unsigned char> *out) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            out->assign(kPng, kPng + sizeof(kPng));
            return true;
          },
          1.0, 1.0, 1.0);
    }
  }
  SUCCEED() << "the destructor returned rather than hanging";
}

// The regression this whole file exists for: the render thread used to run the
// decode inline. Compares what a caller pays now against what the same decode
// costs synchronously.
TEST(AsyncTextureLoader, SubmitCostsFarLessThanDecoding) {
  // Big enough that a synchronous decode is measurable, the way a floorplan is.
  const int w = 3000, h = 2000;
  std::vector<unsigned char> png = make_png(w, h);
  EXPECT_FALSE(png.empty());

  const auto t_sync = Clock::now();
  const DecodedPixels px =
      imrmf::map_editor::canvas::decode_pixels(png.data(), png.size(), 1, 1, 1);
  const long long sync_us = us_since(t_sync);
  EXPECT_TRUE(px.ok);
  EXPECT_EQ(px.width, w);

  AsyncTextureLoader loader;
  const auto t_submit = Clock::now();
  loader.submit(
      "big",
      [&png](std::vector<unsigned char> *out) {
        *out = png;
        return true;
      },
      1.0, 1.0, 1.0);
  const long long submit_us = us_since(t_submit);

  std::printf("  decode %lld us on the calling thread, submit %lld us\n",
              sync_us, submit_us);
  // The old path paid the whole decode every first look at a floor. The new one
  // pays a queue push.
  EXPECT_TRUE(submit_us * 10 < sync_us);

  bool got = false;
  for (int i = 0; i < 200 && !got; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    loader.drain([&](const std::string &, const DecodedPixels &p) {
      got = true;
      EXPECT_TRUE(p.ok);
      EXPECT_EQ(p.width, w);
      EXPECT_EQ(p.height, h);
    });
  }
  EXPECT_TRUE(got);
}

} // namespace
