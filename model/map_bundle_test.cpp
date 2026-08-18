// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The ImRmfMapEditor Authors
//
// Round-trip and hostile-input tests for the .rmfmap bundle. Same no-gtest
// style as yaml_io_test.cpp.

#include "model/asset_paths.hpp"
#include "model/map_bundle.hpp"
#include "model/yaml_io.hpp"

#include "third_party/miniz/miniz.h"

#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <tuple>

using namespace imrmf::map_editor;

namespace {

int g_failures = 0;

#define CHECK(cond)                                                            \
  do {                                                                         \
    if (!(cond)) {                                                             \
      std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);              \
      ++g_failures;                                                            \
    }                                                                          \
  } while (0)

const char *kYaml = R"(name: bundle_test
coordinate_system: reference_image
levels:
  L1:
    elevation: 0
    drawing:
      filename: floor.png
    vertices:
      - [0, 0, 0, ""]
      - [100, 0, 0, ""]
    lanes:
      - [0, 1, {bidirectional: [4, false]}]
    measurements:
      - [0, 1, {distance: [3, 5]}]
    layers:
      office:
        filename: layers/office.png
        visible: true
        color: [1, 0, 0, 0.5]
        transform: {scale: 0.01, yaw: 0, translation_x: 1, translation_y: 2}
      roof:
        filename: layers/roof.png
        visible: false
        color: [0, 1, 0, 0.5]
        transform: {scale: 0.02, yaw: 0.5, translation_x: 0, translation_y: 0}
)";

std::vector<unsigned char> blob(const std::string &s) {
  return std::vector<unsigned char>(s.begin(), s.end());
}

// Stands in for the filesystem or the texture provider.
AssetReader reader_over(const std::map<std::string, std::string> &files) {
  return [&files](const std::string &path, std::vector<unsigned char> *out) {
    auto it = files.find(path);
    if (it == files.end())
      return false;
    *out = blob(it->second);
    return true;
  };
}

void test_roundtrip() {
  Building b = parse_building(kYaml);
  const std::map<std::string, std::string> files = {
      {"floor.png", "FLOORPLANBYTES"},
      {"layers/office.png", "OFFICEBYTES"},
      {"layers/roof.png", "ROOFBYTES"},
  };

  MapBundle packed =
      collect_bundle(b, "bundle_test.building.yaml", reader_over(files));
  CHECK(packed.assets.size() == 3);
  CHECK(packed.missing.empty());

  std::vector<unsigned char> zip = write_bundle(packed);
  CHECK(zip.size() > 4);
  // Local file header magic, it really is a zip.
  CHECK(zip[0] == 'P' && zip[1] == 'K' && zip[2] == 3 && zip[3] == 4);

  MapBundle got = read_bundle(zip.data(), zip.size());
  CHECK(got.yaml_name == "bundle_test.building.yaml");
  CHECK(got.missing.empty());
  CHECK(got.building.name == "bundle_test");
  CHECK(got.building.levels.size() == 1);
  CHECK(got.building.levels[0].layers.size() == 2);
  CHECK(got.building.levels[0].drawing_filename == "floor.png");
  CHECK(got.building.levels[0].lanes.size() == 1);

  for (const auto &[path, want] : files) {
    const std::vector<unsigned char> *bytes = find_asset(got, path);
    CHECK(bytes != nullptr);
    if (bytes)
      CHECK(std::string(bytes->begin(), bytes->end()) == want);
  }

  // The yaml inside the bundle is the ordinary serialization, unchanged.
  CHECK(serialize_building(got.building) == serialize_building(b));
}

void test_missing_asset_is_reported_not_fatal() {
  Building b = parse_building(kYaml);
  const std::map<std::string, std::string> files = {
      {"floor.png", "FLOORPLANBYTES"},
  };
  MapBundle packed = collect_bundle(b, "m.building.yaml", reader_over(files));
  CHECK(packed.assets.size() == 1);
  CHECK(packed.missing.size() == 2);

  std::vector<unsigned char> zip = write_bundle(packed);
  MapBundle got = read_bundle(zip.data(), zip.size());
  CHECK(got.assets.size() == 1);
  // The yaml still references them, so reading flags them again.
  CHECK(got.missing.size() == 2);
}

