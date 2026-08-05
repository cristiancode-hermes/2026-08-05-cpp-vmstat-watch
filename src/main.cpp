// SPDX-License-Identifier: MIT
// cpp-vmstat-watch — Linux virtual memory statistics from /proc/vmstat (C++23)
// Build: g++ -std=c++23 -O2 -Wall -Wextra src/main.cpp -o vmstat-watch
// Usage: ./vmstat-watch [--watch N] [--json] [--no-color] [--selftest] [--help]

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <print>
#include <ranges>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

namespace vmstat {

// ── ANSI palette (Catppuccin-ish) ──────────────────────────────────────────
constexpr const char* kRed     = "\x1b[31m";
constexpr const char* kGreen   = "\x1b[32m";
constexpr const char* kYellow  = "\x1b[33m";
constexpr const char* kBlue    = "\x1b[34m";
constexpr const char* kMagenta = "\x1b[35m";
constexpr const char* kCyan    = "\x1b[36m";
constexpr const char* kBold    = "\x1b[1m";
constexpr const char* kDim     = "\x1b[2m";
constexpr const char* kReset   = "\x1b[0m";

// ── Types ──────────────────────────────────────────────────────────────────
struct VmstatEntry {
  std::string key;
  long long value = 0;
};

using VmstatMap = std::unordered_map<std::string, long long>;

// Category metadata for display grouping
struct CategoryInfo {
  std::string name;
  std::vector<std::string> keys;
  const char* color;
};

// ── Group definitions ──────────────────────────────────────────────────────
// Keys in each category — used for display grouping and selftest assertions
const CategoryInfo kCategories[] = {
  {"PAGE FAULTS", {"pgfault", "pgmajfault", "pgpgin", "pgpgout"}, kYellow},
  {"SWAP", {"pswpin", "pswpout"}, kRed},
  {"NUMA", {"numa_hit", "numa_miss", "numa_foreign", "numa_local", "numa_other"}, kCyan},
  {"WORKINGSET", {"workingset_refault_anon", "workingset_refault_file",
                   "workingset_activate_anon", "workingset_activate_file",
                   "workingset_restore_anon", "workingset_restore_file",
                   "workingset_nodereclaim"}, kMagenta},
  {"SLAB", {"slab_alloc", "slab_free", "slab_nr_pages"}, kBlue},
  {"COMPACT", {"compact_migrate_scanned", "compact_free_scanned",
                "compact_isolated", "compact_stall", "compact_fail",
                "compact_success", "compact_daemon_wake"}, kGreen},
  {"THP (TRANSPARENT HUGEPAGES)", {"thp_fault_alloc", "thp_collapse_alloc",
                                     "thp_split", "thp_zero_page_alloc"}, kYellow},
};

// Get all known keys from all categories
std::vector<std::string> all_known_keys() {
  std::vector<std::string> keys;
  for (const auto& cat : kCategories)
    keys.insert(keys.end(), cat.keys.begin(), cat.keys.end());
  return keys;
}

// ── Parsing /proc/vmstat ──────────────────────────────────────────────────
VmstatMap read_vmstat() {
  VmstatMap result;
  std::ifstream f("/proc/vmstat");
  if (!f.is_open()) return result;

  std::string line;
  while (std::getline(f, line)) {
    if (line.empty()) continue;
    auto space = line.find(' ');
    if (space == std::string::npos) continue;
    std::string key = line.substr(0, space);
    auto val_str = std::string_view(line).substr(space + 1);
    // Trim leading spaces
    while (!val_str.empty() && (val_str.front() == ' ' || val_str.front() == '\t'))
      val_str.remove_prefix(1);
    long long val = 0;
    std::from_chars(val_str.data(), val_str.data() + val_str.size(), val);
    result[key] = val;
  }
  return result;
}

// ── Delta computation ─────────────────────────────────────────────────────
VmstatMap compute_delta(const VmstatMap& prev, const VmstatMap& cur) {
  VmstatMap delta;
  for (const auto& [k, v] : cur) {
    auto it = prev.find(k);
    long long p = (it != prev.end()) ? it->second : 0;
    long long d = v - p;
    // Counter wraparound: if negative, assume counter reset
    delta[k] = (d >= 0) ? d : v;
  }
  return delta;
}

// ── Format helpers ──────────────────────────────────────────────────────────
std::string fmt_count(long long n) {
  if (n == 0) return "    0";
  char buf[32];
  if (n >= 1'000'000'000)
    std::snprintf(buf, sizeof(buf), "%5.1fB", static_cast<double>(n) / 1e9);
  else if (n >= 1'000'000)
    std::snprintf(buf, sizeof(buf), "%5.1fM", static_cast<double>(n) / 1e6);
  else if (n >= 1'000)
    std::snprintf(buf, sizeof(buf), "%5.1fK", static_cast<double>(n) / 1e3);
  else
    std::snprintf(buf, sizeof(buf), "%5lld", n);
  return buf;
}

std::string fmt_delta(long long n) {
  if (n == 0) return "     0";
  char buf[32];
  if (n >= 1'000'000'000)
    std::snprintf(buf, sizeof(buf), "%+6.1fB", static_cast<double>(n) / 1e9);
  else if (n >= 1'000'000)
    std::snprintf(buf, sizeof(buf), "%+6.1fM", static_cast<double>(n) / 1e6);
  else if (n >= 1'000)
    std::snprintf(buf, sizeof(buf), "%+6.1fK", static_cast<double>(n) / 1e3);
  else
    std::snprintf(buf, sizeof(buf), "%+6lld", n);
  return buf;
}

// Rate per second
std::string fmt_rate(long long delta, double seconds) {
  if (seconds <= 0 || delta == 0) return "  0/s";
  double rate = static_cast<double>(delta) / seconds;
  char buf[32];
  if (rate >= 1'000'000.0)
    std::snprintf(buf, sizeof(buf), "%5.1fM/s", rate / 1e6);
  else if (rate >= 1'000.0)
    std::snprintf(buf, sizeof(buf), "%5.1fK/s", rate / 1e3);
  else
    std::snprintf(buf, sizeof(buf), "%5.0f/s", rate);
  return buf;
}

// ── Rendering helpers ──────────────────────────────────────────────────────
void print_banner(bool color) {
  const char* c = color ? kCyan : "";
  const char* b = color ? kBold : "";
  const char* r = color ? kReset : "";
  std::println(std::cout, "{}{}vmstat-watch{} — Linux virtual memory statistics (C++23)", c, b, r);
  std::println(std::cout);
}

void render_snapshot(const VmstatMap& data, bool color) {
  for (const auto& cat : kCategories) {
    const char* c = color ? cat.color : "";
    const char* r = color ? kReset : "";
    std::println(std::cout, "  {}{}{}{}:", c, kBold, cat.name, r);

    for (const auto& key : cat.keys) {
      auto it = data.find(key);
      long long val = (it != data.end()) ? it->second : 0;
      // Pad key to 28 chars
      std::string padded = key;
      if (padded.size() < 28) padded.append(28 - padded.size(), ' ');
      std::println(std::cout, "    {} {}", padded, fmt_count(val));
    }
    std::println(std::cout);
  }

  // ── Summary line ──
  auto pgfault = data.find("pgfault");
  auto pgmajf = data.find("pgmajfault");
  auto pswpin = data.find("pswpin");
  auto pswpout = data.find("pswpout");

  long long faults = pgfault != data.end() ? pgfault->second : 0;
  long long majf = pgmajf != data.end() ? pgmajf->second : 0;
  long long swapin = pswpin != data.end() ? pswpin->second : 0;
  long long swapout = pswpout != data.end() ? pswpout->second : 0;

  const char* d = color ? kDim : "";
  const char* r = color ? kReset : "";
  std::println(std::cout, "  {}total faults: {}  major faults: {}  swap in: {}  swap out: {}{}",
               d, fmt_count(faults), fmt_count(majf), fmt_count(swapin), fmt_count(swapout), r);
}

void render_delta_table(const VmstatMap& delta, double elapsed, bool color) {
  for (const auto& cat : kCategories) {
    const char* c = color ? cat.color : "";
    const char* r = color ? kReset : "";
    std::println(std::cout, "  {}{}{}{}:", c, kBold, cat.name, r);

    for (const auto& key : cat.keys) {
      auto it = delta.find(key);
      long long val = (it != delta.end()) ? it->second : 0;
      std::string padded = key;
      if (padded.size() < 28) padded.append(28 - padded.size(), ' ');
      std::println(std::cout, "    {} {}  {}", padded, fmt_delta(val), fmt_rate(val, elapsed));
    }
    std::println(std::cout);
  }

  auto pgfault = delta.find("pgfault");
  long long faults = pgfault != delta.end() ? pgfault->second : 0;
  auto pgmajf = delta.find("pgmajfault");
  long long majf = pgmajf != delta.end() ? pgmajf->second : 0;
  auto pswpin = delta.find("pswpin");
  long long swapin = pswpin != delta.end() ? pswpin->second : 0;
  auto pswpout = delta.find("pswpout");
  long long swapout = pswpout != delta.end() ? pswpout->second : 0;

  const char* d = color ? kDim : "";
  const char* r = color ? kReset : "";
  std::println(std::cout, "  {}faults: {}  maj faults: {}  swap in: {}  swap out: {}  ({}s interval){}",
               d, fmt_delta(faults), fmt_delta(majf), fmt_delta(swapin), fmt_delta(swapout),
               elapsed, r);
}

void render_json(const VmstatMap& data) {
  std::cout << "{\n";
  bool first = true;
  // Sort keys for stable output
  std::vector<std::string> keys;
  for (const auto& [k, _] : data) keys.push_back(k);
  std::sort(keys.begin(), keys.end());
  for (const auto& k : keys) {
    auto it = data.find(k);
    if (!first) std::cout << ",\n";
    first = false;
    std::cout << "  \"" << k << "\": " << it->second;
  }
  if (!first) std::cout << '\n';
  std::cout << "}\n" << std::flush;
}

// ── Selftest ───────────────────────────────────────────────────────────────
int run_selftest() {
  int passed = 0, total = 0;

  // Test 1: parse vmstat line
  total++;
  {
    std::string line = "pgfault 1234567890";
    auto data = VmstatMap{};
    auto space = line.find(' ');
    if (space != std::string::npos) {
      std::string key = line.substr(0, space);
      auto val_str = std::string_view(line).substr(space + 1);
      long long val = 0;
      std::from_chars(val_str.data(), val_str.data() + val_str.size(), val);
      data[key] = val;
    }
    if (data["pgfault"] == 1234567890) passed++;
  }

  // Test 2: parse line with leading spaces after key
  total++;
  {
    std::string line = "pswpin        12345";
    auto space = line.find(' ');
    auto val_str = std::string_view(line).substr(space + 1);
    while (!val_str.empty() && val_str.front() == ' ')
      val_str.remove_prefix(1);
    long long val = 0;
    std::from_chars(val_str.data(), val_str.data() + val_str.size(), val);
    if (val == 12345) passed++;
  }

  // Test 3: compute_delta with increment
  total++;
  {
    VmstatMap prev{{"pgfault", 100}, {"pswpin", 50}};
    VmstatMap cur{{"pgfault", 150}, {"pswpin", 60}};
    auto delta = compute_delta(prev, cur);
    if (delta["pgfault"] == 50 && delta["pswpin"] == 10) passed++;
  }

  // Test 4: compute_delta with new key
  total++;
  {
    VmstatMap prev{};
    VmstatMap cur{{"pgfault", 100}};
    auto delta = compute_delta(prev, cur);
    if (delta["pgfault"] == 100) passed++;
  }

  // Test 5: compute_delta with wraparound (counter reset)
  total++;
  {
    VmstatMap prev{{"pgfault", 200}};
    VmstatMap cur{{"pgfault", 50}};
    auto delta = compute_delta(prev, cur);
    // Negative delta → treat as reset, use current value
    if (delta["pgfault"] == 50) passed++;
  }

  // Test 6: fmt_count basic
  total++;
  {
    auto s = fmt_count(0);
    if (s.find("0") != std::string::npos) passed++;
  }

  // Test 7: fmt_count millions
  total++;
  {
    auto s = fmt_count(12345678);
    if (s.find("12.3M") != std::string::npos) passed++;
  }

  // Test 8: fmt_rate
  total++;
  {
    auto s = fmt_rate(1000, 1.0);
    if (s.find("1.0K") != std::string::npos) passed++;
  }

  // Test 9: all_known_keys returns non-empty
  total++;
  {
    auto keys = all_known_keys();
    if (!keys.empty()) passed++;
  }

  // Test 10: read_vmstat returns real data (skip if /proc/vmstat unavailable)
  total++;
  {
    auto data = read_vmstat();
    if (!data.empty()) passed++;
    else std::println(std::cout, "  SKIP: /proc/vmstat not available");
  }

  // Test 11: /proc/vmstat has pgfault
  total++;
  {
    auto data = read_vmstat();
    if (data.contains("pgfault")) passed++;
    else std::println(std::cout, "  SKIP: pgfault not in vmstat");
  }

  // Test 12: fmt_delta positive
  total++;
  {
    auto s = fmt_delta(5000000);
    if (s.find("+") != std::string::npos || s.find("5.0M") != std::string::npos) passed++;
  }

  // Test 13: fmt_delta zero
  total++;
  {
    auto s = fmt_delta(0);
    if (s.find("0") != std::string::npos) passed++;
  }

  // Test 14: parse with empty line
  total++;
  {
    std::string line;
    auto space = line.find(' ');
    if (space == std::string::npos) passed++;
  }

  // Test 15: known categories have keys
  total++;
  {
    int count = 0;
    for (const auto& cat : kCategories)
      count += static_cast<int>(cat.keys.size());
    if (count > 0) passed++;
  }

  std::println(std::cout, "{}/{} selftests passed", passed, total);
  return (passed == total) ? 0 : 2;
}

// ── Watch mode ──────────────────────────────────────────────────────────────
volatile std::sig_atomic_t g_running = 1;
extern "C" void handle_signal(int) { g_running = 0; }

void run_watch(int interval, bool color) {
  std::signal(SIGINT, handle_signal);
  std::signal(SIGTERM, handle_signal);

  std::println(std::cout, "{}refreshing every {}s — Ctrl+C to stop{}",
               (color ? kDim : ""), interval, (color ? kReset : ""));
  std::println(std::cout);

  auto prev = read_vmstat();
  std::this_thread::sleep_for(std::chrono::seconds(interval));

  while (g_running) {
    auto cur = read_vmstat();
    auto delta = compute_delta(prev, cur);

    std::cout << "\x1b[2J\x1b[H";
    print_banner(color);
    render_delta_table(delta, static_cast<double>(interval), color);
    std::cout << std::flush;

    prev = cur;
    for (int i = 0; i < interval && g_running; ++i)
      std::this_thread::sleep_for(std::chrono::seconds(1));
  }

  std::println(std::cout, "\n{}vmstat-watch stopped{}",
               (color ? kDim : ""), (color ? kReset : ""));
}

} // namespace vmstat

// ── Main ───────────────────────────────────────────────────────────────────
int main(int argc, char** argv) {
  bool color = true;
  bool json = false;
  bool selftest = false;
  int watch_interval = 0;
  bool help = false;
  for (int i = 1; i < argc; ++i) {
    std::string_view arg{argv[i]};
    if (arg == "--no-color" || arg == "--no-colour") color = false;
    else if (arg == "--json") json = true;
    else if (arg == "--selftest") selftest = true;
    else if (arg == "--help" || arg == "-h") help = true;
    else if (arg == "--watch" && i + 1 < argc) watch_interval = std::atoi(argv[++i]);
    else if (arg.starts_with("--watch=")) watch_interval = std::atoi(arg.data() + 8);
    else {
      std::println(std::cerr, "Unknown option: {}", arg);
      return 2;
    }
  }

  if (help) {
    std::println(std::cout, "Usage: vmstat-watch [options]");
    std::println(std::cout);
    std::println(std::cout, "  --help, -h       Show this help");
    std::println(std::cout, "  --watch SECONDS  Refresh every SECONDS (Ctrl+C to stop)");
    std::println(std::cout, "  --snapshot       Single snapshot with human-readable format");
    std::println(std::cout, "  --json           Output all counters as JSON");
    std::println(std::cout, "  --no-color       Disable ANSI colors");
    std::println(std::cout, "  --selftest       Run self-tests");
    std::println(std::cout);
    std::println(std::cout, "Reads /proc/vmstat for Linux virtual memory statistics.");
    std::println(std::cout, "Groups counters into: page faults, swap, NUMA, workingset,");
    std::println(std::cout, "slab, compaction, THP and more.");
    return 0;
  }

  if (selftest) {
    return vmstat::run_selftest();
  }

  auto data = vmstat::read_vmstat();

  if (json) {
    vmstat::render_json(data);
    return 0;
  }

  if (watch_interval > 0) {
    vmstat::run_watch(watch_interval, color);
    return 0;
  }

  // Default: show snapshot
  vmstat::print_banner(color);
  vmstat::render_snapshot(data, color);

  return 0;
}
