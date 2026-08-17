// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The ImRmfMapEditor Authors

#include "model/map_bundle.hpp"

#include "model/asset_paths.hpp"
#include "model/yaml_io.hpp"
#include "third_party/miniz/miniz.h"

#include "nlohmann/json.hpp"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <map>
#include <set>

namespace imrmf::map_editor {

namespace {

constexpr const char *kManifestEntry = "manifest.json";
constexpr const char *kAssetPrefix = "assets/";
constexpr const char *kExternalPrefix = "assets/_external/";

// Bounds on what we unpack from an untrusted file. A .rmfmap is something
// people hand each other, so opening one is an attack surface.
constexpr std::size_t kMaxEntries = 4096;
constexpr std::size_t kMaxTotalBytes = 512u * 1024u * 1024u;
constexpr std::size_t kMaxManifestAssets = 4096;
// Images are stored and yaml deflates maybe 10x, so a real bundle sits near 1.
// Well past that is a bomb, not a map.
constexpr std::size_t kMaxCompressionRatio = 200;

bool entry_name_is_safe(const std::string &name) {
  if (name.empty() || name.size() > 1024)
    return false;
  if (name.front() == '/' || name.front() == '\\')
    return false;
  if (name.find('\\') != std::string::npos)
    return false;
  // Windows drive letter.
  if (name.size() >= 2 && name[1] == ':')
    return false;
  return asset_path_is_portable(name);
}

std::string basename_of(const std::string &path) {
  const std::string name = std::filesystem::path(path).filename().string();
  return name.empty() ? std::string("asset") : name;
}

// A path that cannot be expressed relative to the map directory gets a flat
// entry. The counter keeps two files with the same basename apart.
std::string entry_for(const std::string &path, int *external_seq) {
  if (asset_path_is_portable(path))
    return kAssetPrefix + path;
  return kExternalPrefix + std::to_string(++*external_seq) + "_" +
         basename_of(path);
}

// miniz's writer owns a heap buffer we have to hand back with mz_free.
struct ZipWriter {
  mz_zip_archive zip{};
  bool inited = false;

  ZipWriter() {
    std::memset(&zip, 0, sizeof(zip));
    inited = mz_zip_writer_init_heap(&zip, 0, 64 * 1024);
  }
  ~ZipWriter() {
    if (inited)
      mz_zip_writer_end(&zip);
  }
  ZipWriter(const ZipWriter &) = delete;
  ZipWriter &operator=(const ZipWriter &) = delete;
};

struct ZipReader {
  mz_zip_archive zip{};
  bool inited = false;

