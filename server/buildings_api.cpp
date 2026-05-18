// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The ImRmfMapEditor Authors

#include "server/buildings_api.hpp"

#include "model/yaml_io.hpp"

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <unistd.h>

namespace imrmf::map_editor {

namespace {

std::string fnv1a_hex(const std::string &data) {
  uint64_t h = 14695981039346656037ULL;
  for (unsigned char c : data) {
    h ^= c;
    h *= 1099511628211ULL;
  }
  char buf[17];
  std::snprintf(buf, sizeof(buf), "%016lx", (unsigned long)h);
  return std::string(buf);
}

std::string slurp(const std::filesystem::path &p) {
  std::ifstream f(p, std::ios::binary);
  std::ostringstream oss;
  oss << f.rdbuf();
  return oss.str();
}

void atomic_write(const std::filesystem::path &p, const std::string &data) {
  std::filesystem::path tmp = p;
  tmp += ".tmp";
  {
    std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
    f.write(data.data(), (std::streamsize)data.size());
  }
  std::filesystem::rename(tmp, p);
}

std::string url_decode(const std::string &s) {
  std::string out;
  out.reserve(s.size());
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '%' && i + 2 < s.size()) {
      auto h = [](char c) {
        if (c >= '0' && c <= '9')
          return c - '0';
        if (c >= 'a' && c <= 'f')
          return c - 'a' + 10;
        if (c >= 'A' && c <= 'F')
          return c - 'A' + 10;
        return 0;
      };
      out += (char)((h(s[i + 1]) << 4) | h(s[i + 2]));
      i += 2;
    } else if (s[i] == '+') {
      out += ' ';
    } else {
      out += s[i];
    }
  }
  return out;
}

bool building_id_safe(const std::string &id) {
  if (id.empty() || id.size() > 64)
    return false;
  for (char c : id) {
    if (!(std::isalnum((unsigned char)c) || c == '_' || c == '-'))
      return false;
  }
  return true;
}

std::filesystem::path building_path(const std::filesystem::path &maps_root,
                                    const std::string &id) {
  return maps_root / id / (id + ".building.yaml");
}

} // namespace

std::string
http_response(int status, const std::string &status_text,
              const std::vector<std::pair<std::string, std::string>> &headers,
              const std::string &body) {
  std::ostringstream oss;
  oss << "HTTP/1.1 " << status << " " << status_text << "\r\n";
  oss << "Access-Control-Allow-Origin: *\r\n";
  oss << "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n";
  oss << "Access-Control-Allow-Headers: If-Match, Content-Type\r\n";
  oss << "Access-Control-Expose-Headers: ETag\r\n";
  for (const auto &[k, v] : headers)
    oss << k << ": " << v << "\r\n";
  oss << "Content-Length: " << body.size() << "\r\n\r\n";
  oss << body;
  return oss.str();
}

std::string header_value(const std::string &headers, const std::string &name) {
  std::string lower(headers.size(), '\0');
  for (size_t i = 0; i < headers.size(); ++i)
    lower[i] = (char)std::tolower((unsigned char)headers[i]);
  std::string needle = name;
  for (auto &c : needle)
    c = (char)std::tolower((unsigned char)c);
  size_t pos = 0;
  while (pos < lower.size()) {
    size_t found = lower.find(needle, pos);
    if (found == std::string::npos)
      return {};
    bool at_line_start = (found == 0 || lower[found - 1] == '\n');
    size_t after = found + needle.size();
    if (at_line_start && after < lower.size() && lower[after] == ':') {
      size_t v = after + 1;
      while (v < headers.size() && (headers[v] == ' ' || headers[v] == '\t'))
        ++v;
      size_t eol = headers.find('\n', v);
      std::string val = headers.substr(
          v, eol == std::string::npos ? std::string::npos : eol - v);
      while (!val.empty() &&
             (val.back() == '\r' || val.back() == ' ' || val.back() == '\t'))
        val.pop_back();
      return val;
    }
    pos = after + 1;
  }
  return {};
}

bool read_until(int fd, std::string &accum, size_t target) {
  constexpr size_t chunk = 8192;
  char buf[chunk];
  while (accum.size() < target) {
    ssize_t n = read(fd, buf, sizeof(buf));
    if (n <= 0)
      return false;
    accum.append(buf, (size_t)n);
  }
  return true;
}