void test_absolute_path_goes_external() {
  Building b = parse_building(kYaml);
  b.levels[0].layers[0].filename = "/etc/somewhere/office.png";
  const std::map<std::string, std::string> files = {
      {"floor.png", "F"},
      {"/etc/somewhere/office.png", "OFFICE"},
      {"layers/roof.png", "R"},
  };
  MapBundle packed = collect_bundle(b, "m.building.yaml", reader_over(files));

  bool found_external = false;
  for (const BundleAsset &a : packed.assets) {
    if (a.path == "/etc/somewhere/office.png") {
      found_external = true;
      CHECK(a.entry.rfind("_external/", 0) == 0);
      CHECK(a.entry.find("office.png") != std::string::npos);
    }
  }
  CHECK(found_external);

  std::vector<unsigned char> zip = write_bundle(packed);
  MapBundle got = read_bundle(zip.data(), zip.size());
  // The yaml keeps the original string, and the bytes still come back by it.
  CHECK(got.building.levels[0].layers[0].filename ==
        "/etc/somewhere/office.png");
  const std::vector<unsigned char> *bytes =
      find_asset(got, "/etc/somewhere/office.png");
  CHECK(bytes != nullptr);
  if (bytes)
    CHECK(std::string(bytes->begin(), bytes->end()) == "OFFICE");
}

void test_duplicate_reference_packed_once() {
  Building b = parse_building(kYaml);
  b.levels[0].layers[1].filename = b.levels[0].layers[0].filename;
  const std::map<std::string, std::string> files = {
      {"floor.png", "F"},
      {"layers/office.png", "OFFICE"},
  };
  MapBundle packed = collect_bundle(b, "m.building.yaml", reader_over(files));
  CHECK(packed.assets.size() == 2);
  CHECK(packed.missing.empty());
}

void test_rejects_garbage() {
  const std::string junk = "not a zip at all, not even close";
  bool threw = false;
  try {
    read_bundle(reinterpret_cast<const unsigned char *>(junk.data()),
                junk.size());
  } catch (const BundleError &) {
    threw = true;
  }
  CHECK(threw);

  threw = false;
  try {
    read_bundle(nullptr, 0);
  } catch (const BundleError &) {
    threw = true;
  }
  CHECK(threw);
}

void test_rejects_truncated() {
  Building b = parse_building(kYaml);
  const std::map<std::string, std::string> files = {{"floor.png", "F"}};
  std::vector<unsigned char> zip =
      write_bundle(collect_bundle(b, "m.building.yaml", reader_over(files)));
  zip.resize(zip.size() / 2);
  bool threw = false;
  try {
    read_bundle(zip.data(), zip.size());
  } catch (const BundleError &) {
    threw = true;
  }
  CHECK(threw);
}

// The layout is the point: an image sits where the yaml says it does, which is
// where the storage backend keeps it too.
void test_entries_mirror_the_backend_layout() {
  Building b = parse_building(kYaml);
  const std::map<std::string, std::string> files = {
      {"floor.png", "F"},
      {"layers/office.png", "O"},
      {"layers/roof.png", "R"},
  };
  MapBundle packed = collect_bundle(b, "m.building.yaml", reader_over(files));
  for (const BundleAsset &a : packed.assets)
    CHECK(a.entry == a.path);

  const std::vector<unsigned char> zip = write_bundle(packed);
  mz_zip_archive z{};
  std::memset(&z, 0, sizeof(z));
  CHECK(mz_zip_reader_init_mem(&z, zip.data(), zip.size(), 0));
  std::set<std::string> names;
  for (mz_uint i = 0; i < mz_zip_reader_get_num_files(&z); ++i) {
    mz_zip_archive_file_stat st;
    if (mz_zip_reader_file_stat(&z, i, &st))
      names.insert(st.m_filename);
  }
  mz_zip_reader_end(&z);
  CHECK(names.count("manifest.json") == 1);
  CHECK(names.count("m.building.yaml") == 1);
  CHECK(names.count("layers/office.png") == 1);
  CHECK(names.count("floor.png") == 1);
  for (const std::string &n : names)
    CHECK(n.rfind("assets/", 0) != 0);
}

