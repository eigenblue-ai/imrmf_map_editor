#pragma once

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <string>

// Value-to-string formatters every tool was rewriting. No ImGui dependency.
namespace ui {

// 1536 -> "1.5 KB". Binary units, one decimal.
inline std::string human_size(std::uint64_t bytes) {
  const char *units[] = {"B", "KB", "MB", "GB", "TB"};
  double v = static_cast<double>(bytes);
  int u = 0;
  while (v >= 1024.0 && u < 4) {
    v /= 1024.0;
    ++u;
  }
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%.1f %s", v, units[u]);
  return buf;
}

// 83.0 -> "1m 23s". Zero or NaN -> "-".
inline std::string format_duration(double seconds) {
  if (seconds == 0.0 || std::isnan(seconds))
    return "-";
  const int total = static_cast<int>(seconds);
  const int mins = total / 60;
  const int secs = total % 60;
  char buf[32];
  if (mins > 0)
    std::snprintf(buf, sizeof(buf), "%dm %ds", mins, secs);
  else
    std::snprintf(buf, sizeof(buf), "%ds", secs);
  return buf;
}

// Unix seconds -> "2026-08-05 17:03" local time. Non-positive -> "-".
inline std::string format_time(std::int64_t unix_s) {
  if (unix_s <= 0)
    return "-";
  std::time_t t = static_cast<std::time_t>(unix_s);
  std::tm tm{};
  localtime_r(&t, &tm);
  char buf[32];
  std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", &tm);
  return buf;
}

// Unix milliseconds -> "2026-08-05 17:03:12" local time. Non-positive -> "-".
inline std::string format_timestamp_ms(std::int64_t unix_ms) {
  if (unix_ms <= 0)
    return "-";
  std::time_t t = static_cast<std::time_t>(unix_ms / 1000);
  std::tm tm{};
  localtime_r(&t, &tm);
  char buf[64];
  std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm);
  return buf;
}

// Long id (UUID etc.) truncated for display.
inline std::string short_id(const std::string &id, std::size_t n = 12) {
  return id.size() > n ? id.substr(0, n) : id;
}

} // namespace ui