  ZipReader(const unsigned char *data, std::size_t len) {
    std::memset(&zip, 0, sizeof(zip));
    inited = mz_zip_reader_init_mem(&zip, data, len, 0);
  }
  ~ZipReader() {
    if (inited)
      mz_zip_reader_end(&zip);
  }
  ZipReader(const ZipReader &) = delete;
  ZipReader &operator=(const ZipReader &) = delete;
};

void add_entry(ZipWriter &w, const std::string &name, const void *data,
               std::size_t len, mz_uint level) {
  if (!mz_zip_writer_add_mem(&w.zip, name.c_str(), data, len, level))
    throw BundleError("failed to add " + name + " to the archive");
}

std::vector<unsigned char> extract(ZipReader &r, mz_uint index,
                                   const std::string &name) {
  std::size_t size = 0;
  void *p = mz_zip_reader_extract_to_heap(&r.zip, index, &size, 0);
  if (!p)
    throw BundleError("failed to extract " + name);
  std::vector<unsigned char> out(static_cast<unsigned char *>(p),
                                 static_cast<unsigned char *>(p) + size);
  mz_free(p);
  return out;
}

} // namespace

MapBundle collect_bundle(const Building &building, std::string yaml_name,
                         const AssetReader &read) {
  MapBundle out;
  out.yaml_name = std::move(yaml_name);
  out.building = building;

  int external_seq = 0;
  std::set<std::string> seen;
  for (const std::string *ref : building_asset_refs(out.building)) {
    // The same image can back several layers, pack it once.
    if (!seen.insert(*ref).second)
      continue;

    std::vector<unsigned char> bytes;
    if (!read || !read(*ref, &bytes) || bytes.empty()) {
      out.missing.push_back(*ref);
      continue;
    }
    BundleAsset a;
    a.path = *ref;
    a.entry = entry_for(*ref, &external_seq);
    a.bytes = std::move(bytes);
    out.assets.push_back(std::move(a));
  }
  return out;
}

std::vector<unsigned char> write_bundle(const MapBundle &bundle) {
  if (bundle.yaml_name.empty())
    throw BundleError("bundle has no yaml name");
  if (!entry_name_is_safe(bundle.yaml_name))
    throw BundleError("unsafe yaml name: " + bundle.yaml_name);

  ZipWriter w;
  if (!w.inited)
    throw BundleError("could not open a zip writer");

  nlohmann::json manifest;
  manifest["format"] = kBundleFormat;
  manifest["version"] = kBundleVersion;
  manifest["building"] = bundle.yaml_name;
  manifest["assets"] = nlohmann::json::array();
  for (const BundleAsset &a : bundle.assets) {
    if (!entry_name_is_safe(a.entry))
      throw BundleError("unsafe asset entry: " + a.entry);
    manifest["assets"].push_back({{"path", a.path}, {"entry", a.entry}});
  }

  const std::string manifest_text = manifest.dump(2);
  add_entry(w, kManifestEntry, manifest_text.data(), manifest_text.size(),
            MZ_BEST_COMPRESSION);

  const std::string yaml = serialize_building(bundle.building);
  add_entry(w, bundle.yaml_name, yaml.data(), yaml.size(), MZ_BEST_COMPRESSION);

  // PNGs are already deflated, a second pass costs time and saves nothing.
  for (const BundleAsset &a : bundle.assets)
    add_entry(w, a.entry, a.bytes.data(), a.bytes.size(), MZ_NO_COMPRESSION);

  void *buf = nullptr;
  std::size_t size = 0;
  if (!mz_zip_writer_finalize_heap_archive(&w.zip, &buf, &size))
    throw BundleError("failed to finalize the archive");
  std::vector<unsigned char> out(static_cast<unsigned char *>(buf),
                                 static_cast<unsigned char *>(buf) + size);
  mz_free(buf);
  return out;
}

MapBundle read_bundle(const unsigned char *data, std::size_t len) {
  if (!data || len == 0)
    throw BundleError("empty file");

  ZipReader r(data, len);
  if (!r.inited)
    throw BundleError("not a zip archive");

  const mz_uint count = mz_zip_reader_get_num_files(&r.zip);
  if (count == 0)
    throw BundleError("archive is empty");
  if (count > kMaxEntries)
    throw BundleError("archive has too many entries");

  // Index by name so the manifest drives what we extract.
  std::vector<std::string> names(count);
  std::map<std::string, int> by_name;
  std::size_t total_uncompressed = 0;
  std::size_t total_compressed = 0;
  for (mz_uint i = 0; i < count; ++i) {
    mz_zip_archive_file_stat st;
    if (!mz_zip_reader_file_stat(&r.zip, i, &st))
      throw BundleError("corrupt archive directory");
    if (mz_zip_reader_is_file_a_directory(&r.zip, i))
      continue;
    const std::string name(st.m_filename);
    if (!entry_name_is_safe(name))
      throw BundleError("unsafe entry name: " + name);
    total_uncompressed += static_cast<std::size_t>(st.m_uncomp_size);
    total_compressed += static_cast<std::size_t>(st.m_comp_size);
    if (total_uncompressed > kMaxTotalBytes)
      throw BundleError("archive is too large to open");
    names[i] = name;
    // First entry of a given name wins, so a duplicate name cannot point a
    // later lookup somewhere the size accounting did not expect.
    by_name.emplace(name, static_cast<int>(i));
  }

  // Zip bomb: a few KB that inflate to gigabytes. The sizes above bound the
  // total, this bounds the amplification.
  if (total_compressed > 0 &&
      total_uncompressed / total_compressed > kMaxCompressionRatio)
    throw BundleError("archive expands too much to be a map bundle");

  auto index_of = [&](const std::string &name) -> int {
    auto it = by_name.find(name);
    return it == by_name.end() ? -1 : it->second;
  };

  const int manifest_idx = index_of(kManifestEntry);
  if (manifest_idx < 0)
    throw BundleError("no manifest.json, not a .rmfmap map bundle");

  const std::vector<unsigned char> manifest_bytes =
      extract(r, static_cast<mz_uint>(manifest_idx), kManifestEntry);
  nlohmann::json manifest =
      nlohmann::json::parse(manifest_bytes.begin(), manifest_bytes.end(),
                            /*cb=*/nullptr, /*allow_exceptions=*/false);
  if (manifest.is_discarded() || !manifest.is_object())
    throw BundleError("manifest.json is not valid JSON");
  if (manifest.value("format", std::string()) != kBundleFormat)
    throw BundleError("manifest.json is not an " + std::string(kBundleFormat) +
                      " manifest");
  const int version = manifest.value("version", 0);
  if (version <= 0 || version > kBundleVersion)
    throw BundleError("unsupported bundle version " + std::to_string(version));

  MapBundle out;
  out.yaml_name = manifest.value("building", std::string());
  if (out.yaml_name.empty() || !entry_name_is_safe(out.yaml_name))
    throw BundleError("manifest.json names no usable building yaml");

  const int yaml_idx = index_of(out.yaml_name);
  if (yaml_idx < 0)
    throw BundleError("archive has no " + out.yaml_name);
  const std::vector<unsigned char> yaml_bytes =
      extract(r, static_cast<mz_uint>(yaml_idx), out.yaml_name);
  try {
    out.building =
        parse_building(std::string(yaml_bytes.begin(), yaml_bytes.end()));
  } catch (const std::exception &e) {
    throw BundleError(std::string("bundled yaml failed to parse: ") + e.what());
  }

  if (manifest.contains("assets") && manifest["assets"].is_array()) {
    const auto &listed = manifest["assets"];
    if (listed.size() > kMaxManifestAssets)
      throw BundleError("manifest.json lists too many assets");

    // The cap above counts each archive entry once, but the manifest can name
    // the same entry repeatedly. Without this, one large entry listed many
    // times extracts a fresh copy per mention while every check still passes.
    std::set<std::string> extracted_entries;
    std::size_t extracted_bytes = 0;

    for (const auto &a : listed) {
      if (!a.is_object())
        continue;
      BundleAsset asset;
      asset.path = a.value("path", std::string());
      asset.entry = a.value("entry", std::string());
      if (asset.path.empty() || asset.entry.empty())
        continue;
      if (!entry_name_is_safe(asset.entry))
        throw BundleError("unsafe asset entry: " + asset.entry);
      if (!extracted_entries.insert(asset.entry).second)
        continue; // already unpacked under another path
      const int idx = index_of(asset.entry);
      if (idx < 0) {
        out.missing.push_back(asset.path);
        continue;
      }
      asset.bytes = extract(r, static_cast<mz_uint>(idx), asset.entry);
      extracted_bytes += asset.bytes.size();
      if (extracted_bytes > kMaxTotalBytes)
        throw BundleError("archive is too large to open");
      out.assets.push_back(std::move(asset));
    }
  }

  // Anything the yaml points at that the manifest never listed.
  for (const std::string *ref : building_asset_refs(out.building)) {
    const bool packed =
        std::any_of(out.assets.begin(), out.assets.end(),
                    [&](const BundleAsset &a) { return a.path == *ref; });
    const bool known = std::find(out.missing.begin(), out.missing.end(),
                                 *ref) != out.missing.end();
    if (!packed && !known)
      out.missing.push_back(*ref);
  }
  return out;
}

const std::vector<unsigned char> *find_asset(const MapBundle &bundle,
                                             const std::string &path) {
  for (const BundleAsset &a : bundle.assets)
    if (a.path == path)
      return &a.bytes;
  return nullptr;
}

} // namespace imrmf::map_editor