std::string handle_building_get(const std::filesystem::path &maps_root,
                                const std::string &id) {
  if (!building_id_safe(id)) {
    return http_response(400, "Bad Request", {}, "invalid building id");
  }
  auto path = building_path(maps_root, id);
  if (!std::filesystem::exists(path)) {
    return http_response(404, "Not Found", {}, "no such building");
  }
  std::string body = slurp(path);
  std::string etag = "\"" + fnv1a_hex(body) + "\"";
  return http_response(200, "OK",
                       {{"Content-Type", "text/yaml"}, {"ETag", etag}}, body);
}

std::string handle_building_post(const std::filesystem::path &maps_root,
                                 const std::string &id,
                                 const std::string &if_match,
                                 const std::string &body) {
  if (!building_id_safe(id)) {
    return http_response(400, "Bad Request", {}, "invalid building id");
  }
  auto path = building_path(maps_root, id);
  if (!std::filesystem::exists(path)) {
    return http_response(404, "Not Found", {}, "no such building");
  }
  std::string current = slurp(path);
  std::string current_etag = "\"" + fnv1a_hex(current) + "\"";
  if (!if_match.empty() && if_match != current_etag) {
    return http_response(409, "Conflict", {{"ETag", current_etag}},
                         "stale etag");
  }
  try {
    (void)imrmf::map_editor::parse_building(body);
  } catch (const std::exception &e) {
    return http_response(400, "Bad Request", {},
                         std::string("yaml parse failed: ") + e.what());
  }
  atomic_write(path, body);
  std::string new_etag = "\"" + fnv1a_hex(body) + "\"";
  return http_response(200, "OK", {{"ETag", new_etag}}, "");
}

bool is_buildings_request(const std::string &request) {
  return request.compare(0, 19, "OPTIONS /buildings/") == 0 ||
         request.compare(0, 15, "GET /buildings/") == 0 ||
         request.compare(0, 16, "POST /buildings/") == 0;
}

std::string dispatch_buildings_request(int fd,
                                       const std::string &initial_request,
                                       const std::filesystem::path &maps_root) {
  if (!is_buildings_request(initial_request))
    return {};

  std::string request = initial_request;
  bool is_options = request.compare(0, 19, "OPTIONS /buildings/") == 0;
  bool is_get = request.compare(0, 15, "GET /buildings/") == 0;
  bool is_post = request.compare(0, 16, "POST /buildings/") == 0;

  size_t sp1 = request.find(' ');
  size_t sp2 = request.find(' ', sp1 + 1);
  std::string id;
  if (sp1 != std::string::npos && sp2 != std::string::npos) {
    std::string path_with_query = request.substr(sp1 + 1, sp2 - sp1 - 1);
    const std::string prefix = "/buildings/";
    if (path_with_query.compare(0, prefix.size(), prefix) == 0) {
      std::string rest = path_with_query.substr(prefix.size());
      size_t qm = rest.find('?');
      if (qm != std::string::npos)
        rest = rest.substr(0, qm);
      id = url_decode(rest);
    }
  }

  if (is_options)
    return http_response(204, "No Content", {}, "");
  if (is_get)
    return handle_building_get(maps_root, id);
  if (!is_post)
    return {};

  size_t header_end = request.find("\r\n\r\n");
  std::string headers_only =
      header_end == std::string::npos ? request : request.substr(0, header_end);
  std::string content_length_str = header_value(headers_only, "Content-Length");
  size_t content_length =
      content_length_str.empty() ? 0 : (size_t)std::stoul(content_length_str);
  std::string if_match = header_value(headers_only, "If-Match");
  size_t body_start =
      header_end == std::string::npos ? request.size() : header_end + 4;
  std::string body = request.substr(body_start);
  if (content_length > body.size()) {
    if (!read_until(fd, body, content_length)) {
      return http_response(400, "Bad Request", {}, "short body");
    }
  }
  if (body.size() > content_length)
    body.resize(content_length);
  return handle_building_post(maps_root, id, if_match, body);
}

} // namespace imrmf::map_editor