// Hand-built archives, since write_bundle only produces well-formed ones and
// these attacks are about malformed input.
std::vector<unsigned char> build_zip(
    const std::vector<std::tuple<std::string, std::string, bool>> &entries) {
  mz_zip_archive zip{};
  std::memset(&zip, 0, sizeof(zip));
  if (!mz_zip_writer_init_heap(&zip, 0, 64 * 1024))
    return {};
  for (const auto &[name, body, compress] : entries) {
    mz_zip_writer_add_mem(&zip, name.c_str(), body.data(), body.size(),
                          compress ? MZ_BEST_COMPRESSION : MZ_NO_COMPRESSION);
  }
  void *buf = nullptr;
  std::size_t size = 0;
  std::vector<unsigned char> out;
  if (mz_zip_writer_finalize_heap_archive(&zip, &buf, &size)) {
    out.assign(static_cast<unsigned char *>(buf),
               static_cast<unsigned char *>(buf) + size);
    mz_free(buf);
  }
  mz_zip_writer_end(&zip);
  return out;
}

bool rejects(const std::vector<unsigned char> &zip) {
  try {
    read_bundle(zip.data(), zip.size());
    return false;
  } catch (const BundleError &) {
    return true;
  }
}

// A manifest that is present but unreadable is an error. Absent is not, that is
// the plain-folder case.
void test_rejects_broken_manifest() {
  CHECK(rejects(build_zip({{"manifest.json", "{ not json", true},
                           {"m.building.yaml", kYaml, true}})));
  CHECK(rejects(build_zip({{"manifest.json",
                            R"({"format":"something-else",)"
                            R"("version":1,)"
                            R"("building":"m.building.yaml"})",
                            true},
                           {"m.building.yaml", kYaml, true}})));
  CHECK(rejects(build_zip({{"manifest.json",
                            R"({"format":"rmfmap","version":99,)"
                            R"("building":"m.building.yaml"})",
                            true},
                           {"m.building.yaml", kYaml, true}})));
}

// A few KB that inflate to many MB. The per-entry cap alone would not catch
// this ratio until after it had been unpacked.
void test_rejects_zip_bomb() {
  const std::string bomb(48u * 1024u * 1024u, '\0');
  const std::string manifest =
      R"({"format":"rmfmap","version":1,"building":"m.building.yaml",)"
      R"("assets":[{"path":"floor.png","entry":"assets/floor.png"}]})";
  CHECK(rejects(build_zip({{"manifest.json", manifest, true},
                           {"m.building.yaml", kYaml, true},
                           {"assets/floor.png", bomb, true}})));
}

// One large entry named by many manifest rows. Each row used to extract its own
// copy, so memory blew up while every individual check still passed.
void test_duplicate_manifest_entries_extract_once() {
  const std::string big(4u * 1024u * 1024u, 'F');
  std::string assets;
  for (int i = 0; i < 500; ++i) {
    if (i)
      assets += ",";
    assets += R"({"path":"copy)" + std::to_string(i) +
              R"(.png","entry":"assets/floor.png"})";
  }
  const std::string manifest =
      R"({"format":"rmfmap","version":1,"building":"m.building.yaml",)"
      R"("assets":[)" +
      assets + "]}";
  const std::vector<unsigned char> zip =
      build_zip({{"manifest.json", manifest, true},
                 {"m.building.yaml", kYaml, true},
                 {"assets/floor.png", big, false}});
  MapBundle got = read_bundle(zip.data(), zip.size());
  std::size_t total = 0;
  for (const BundleAsset &a : got.assets)
    total += a.bytes.size();
  // 500 mentions, one entry, unpacked once.
  CHECK(got.assets.size() == 1);
  CHECK(total == big.size());
}

// An unbounded asset list is a CPU cost before a single byte is unpacked.
void test_rejects_oversized_manifest() {
  std::string assets;
  for (int i = 0; i < 5000; ++i) {
    if (i)
      assets += ",";
    assets += R"({"path":"p)" + std::to_string(i) + R"(.png","entry":"a.png"})";
  }
  const std::string manifest =
      R"({"format":"rmfmap","version":1,"building":"m.building.yaml",)"
      R"("assets":[)" +
      assets + "]}";
  CHECK(rejects(build_zip(
      {{"manifest.json", manifest, true}, {"m.building.yaml", kYaml, true}})));
}

// Version 1 wrote images under assets/. The manifest says where each one lives,
// so those bundles keep opening.
void test_version_1_layout_still_opens() {
  const std::string manifest =
      R"({"format":"rmfmap","version":1,"building":"m.building.yaml",)"
      R"("assets":[{"path":"floor.png","entry":"assets/floor.png"},)"
      R"({"path":"layers/office.png","entry":"assets/layers/office.png"}]})";
  const std::vector<unsigned char> zip =
      build_zip({{"manifest.json", manifest, true},
                 {"m.building.yaml", kYaml, true},
                 {"assets/floor.png", "OLDFLOOR", false},
                 {"assets/layers/office.png", "OLDOFFICE", false}});
  MapBundle got = read_bundle(zip.data(), zip.size());
  CHECK(got.assets.size() == 2);
  const std::vector<unsigned char> *floor = find_asset(got, "floor.png");
  CHECK(floor != nullptr);
  if (floor)
    CHECK(std::string(floor->begin(), floor->end()) == "OLDFLOOR");
  const std::vector<unsigned char> *office =
      find_asset(got, "layers/office.png");
  CHECK(office != nullptr);
  if (office)
    CHECK(std::string(office->begin(), office->end()) == "OLDOFFICE");
}

// A building folder straight out of the bucket, zipped, with no manifest.
void test_plain_building_folder_opens() {
  const std::vector<unsigned char> zip =
      build_zip({{"m.building.yaml", kYaml, true},
                 {"floor.png", "FLOOR", false},
                 {"layers/office.png", "OFFICE", false},
                 {"layers/roof.png", "ROOF", false},
                 {"notes.txt", "ignored", true}});
  MapBundle got = read_bundle(zip.data(), zip.size());
  CHECK(got.yaml_name == "m.building.yaml");
  CHECK(got.building.name == "bundle_test");
  CHECK(got.assets.size() == 3);
  CHECK(got.missing.empty());
  const std::vector<unsigned char> *office =
      find_asset(got, "layers/office.png");
  CHECK(office != nullptr);
  if (office)
    CHECK(std::string(office->begin(), office->end()) == "OFFICE");
  // A file the map never mentions is left where it is.
  CHECK(find_asset(got, "notes.txt") == nullptr);
}

void test_plain_folder_needs_exactly_one_yaml() {
  CHECK(rejects(build_zip({{"floor.png", "F", false}})));
  CHECK(rejects(build_zip(
      {{"a.building.yaml", kYaml, true}, {"b.building.yaml", kYaml, true}})));
  // One at the root and one deeper is not ambiguous, the root wins.
  const std::vector<unsigned char> nested =
      build_zip({{"m.building.yaml", kYaml, true},
                 {"snapshots/old/m.building.yaml", kYaml, true},
                 {"floor.png", "F", false}});
  MapBundle got = read_bundle(nested.data(), nested.size());
  CHECK(got.yaml_name == "m.building.yaml");
}

// A missing image in a folder zip is reported the same way a bundle's is.
void test_plain_folder_reports_missing() {
  const std::vector<unsigned char> zip =
      build_zip({{"m.building.yaml", kYaml, true}, {"floor.png", "F", false}});
  MapBundle got = read_bundle(zip.data(), zip.size());
  CHECK(got.assets.size() == 1);
  CHECK(got.missing.size() == 2);
}

// A map that names an image inside _external/ must not have a flattened entry
// dropped on top of it.
void test_external_namespace_is_reserved() {
  Building b = parse_building(kYaml);
  b.levels[0].drawing_filename = "_external/1_office.png";
  b.levels[0].layers[0].filename = "/etc/somewhere/office.png";
  const std::map<std::string, std::string> files = {
      {"_external/1_office.png", "MINE"},
      {"/etc/somewhere/office.png", "THEIRS"},
      {"layers/roof.png", "R"},
  };
  MapBundle packed = collect_bundle(b, "m.building.yaml", reader_over(files));
  std::set<std::string> entries;
  for (const BundleAsset &a : packed.assets)
    CHECK(entries.insert(a.entry).second);

  const std::vector<unsigned char> zip = write_bundle(packed);
  MapBundle got = read_bundle(zip.data(), zip.size());
  const std::vector<unsigned char> *mine =
      find_asset(got, "_external/1_office.png");
  const std::vector<unsigned char> *theirs =
      find_asset(got, "/etc/somewhere/office.png");
  CHECK(mine != nullptr);
  CHECK(theirs != nullptr);
  if (mine)
    CHECK(std::string(mine->begin(), mine->end()) == "MINE");
  if (theirs)
    CHECK(std::string(theirs->begin(), theirs->end()) == "THEIRS");
}

// One image shared by many layers is unpacked once, in a folder zip just as in
// a manifest one. Without that, the archive-level size cap counts the entry
// once while extraction pays for every mention.
void test_plain_folder_shares_one_image() {
  std::string yaml =
      "name: shared\ncoordinate_system: reference_image\nlevels:\n"
      "  L1:\n    elevation: 0\n    drawing:\n"
      "      filename: layers/L1/shared.png\n    layers:\n";
  for (int i = 0; i < 200; ++i) {
    yaml += "      l" + std::to_string(i) +
            ":\n        filename: layers/L1/shared.png\n"
            "        visible: true\n        color: [1, 0, 0, 0.5]\n"
            "        transform: {scale: 0.01, yaw: 0, translation_x: 0, "
            "translation_y: 0}\n";
  }
  const std::string big(2u * 1024u * 1024u, 'S');
  const std::vector<unsigned char> zip = build_zip(
      {{"m.building.yaml", yaml, true}, {"layers/L1/shared.png", big, false}});
  MapBundle got = read_bundle(zip.data(), zip.size());
  CHECK(got.assets.size() == 1);
  std::size_t total = 0;
  for (const BundleAsset &a : got.assets)
    total += a.bytes.size();
  CHECK(total == big.size());
}

// Traversal never reaches the filesystem (bundles stay in memory), but the name
// check is all that stands between us and it, so pin it.
void test_rejects_traversal_entry() {
  const std::string manifest =
      R"({"format":"rmfmap","version":1,"building":"m.building.yaml"})";
  CHECK(rejects(build_zip({{"manifest.json", manifest, true},
                           {"m.building.yaml", kYaml, true},
                           {"../../etc/passwd", "x", false}})));
  // miniz's writer normalises a leading slash away, so name it with a
  // placeholder and patch it back to '/' in the raw bytes. Same length, so
  // every offset in the archive stays valid.
  std::vector<unsigned char> abs_zip =
      build_zip({{"manifest.json", manifest, true},
                 {"m.building.yaml", kYaml, true},
                 {"Xetc/passwd", "x", false}});
  const std::string needle = "Xetc/passwd";
  int patched = 0;
  for (std::size_t i = 0; i + needle.size() <= abs_zip.size(); ++i) {
    if (std::memcmp(abs_zip.data() + i, needle.data(), needle.size()) == 0) {
      abs_zip[i] = '/';
      ++patched;
    }
  }
  CHECK(patched > 0);
  CHECK(rejects(abs_zip));
}

void test_path_helpers() {
  CHECK(asset_path_is_portable("floor.png"));
  CHECK(asset_path_is_portable("layers/office.png"));
  CHECK(asset_path_is_portable("a/../b.png"));
  CHECK(!asset_path_is_portable("/abs/floor.png"));
  CHECK(!asset_path_is_portable("../outside.png"));
  CHECK(!asset_path_is_portable("a/../../outside.png"));

  const std::filesystem::path base = "/maps/office";
  CHECK(relativize_asset_path("/maps/office/floor.png", base) == "floor.png");
  CHECK(relativize_asset_path("/maps/office/layers/a.png", base) ==
        "layers/a.png");
  CHECK(relativize_asset_path("./floor.png", base) == "floor.png");
  CHECK(relativize_asset_path("layers/a.png", base) == "layers/a.png");
  // Outside the map directory: left as-is rather than turned into "../".
  CHECK(relativize_asset_path("/elsewhere/a.png", base) == "/elsewhere/a.png");
  CHECK(relativize_asset_path("", base).empty());
}

void test_asset_refs_skip_empties() {
  Building b = parse_building(kYaml);
  CHECK(building_asset_refs(b).size() == 3);
  b.levels[0].drawing_filename.clear();
  CHECK(building_asset_refs(b).size() == 2);
}

} // namespace

int main() {
  test_roundtrip();
  test_missing_asset_is_reported_not_fatal();
  test_absolute_path_goes_external();
  test_duplicate_reference_packed_once();
  test_rejects_garbage();
  test_rejects_truncated();
  test_rejects_broken_manifest();
  test_duplicate_manifest_entries_extract_once();
  test_rejects_zip_bomb();
  test_rejects_oversized_manifest();
  test_rejects_traversal_entry();
  test_entries_mirror_the_backend_layout();
  test_version_1_layout_still_opens();
  test_plain_building_folder_opens();
  test_plain_folder_needs_exactly_one_yaml();
  test_plain_folder_reports_missing();
  test_plain_folder_shares_one_image();
  test_external_namespace_is_reserved();
  test_path_helpers();
  test_asset_refs_skip_empties();
  if (g_failures == 0) {
    std::printf("ALL TESTS PASSED\n");
    return 0;
  }
  std::printf("%d CHECK(s) FAILED\n", g_failures);
  return 1;
}
