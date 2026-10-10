#include "led-matrix.h"
#include "graphics.h"

#include <getopt.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <limits.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <curl/curl.h>
#include <fstream>
#include <iostream>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>
#include <thread>
#include <vector>
#include <sys/wait.h>

using namespace rgb_matrix;
using json = nlohmann::json;

static const char *API_URL = "https://freddyanddavid.com/api/all";
static const char *CACHE_FILE = "all-cache.json";

static volatile sig_atomic_t interrupt_received = 0;

static std::mutex api_mutex;
static json latest_api_data = json::object();
static std::atomic<bool> new_api_data(false);
static std::atomic<bool> api_thread_running(true);


enum class EventTimeMode {
  REAL_TIME,
  TIME_ONLY,
  DATETIME
};

struct EventTimeFilter {
  EventTimeMode mode = EventTimeMode::REAL_TIME;
  int seconds_since_midnight = 0;
  time_t datetime = 0;
};

static constexpr time_t EVENT_WINDOW_SECONDS = 6 * 60 * 60;

class ObjectData {
public:
  std::string destination;
  std::string description;
  time_t scheduled_time;
  time_t estimated_time;

  std::string get_scheduled_time() const {
    char buf[32];
    struct tm tm_info;
    localtime_r(&scheduled_time, &tm_info);
    strftime(buf, sizeof(buf), "%H:%M", &tm_info);
    return std::string(buf);
  }

  std::string get_estimated_time() const {
    char buf[32];
    struct tm tm_info;
    localtime_r(&estimated_time, &tm_info);
    strftime(buf, sizeof(buf), "%H:%M", &tm_info);
    return std::string(buf);
  }
};

static void InterruptHandler(int signo) {
  (void)signo;
  interrupt_received = 1;
}

static bool parseColor(Color *c, const char *str) {
  return sscanf(str, "%hhu,%hhu,%hhu", &c->r, &c->g, &c->b) == 3;
}

static bool FullSaturation(const Color &c) {
  return (c.r == 0 || c.r == 255)
      && (c.g == 0 || c.g == 255)
      && (c.b == 0 || c.b == 255);
}

static size_t WriteMemoryCallback(void *contents, size_t size, size_t nmemb,
                                  void *userp) {
  static_cast<std::string *>(userp)->append(
      static_cast<char *>(contents), size * nmemb);
  return size * nmemb;
}

static std::string get_config_string(const json &root, const std::string &key,
                                     const std::string &default_value = "") {
  if (!root.contains(key) || root[key].is_null()) {
    return default_value;
  }

  const json &value = root[key];

  if (value.is_string()) {
    return value.get<std::string>();
  }

  // /api/all exposes config values as objects such as:
  // {"key":"mode","value":"all_screens"}
  if (value.is_object()
      && value.contains("value")
      && value["value"].is_string()) {
    return value["value"].get<std::string>();
  }

  return default_value;
}

static bool json_to_bool(const json &value, bool default_value = false) {
  if (value.is_boolean()) {
    return value.get<bool>();
  }

  if (value.is_number_integer()) {
    return value.get<long long>() != 0;
  }

  if (value.is_string()) {
    std::string text = value.get<std::string>();
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    if (text == "true" || text == "1" || text == "yes" || text == "on") {
      return true;
    }
    if (text == "false" || text == "0" || text == "no" || text == "off" || text.empty()) {
      return false;
    }
  }

  return default_value;
}

static bool get_config_bool(const json &root, const std::string &key,
                            bool default_value = false) {
  if (!root.contains(key) || root[key].is_null()) {
    return default_value;
  }

  const json &value = root[key];

  if (value.is_object() && value.contains("value")) {
    return json_to_bool(value["value"], default_value);
  }

  return json_to_bool(value, default_value);
}

static bool item_is_enabled(const json &item) {
  // Older/cached data may not have an enabled field. Treat missing as enabled,
  // but an explicit false value always removes the item from rotation.
  if (!item.is_object() || !item.contains("enabled")) {
    return true;
  }

  return json_to_bool(item["enabled"], false);
}

static json enabled_items_only(const json &items) {
  json filtered = json::array();

  if (!items.is_array()) {
    return filtered;
  }

  for (const auto &item : items) {
    if (item_is_enabled(item)) {
      filtered.push_back(item);
    }
  }

  return filtered;
}

static bool parse_iso8601_utc(const std::string &value, time_t *result) {
  // Laravel's JSON timestamps from /api/all look like
  // 2026-09-05T12:30:00.000000Z. Parse the UTC portion explicitly so BST is
  // applied only when the timestamp is later formatted with localtime().
  if (value.size() < 19) {
    return false;
  }

  struct tm parsed_tm = {};
  char *end = strptime(value.substr(0, 19).c_str(), "%Y-%m-%dT%H:%M:%S", &parsed_tm);
  if (end == nullptr || *end != '\0') {
    return false;
  }

  parsed_tm.tm_isdst = 0;
  const time_t parsed = timegm(&parsed_tm);
  if (parsed == static_cast<time_t>(-1)) {
    return false;
  }

  *result = parsed;
  return true;
}

static long double normalize_unix_timestamp(long double value) {
  const long double absolute = value < 0 ? -value : value;

  // Accept Unix seconds, milliseconds or microseconds. This keeps the API
  // flexible while normalising everything to seconds for comparison.
  if (absolute >= 100000000000000.0L) {
    return value / 1000000.0L;
  }
  if (absolute >= 100000000000.0L) {
    return value / 1000.0L;
  }
  return value;
}

static bool parse_latch_timestamp_value(const json &input, long double *result) {
  const json *value = &input;

  // Config-style values from /api/all may be wrapped as {"value": ...}.
  if (value->is_object() && value->contains("value")) {
    value = &((*value)["value"]);
  }

  if (value->is_null()) {
    return false;
  }

  if (value->is_number()) {
    *result = normalize_unix_timestamp(value->get<long double>());
    return true;
  }

  if (!value->is_string()) {
    return false;
  }

  const std::string text = value->get<std::string>();
  if (text.empty()) {
    return false;
  }

  // Numeric timestamps supplied as strings are also accepted.
  char *numeric_end = nullptr;
  errno = 0;
  const long double numeric = strtold(text.c_str(), &numeric_end);
  if (errno == 0 && numeric_end != text.c_str() && *numeric_end == '\0') {
    *result = normalize_unix_timestamp(numeric);
    return true;
  }

  // Otherwise accept the same Laravel-style UTC ISO timestamp used by the
  // events API, retaining fractional seconds so two rapid updates still order
  // correctly.
  time_t whole_seconds = 0;
  if (!parse_iso8601_utc(text, &whole_seconds)) {
    return false;
  }

  long double timestamp = static_cast<long double>(whole_seconds);

  if (text.size() > 20 && text[19] == '.') {
    long double place = 0.1L;
    for (size_t i = 20; i < text.size() && std::isdigit(static_cast<unsigned char>(text[i])); ++i) {
      timestamp += static_cast<long double>(text[i] - '0') * place;
      place *= 0.1L;
    }
  }

  *result = timestamp;
  return true;
}

static bool get_open_latch_timestamp(const json &root, long double *result) {
  if (!root.is_object() || !root.contains("openLatch")) {
    return false;
  }

  return parse_latch_timestamp_value(root["openLatch"], result);
}

static time_t get_time_value(const json &event, const char *primary_key,
                             const char *fallback_key) {
  const char *keys[] = {primary_key, fallback_key};

  for (const char *key : keys) {
    try {
      if (!event.contains(key) || event[key].is_null()) {
        continue;
      }

      if (event[key].is_number()) {
        return event[key].get<time_t>();
      }

      if (event[key].is_string()) {
        time_t parsed = 0;
        if (parse_iso8601_utc(event[key].get<std::string>(), &parsed)) {
          return parsed;
        }
      }
    } catch (const json::exception &) {
    }
  }

  return 0;
}

static std::string get_item_message(const json &item) {
  if (item.is_string()) {
    return item.get<std::string>();
  }

  if (item.is_object()
      && item.contains("message")
      && item["message"].is_string()) {
    return item["message"].get<std::string>();
  }

  return "";
}

static std::string get_random_message(const json &items) {
  if (!items.is_array() || items.empty()) {
    return "";
  }

  const size_t index = static_cast<size_t>(rand()) % items.size();
  return get_item_message(items[index]);
}

static bool items_contain_message(const json &items, const std::string &message) {
  if (!items.is_array()) {
    return false;
  }

  for (const auto &item : items) {
    if (get_item_message(item) == message) {
      return true;
    }
  }

  return false;
}

static int text_width(const rgb_matrix::Font &font, const std::string &text,
                      int letter_spacing) {
  int width = 0;

  for (size_t i = 0; i < text.size(); ++i) {
    width += font.CharacterWidth(text[i]);
    if (i + 1 < text.size()) {
      width += letter_spacing;
    }
  }

  return width;
}

static bool parse_time_only(const std::string &value, int *seconds_since_midnight) {
  int hour = 0;
  int minute = 0;
  int second = 0;
  int consumed = 0;

  if (sscanf(value.c_str(), "%d:%d:%d%n", &hour, &minute, &second, &consumed) == 3
      && consumed == static_cast<int>(value.size())) {
    // Parsed HH:MM:SS.
  } else {
    second = 0;
    consumed = 0;
    if (sscanf(value.c_str(), "%d:%d%n", &hour, &minute, &consumed) != 2
        || consumed != static_cast<int>(value.size())) {
      return false;
    }
  }

  if (hour < 0 || hour > 23
      || minute < 0 || minute > 59
      || second < 0 || second > 59) {
    return false;
  }

  *seconds_since_midnight = hour * 3600 + minute * 60 + second;
  return true;
}

static bool parse_datetime_value(const std::string &value, time_t *result) {
  const char *formats[] = {
      "%Y-%m-%d %H:%M:%S",
      "%Y-%m-%d %H:%M",
      "%Y-%m-%dT%H:%M:%S",
      "%Y-%m-%dT%H:%M"
  };

  for (const char *format : formats) {
    struct tm parsed_tm = {};
    parsed_tm.tm_isdst = -1;

    char *end = strptime(value.c_str(), format, &parsed_tm);
    if (end == nullptr) {
      continue;
    }

    while (*end == ' ' || *end == '\t') {
      ++end;
    }

    if (*end != '\0') {
      continue;
    }

    const time_t parsed = mktime(&parsed_tm);
    if (parsed == static_cast<time_t>(-1)) {
      continue;
    }

    *result = parsed;
    return true;
  }

  return false;
}

static bool parse_event_time_filter(const std::string &value,
                                    EventTimeFilter *filter) {
  int seconds_since_midnight = 0;
  if (parse_time_only(value, &seconds_since_midnight)) {
    filter->mode = EventTimeMode::TIME_ONLY;
    filter->seconds_since_midnight = seconds_since_midnight;
    filter->datetime = 0;
    return true;
  }

  time_t datetime = 0;
  if (parse_datetime_value(value, &datetime)) {
    filter->mode = EventTimeMode::DATETIME;
    filter->datetime = datetime;
    filter->seconds_since_midnight = 0;
    return true;
  }

  return false;
}

static bool same_local_date(time_t a, time_t b) {
  struct tm a_tm;
  struct tm b_tm;
  localtime_r(&a, &a_tm);
  localtime_r(&b, &b_tm);

  return a_tm.tm_year == b_tm.tm_year
      && a_tm.tm_mon == b_tm.tm_mon
      && a_tm.tm_mday == b_tm.tm_mday;
}

static bool event_is_future(const ObjectData &object,
                            const EventTimeFilter &filter) {
  if (object.estimated_time <= 0) {
    return false;
  }

  // In normal operation, keep only departures in the rolling six-hour window.
  // There is deliberately no same-day restriction, so a 23:00 comparison time
  // can include departures after midnight.
  if (filter.mode == EventTimeMode::REAL_TIME) {
    const time_t now = time(nullptr);
    return object.estimated_time > now
        && object.estimated_time <= now + EVENT_WINDOW_SECONDS;
  }

  if (filter.mode == EventTimeMode::DATETIME) {
    return object.estimated_time > filter.datetime
        && object.estimated_time <= filter.datetime + EVENT_WINDOW_SECONDS;
  }

  // TIME_ONLY objects are built against a concrete reference date in
  // build_events(). They are not re-filtered as real time advances.
  return true;
}

static std::string first_event_key(const std::vector<ObjectData> &objects) {
  if (objects.empty()) {
    return "";
  }

  const ObjectData &object = objects.front();
  return std::to_string(static_cast<long long>(object.scheduled_time))
      + "|" + object.destination
      + "|" + object.description;
}

static std::vector<ObjectData> build_events(const json &events,
                                                  const EventTimeFilter &filter) {
  std::vector<ObjectData> all_enabled_events;

  if (!events.is_array()) {
    return all_enabled_events;
  }

  for (const auto &event : events) {
    if (!event.is_object() || !item_is_enabled(event)) {
      continue;
    }

    ObjectData object;
    object.destination = event.value("destination", "");
    object.description = event.value("description", "");
    object.scheduled_time = get_time_value(event, "scheduled_time", "scheduled_ts");
    object.estimated_time = get_time_value(event, "estimated_time", "estimated_ts");

    if (object.estimated_time > 0) {
      all_enabled_events.push_back(object);
    }
  }

  if (all_enabled_events.empty()) {
    return all_enabled_events;
  }

  // A time-only override uses the date of the earliest enabled API event as
  // its reference date. The six-hour window is allowed to cross midnight.
  time_t comparison_time = time(nullptr);

  if (filter.mode == EventTimeMode::DATETIME) {
    comparison_time = filter.datetime;
  } else if (filter.mode == EventTimeMode::TIME_ONLY) {
    const auto earliest = std::min_element(
        all_enabled_events.begin(), all_enabled_events.end(),
        [](const ObjectData &a, const ObjectData &b) {
          return a.estimated_time < b.estimated_time;
        });

    struct tm reference_tm;
    localtime_r(&earliest->estimated_time, &reference_tm);
    reference_tm.tm_hour = filter.seconds_since_midnight / 3600;
    reference_tm.tm_min = (filter.seconds_since_midnight % 3600) / 60;
    reference_tm.tm_sec = filter.seconds_since_midnight % 60;
    reference_tm.tm_isdst = -1;
    comparison_time = mktime(&reference_tm);
  }

  const time_t window_end = comparison_time + EVENT_WINDOW_SECONDS;

  std::vector<ObjectData> objects;
  for (const ObjectData &object : all_enabled_events) {
    if (object.estimated_time > comparison_time
        && object.estimated_time <= window_end) {
      objects.push_back(object);
    }
  }

  std::sort(objects.begin(), objects.end(),
            [](const ObjectData &a, const ObjectData &b) {
              return a.estimated_time < b.estimated_time;
            });

  return objects;
}

static bool fetch_from_api(json &result) {
  std::string json_string;
  CURL *curl = curl_easy_init();

  if (!curl) {
    return false;
  }

  curl_easy_setopt(curl, CURLOPT_URL, API_URL);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteMemoryCallback);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &json_string);
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 1500L);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 4000L);
  curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

  const CURLcode res = curl_easy_perform(curl);
  curl_easy_cleanup(curl);

  if (res != CURLE_OK) {
    std::cerr << "API fetch failed: " << curl_easy_strerror(res) << std::endl;
    return false;
  }

  try {
    result = json::parse(json_string);
    return true;
  } catch (const json::exception &e) {
    std::cerr << "API returned invalid JSON: " << e.what() << std::endl;
    return false;
  }
}

static bool save_cache(const json &data) {
  const std::string cache_file(CACHE_FILE);
  const std::string temporary_file = cache_file + ".tmp";

  try {
    {
      std::ofstream file(temporary_file.c_str(), std::ios::out | std::ios::trunc);
      if (!file) {
        return false;
      }

      file << data.dump(2);
      file.flush();

      if (!file) {
        return false;
      }
    }

    if (std::rename(temporary_file.c_str(), cache_file.c_str()) != 0) {
      std::remove(temporary_file.c_str());
      return false;
    }

    return true;
  } catch (const std::exception &e) {
    std::cerr << "Could not save API cache: " << e.what() << std::endl;
    std::remove(temporary_file.c_str());
    return false;
  }
}

static bool load_cache(json &data) {
  try {
    std::ifstream file(CACHE_FILE);
    if (!file) {
      return false;
    }

    json cached;
    file >> cached;

    if (!cached.is_object()) {
      return false;
    }

    data = cached;
    return true;
  } catch (const std::exception &e) {
    std::cerr << "Could not load API cache: " << e.what() << std::endl;
    return false;
  }
}

static bool trigger_latch_script(const std::string &script_path) {
  pid_t pid = fork();

  if (pid < 0) {
    std::cerr << "Could not fork latch helper: " << strerror(errno) << std::endl;
    return false;
  }

  if (pid == 0) {
    execl("/usr/bin/python3", "python3", script_path.c_str(),
          static_cast<char *>(nullptr));
    _exit(127);
  }

  int status = 0;
  pid_t waited;
  do {
    waited = waitpid(pid, &status, 0);
  } while (waited < 0 && errno == EINTR);

  if (waited < 0) {
    std::cerr << "Could not wait for latch helper: " << strerror(errno) << std::endl;
    return false;
  }

  if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
    std::cerr << "Latch helper failed" << std::endl;
    return false;
  }

  return true;
}

static void api_fetch_loop(int interval_seconds,
                           const std::string &latch_script_path,
                           bool have_last_open_latch,
                           long double last_open_latch_timestamp) {
  while (api_thread_running.load()) {
    json fetched;

    if (fetch_from_api(fetched)) {
      long double fetched_open_latch_timestamp = 0.0L;
      const bool have_fetched_open_latch = get_open_latch_timestamp(
          fetched, &fetched_open_latch_timestamp);

      const bool should_open_latch = have_last_open_latch
          && have_fetched_open_latch
          && fetched_open_latch_timestamp > last_open_latch_timestamp;

      // Save the new API response before actuating the latch. This gives the
      // trigger at-most-once behaviour across restarts: once a newer timestamp
      // has been accepted into the cache, the same timestamp cannot fire again.
      const bool cache_saved = save_cache(fetched);
      if (!cache_saved) {
        std::cerr << "Warning: fetched API data but could not update cache"
                  << std::endl;
      } else {
        if (have_fetched_open_latch
            && (!have_last_open_latch
                || fetched_open_latch_timestamp > last_open_latch_timestamp)) {
          last_open_latch_timestamp = fetched_open_latch_timestamp;
          have_last_open_latch = true;
        }

        if (should_open_latch) {
          std::cout << "openLatch advanced; opening latch" << std::endl;
          if (!trigger_latch_script(latch_script_path)) {
            std::cerr << "Latch trigger failed" << std::endl;
          }
        }
      }

      {
        std::lock_guard<std::mutex> lock(api_mutex);
        latest_api_data = fetched;
      }

      new_api_data.store(true);
    }

    // Sleep in short chunks so Ctrl+C shutdown remains responsive.
    const int sleep_chunks = interval_seconds * 10;
    for (int i = 0; i < sleep_chunks && api_thread_running.load(); ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
  }
}

static std::vector<std::string> build_alt_screens(
    const std::string &mode,
    const std::string &important_message,
    const json &jokes,
    const json &affirmations,
    const json &comments,
    const json &secrets,
    bool enable_secret_screen) {

  std::vector<std::string> alt_screens;

  if (mode != "all_screens") {
    return alt_screens;
  }

  if (!important_message.empty()) {
    alt_screens.push_back("importantMessage");
  }

  if (jokes.is_array() && !jokes.empty()) {
    alt_screens.push_back("jokes");
  }

  if (affirmations.is_array() && !affirmations.empty()) {
    alt_screens.push_back("affirmations");
  }

  if (comments.is_array() && !comments.empty()) {
    alt_screens.push_back("comments");
  }

  if (enable_secret_screen && secrets.is_array() && !secrets.empty()) {
    alt_screens.push_back("secrets");
  }

  // slideshow/gifs are deliberately not added here yet: the original file
  // added those screen names to the rotation but had no renderer for them,
  // which resulted in blank screens.

  return alt_screens;
}

static std::string main_screen_for_mode(const std::string &mode) {
  if (mode == "important_message_only") {
    return "importantMessage";
  }

  return "events";
}

static bool is_text_screen(const std::string &screen) {
  return screen == "jokes"
      || screen == "comments"
      || screen == "affirmations"
      || screen == "secrets"
      || screen == "importantMessage";
}

static std::string executable_directory() {
  char path[PATH_MAX];
  ssize_t len = readlink("/proc/self/exe", path, sizeof(path) - 1);

  if (len <= 0) {
    return ".";
  }

  path[len] = '\0';
  std::string full_path(path);
  size_t slash = full_path.find_last_of('/');

  if (slash == std::string::npos) {
    return ".";
  }

  return full_path.substr(0, slash);
}

static int usage(const char *progname) {
  fprintf(stderr, "usage: %s [options]\n", progname);
  fprintf(stderr,
          "Options:\n"
          "\t-f <font-file>       : Use given BDF font.\n"
          "\t-x <x-origin>        : X origin (Default: 0)\n"
          "\t-y <y-origin>        : Y origin (Default: 0)\n"
          "\t-s <line-spacing>    : Extra spacing between lines\n"
          "\t-S <letter-spacing>  : Extra spacing between letters\n"
          "\t-v <pixels/second>   : Scroll speed (Default: 20)\n"
          "\t-r <seconds>         : API refresh interval (Default: 5)\n"
          "\t-t <time|datetime>   : Event filter override. HH:MM[:SS] compares\n"
          "\t                      : by time-of-day; YYYY-MM-DD HH:MM[:SS] (or T)\n"
          "\t                      : compares against an exact UK datetime.\n"
          "\t-C <r,g,b>           : Text colour (Default: 250,184,0)\n"
          "\t-B <r,g,b>           : Background colour (Default: 0,0,0)\n"
          "\n");
  rgb_matrix::PrintMatrixFlags(stderr);
  return 1;
}

int main(int argc, char *argv[]) {
  RGBMatrix::Options matrix_options;
  rgb_matrix::RuntimeOptions runtime_opt;

  // The latch helper needs GPIO access after the matrix has been initialised.
  // rpi-rgb-led-matrix normally drops root privileges at that point, so keep
  // the process privileged for this appliance.
  runtime_opt.drop_privileges = 0;

  if (!rgb_matrix::ParseOptionsFromFlags(&argc, &argv,
                                          &matrix_options, &runtime_opt)) {
    return usage(argv[0]);
  }

  // Set the process timezone once. Europe/London handles GMT/BST automatically.
  setenv("TZ", "Europe/London", 1);
  tzset();

  srand(static_cast<unsigned int>(time(nullptr)));

  Color color(250, 184, 0);
  Color bg_color(0, 0, 0);
  Color outline_color(0, 0, 0);

  const char *bdf_font_file = NULL;
  int x_orig = 0;
  int y_orig = 0;
  int letter_spacing = 0;
  int line_spacing = 0;

  double scroll_speed = 20.0;  // pixels per second
  int fetch_interval = 5;      // seconds
  EventTimeFilter event_time_filter;

  int opt;
  while ((opt = getopt(argc, argv, "x:y:f:C:B:O:s:S:v:r:t:")) != -1) {
    switch (opt) {
      case 'x':
        x_orig = atoi(optarg);
        break;
      case 'y':
        y_orig = atoi(optarg);
        break;
      case 'f':
        bdf_font_file = strdup(optarg);
        break;
      case 's':
        line_spacing = atoi(optarg);
        break;
      case 'S':
        letter_spacing = atoi(optarg);
        break;
      case 'v':
        scroll_speed = atof(optarg);
        if (scroll_speed <= 0.0) {
          fprintf(stderr, "Scroll speed must be greater than zero.\n");
          return usage(argv[0]);
        }
        break;
      case 'r':
        fetch_interval = atoi(optarg);
        if (fetch_interval < 1) {
          fprintf(stderr, "API refresh interval must be at least 1 second.\n");
          return usage(argv[0]);
        }
        break;
      case 't':
        if (!parse_event_time_filter(optarg, &event_time_filter)) {
          fprintf(stderr,
                  "Invalid event time '%s'. Use HH:MM[:SS] or "
                  "YYYY-MM-DD HH:MM[:SS].\n",
                  optarg);
          return usage(argv[0]);
        }
        break;
      case 'C':
        if (!parseColor(&color, optarg)) {
          fprintf(stderr, "Invalid color spec: %s\n", optarg);
          return usage(argv[0]);
        }
        break;
      case 'B':
        if (!parseColor(&bg_color, optarg)) {
          fprintf(stderr, "Invalid background color spec: %s\n", optarg);
          return usage(argv[0]);
        }
        break;
      default:
        return usage(argv[0]);
    }
  }

  if (bdf_font_file == NULL) {
    fprintf(stderr, "Need to specify BDF font-file with -f\n");
    return usage(argv[0]);
  }

  rgb_matrix::Font font;
  if (!font.LoadFont(bdf_font_file)) {
    fprintf(stderr, "Couldn't load font '%s'\n", bdf_font_file);
    return 1;
  }

  rgb_matrix::Font time_font;
  if (!time_font.LoadFont("fonts/dotbold.bdf")) {
    fprintf(stderr, "Couldn't load time font fonts/dotbold.bdf\n");
    return 1;
  }

  RGBMatrix *matrix = RGBMatrix::CreateFromOptions(matrix_options, runtime_opt);
  if (matrix == NULL) {
    return 1;
  }

  const bool all_extreme_colors = (matrix_options.brightness == 100)
      && FullSaturation(color)
      && FullSaturation(bg_color)
      && FullSaturation(outline_color);

  if (all_extreme_colors) {
    matrix->SetPWMBits(1);
  }

  FrameCanvas *offscreen = matrix->CreateFrameCanvas();

  const int screen_width = offscreen->width();
  const int x = x_orig;
  const int y = y_orig;
  const int scroll_start_x = screen_width + 5;

  signal(SIGTERM, InterruptHandler);
  signal(SIGINT, InterruptHandler);

  // Start from the most recent good response saved on disk. There is no
  // synchronous network request here, so startup/display never waits for CURL.
  json root = json::object();
  if (load_cache(root)) {
    std::cout << "Loaded cached API configuration" << std::endl;
  }

  long double cached_open_latch_timestamp = 0.0L;
  const bool have_cached_open_latch = get_open_latch_timestamp(
      root, &cached_open_latch_timestamp);

  json jokes = enabled_items_only(root.value("jokes", json::array()));
  json affirmations = enabled_items_only(root.value("affirmations", json::array()));
  json events = enabled_items_only(root.value("events", json::array()));
  json slideshow = enabled_items_only(root.value("slideshow", json::array()));
  json gifs = enabled_items_only(root.value("gifs", json::array()));
  json comments = enabled_items_only(root.value("comments", json::array()));
  json secrets = enabled_items_only(root.value("secrets", json::array()));

  std::string important_message = get_config_string(root, "importantMessage", "");
  std::string mode = get_config_string(root, "mode", "all_screens");
  if (mode != "all_screens"
      && mode != "events_only"
      && mode != "important_message_only") {
    mode = "all_screens";
  }
  bool enable_secret_screen = get_config_bool(root, "secretEnabled", false);

  std::vector<ObjectData> objects = build_events(events, event_time_filter);
  std::vector<std::string> alt_screens = build_alt_screens(
      mode,
      important_message,
      jokes,
      affirmations,
      comments,
      secrets,
      enable_secret_screen);

  std::string main_screen = main_screen_for_mode(mode);
  std::string current_screen = main_screen;
  std::string last_alt_screen;
  std::string message;

  const int screen_update_interval = 15;
  const int event_screen_interval = 30;

  double scroll_position = static_cast<double>(scroll_start_x);
  bool scroll_completed_pass = false;
  int middle_length = 0;
  time_t next_screen_update = time(nullptr);

  auto select_message_for_screen = [&]() -> std::string {
    if (current_screen == "jokes") {
      return get_random_message(jokes);
    }

    if (current_screen == "affirmations") {
      return get_random_message(affirmations);
    }

    if (current_screen == "comments") {
      return get_random_message(comments);
    }

    if (current_screen == "secrets") {
      return get_random_message(secrets);
    }

    if (current_screen == "importantMessage") {
      return important_message;
    }

    return "";
  };

  auto set_screen_deadline = [&]() {
    const time_t now = time(nullptr);

    if (current_screen == "events") {
      if (!objects.empty() && event_time_filter.mode == EventTimeMode::REAL_TIME) {
        next_screen_update = std::min(
            objects.front().estimated_time,
            now + event_screen_interval);
      } else {
        // When using a fixed -t test time, event timestamps may be in the past
        // relative to the Pi's real clock, so keep screen timing based on real
        // elapsed runtime rather than the overridden event-filter time.
        next_screen_update = now + event_screen_interval;
      }
    } else {
      next_screen_update = now + screen_update_interval;
    }
  };

  auto reset_scroll = [&]() {
    scroll_position = static_cast<double>(scroll_start_x);
    scroll_completed_pass = false;
    middle_length = 0;
  };

  auto enter_screen = [&]() {
    reset_scroll();
    message = select_message_for_screen();
    set_screen_deadline();
  };

  enter_screen();

  // Initialise libcurl once for the process, then let the background thread own
  // all HTTP activity.
  bool curl_available = (curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK);
  std::thread api_thread;

  if (curl_available) {
    const std::string latch_script_path = executable_directory() + "/open_latch.py";
    api_thread = std::thread(
        api_fetch_loop,
        fetch_interval,
        latch_script_path,
        have_cached_open_latch,
        cached_open_latch_timestamp);
  } else {
    std::cerr << "Could not initialise CURL; continuing with cached data"
              << std::endl;
  }

  auto last_frame_time = std::chrono::steady_clock::now();

  while (!interrupt_received) {
    const auto frame_time = std::chrono::steady_clock::now();
    double elapsed_seconds = std::chrono::duration<double>(
        frame_time - last_frame_time).count();
    last_frame_time = frame_time;

    // Avoid a large scroll jump if the process is suspended/debugged.
    elapsed_seconds = std::max(0.0, std::min(elapsed_seconds, 0.25));

    // Apply a completed background fetch. This is just a memory copy under a
    // mutex; CURL and cache writes never happen on the render thread.
    if (new_api_data.exchange(false)) {
      json new_root;

      {
        std::lock_guard<std::mutex> lock(api_mutex);
        new_root = latest_api_data;
      }

      const std::string old_first_event = first_event_key(objects);
      const std::string old_important_message = important_message;
      const std::string old_mode = mode;

      root = new_root;
      jokes = enabled_items_only(root.value("jokes", json::array()));
      affirmations = enabled_items_only(root.value("affirmations", json::array()));
      events = enabled_items_only(root.value("events", json::array()));
      slideshow = enabled_items_only(root.value("slideshow", json::array()));
      gifs = enabled_items_only(root.value("gifs", json::array()));
      comments = enabled_items_only(root.value("comments", json::array()));
      secrets = enabled_items_only(root.value("secrets", json::array()));
      important_message = get_config_string(root, "importantMessage", "");
      mode = get_config_string(root, "mode", "all_screens");
      if (mode != "all_screens"
          && mode != "events_only"
          && mode != "important_message_only") {
        mode = "all_screens";
      }
      enable_secret_screen = get_config_bool(root, "secretEnabled", false);

      objects = build_events(events, event_time_filter);
      main_screen = main_screen_for_mode(mode);
      alt_screens = build_alt_screens(
          mode,
          important_message,
          jokes,
          affirmations,
          comments,
          secrets,
          enable_secret_screen);

      // If a mode/config update makes the current screen invalid (for example
      // secretEnabled becomes false while a secret is showing), leave it
      // immediately rather than waiting for the normal rotation deadline.
      const bool current_is_main = (current_screen == main_screen);
      const bool current_is_alt = std::find(
          alt_screens.begin(), alt_screens.end(), current_screen) != alt_screens.end();

      if (!current_is_main && !current_is_alt) {
        current_screen = main_screen;
        last_alt_screen.clear();
        enter_screen();
        last_frame_time = std::chrono::steady_clock::now();
        elapsed_seconds = 0.0;
      } else if (mode != old_mode) {
        // A mode change is an explicit operator instruction, so apply it now.
        current_screen = main_screen;
        last_alt_screen.clear();
        enter_screen();
        last_frame_time = std::chrono::steady_clock::now();
        elapsed_seconds = 0.0;
      } else if (current_screen == "importantMessage"
                 && important_message != old_important_message) {
        message = important_message;
        reset_scroll();
        set_screen_deadline();
      } else if (current_screen == "jokes"
                 && !items_contain_message(jokes, message)) {
        message = get_random_message(jokes);
        reset_scroll();
        set_screen_deadline();
      } else if (current_screen == "affirmations"
                 && !items_contain_message(affirmations, message)) {
        message = get_random_message(affirmations);
        reset_scroll();
        set_screen_deadline();
      } else if (current_screen == "comments"
                 && !items_contain_message(comments, message)) {
        message = get_random_message(comments);
        reset_scroll();
        set_screen_deadline();
      } else if (current_screen == "secrets"
                 && !items_contain_message(secrets, message)) {
        message = get_random_message(secrets);
        reset_scroll();
        set_screen_deadline();
      }

      // If the event at the top of the board changed, restart its description
      // from the right so the new text is guaranteed a complete pass.
      if (current_screen == "events"
          && old_first_event != first_event_key(objects)) {
        reset_scroll();
        set_screen_deadline();
      }
    }

    // In normal mode, events expire as real time advances between API updates.
    // With -t, the supplied test time is deliberately fixed, so the selected
    // event set remains stable until the API data itself changes.
    if (event_time_filter.mode == EventTimeMode::REAL_TIME) {
      const std::string old_first_event = first_event_key(objects);

      objects.erase(
          std::remove_if(
              objects.begin(),
              objects.end(),
              [&](const ObjectData &object) {
                return !event_is_future(object, event_time_filter);
              }),
          objects.end());

      if (current_screen == "events"
          && old_first_event != first_event_key(objects)) {
        reset_scroll();
        set_screen_deadline();
      }
    }

    // A text screen may only change after its scrolling text has completely
    // traversed the display at least once. The 15/30 second deadlines remain
    // minimum durations, not hard cut-offs.
    bool scroll_required = false;

    if (current_screen == "events") {
      scroll_required = !objects.empty() && !objects.front().description.empty();
    } else if (is_text_screen(current_screen)) {
      scroll_required = !message.empty();
    }

    const bool may_change_screen = !scroll_required || scroll_completed_pass;

    if (time(nullptr) >= next_screen_update && may_change_screen) {
      if (alt_screens.empty()) {
        current_screen = main_screen;
      } else if (current_screen != main_screen) {
        last_alt_screen = current_screen;
        current_screen = main_screen;
      } else if (last_alt_screen.empty()) {
        current_screen = alt_screens.front();
      } else {
        auto it = std::find(
            alt_screens.begin(), alt_screens.end(), last_alt_screen);

        if (it == alt_screens.end()) {
          current_screen = alt_screens.front();
        } else {
          ++it;
          current_screen = (it == alt_screens.end())
              ? alt_screens.front()
              : *it;
        }
      }

      enter_screen();
      last_frame_time = std::chrono::steady_clock::now();
      elapsed_seconds = 0.0;
    }

    offscreen->Fill(bg_color.r, bg_color.g, bg_color.b);

    int line_offset = 0;

    if (current_screen == "events") {
      if (!objects.empty()) {
        const ObjectData &first = objects[0];

        const std::string scheduled_text = "1st " + first.get_scheduled_time();
        rgb_matrix::DrawText(
            offscreen, font,
            x, y + font.baseline() + line_offset,
            color, NULL, scheduled_text.c_str(), letter_spacing);

        rgb_matrix::DrawText(
            offscreen, font,
            x + 45, y + font.baseline() + line_offset,
            color, NULL, first.destination.c_str(), letter_spacing);

        const std::string estimated_text = "Exp " + first.get_estimated_time();
        rgb_matrix::DrawText(
            offscreen, font,
            screen_width - 45, y + font.baseline() + line_offset,
            color, NULL, estimated_text.c_str(), letter_spacing);

        line_offset += font.height() + line_spacing;

        if (!first.description.empty()) {
          const int scroll_x = static_cast<int>(scroll_position);

          middle_length = rgb_matrix::DrawText(
              offscreen, font,
              scroll_x, y + font.baseline() + line_offset,
              color, nullptr, first.description.c_str(), letter_spacing);

          scroll_position -= scroll_speed * elapsed_seconds;

          if (scroll_position + middle_length < 0.0) {
            scroll_completed_pass = true;
            scroll_position = static_cast<double>(scroll_start_x);
          }
        } else {
          scroll_completed_pass = true;
        }

        line_offset += font.height() + line_spacing;

        if (objects.size() > 1) {
          const ObjectData &second = objects[1];

          const std::string scheduled_text_2 = "2nd " + second.get_scheduled_time();
          rgb_matrix::DrawText(
              offscreen, font,
              x, y + font.baseline() + line_offset,
              color, NULL, scheduled_text_2.c_str(), letter_spacing);

          rgb_matrix::DrawText(
              offscreen, font,
              x + 45, y + font.baseline() + line_offset,
              color, NULL, second.destination.c_str(), letter_spacing);

          const std::string estimated_text_2 = "Exp " + second.get_estimated_time();
          rgb_matrix::DrawText(
              offscreen, font,
              screen_width - 45, y + font.baseline() + line_offset,
              color, NULL, estimated_text_2.c_str(), letter_spacing);
        } else {
          char time_buffer[32];
          const time_t now = time(nullptr);
          struct tm now_tm;
          localtime_r(&now, &now_tm);
          strftime(time_buffer, sizeof(time_buffer), "%H:%M:%S", &now_tm);

          const std::string time_text(time_buffer);
          const int time_x = (screen_width
              - text_width(time_font, time_text, letter_spacing)) / 2;

          rgb_matrix::DrawText(
              offscreen, time_font,
              time_x, y + time_font.baseline() + line_offset,
              color, NULL, time_text.c_str(), letter_spacing);
        }
      } else {
        // No remaining events: show a standard departure-board empty state.
        // The message is centred rather than scrolled because it comfortably
        // fits across the three-panel display.
        const std::string no_departures_text = "No scheduled departures";
        const int no_departures_x = (screen_width
            - text_width(font, no_departures_text, letter_spacing)) / 2;

        rgb_matrix::DrawText(
            offscreen, font,
            no_departures_x, y + font.baseline() + line_offset,
            color, NULL, no_departures_text.c_str(), letter_spacing);

        line_offset += font.height() + line_spacing;

        char time_buffer[32];
        const time_t now = time(nullptr);
        struct tm now_tm;
        localtime_r(&now, &now_tm);
        strftime(time_buffer, sizeof(time_buffer), "%H:%M:%S", &now_tm);

        const std::string time_text(time_buffer);
        const int time_x = (screen_width
            - text_width(time_font, time_text, letter_spacing)) / 2;

        rgb_matrix::DrawText(
            offscreen, time_font,
            time_x, y + time_font.baseline() + line_offset,
            color, NULL, time_text.c_str(), letter_spacing);

        scroll_completed_pass = true;
      }
    } else if (is_text_screen(current_screen)) {
      // Non-event screens always show the bold current time centred across the
      // top, with the current message scrolling beneath it.
      char time_buffer[32];
      const time_t now = time(nullptr);
      struct tm now_tm;
      localtime_r(&now, &now_tm);
      strftime(time_buffer, sizeof(time_buffer), "%H:%M:%S", &now_tm);

      const std::string time_text(time_buffer);
      const int time_x = (screen_width
          - text_width(time_font, time_text, letter_spacing)) / 2;

      rgb_matrix::DrawText(
          offscreen, time_font,
          time_x, y + time_font.baseline(),
          color, NULL, time_text.c_str(), letter_spacing);

      line_offset = time_font.height() + line_spacing;

      if (!message.empty()) {
        const int scroll_x = static_cast<int>(scroll_position);

        middle_length = rgb_matrix::DrawText(
            offscreen, font,
            scroll_x, y + font.baseline() + line_offset,
            color, nullptr, message.c_str(), letter_spacing);

        scroll_position -= scroll_speed * elapsed_seconds;

        if (scroll_position + middle_length < 0.0) {
          scroll_completed_pass = true;
          scroll_position = static_cast<double>(scroll_start_x);
        }
      } else {
        scroll_completed_pass = true;
      }
    }

    // Atomic swap with double buffering. SwapOnVSync also naturally paces the
    // render loop to the matrix refresh rather than busy-waiting.
    offscreen = matrix->SwapOnVSync(offscreen);
  }

  api_thread_running.store(false);

  if (api_thread.joinable()) {
    api_thread.join();
  }

  if (curl_available) {
    curl_global_cleanup();
  }

  delete matrix;

  if (bdf_font_file != NULL) {
    free(const_cast<char *>(bdf_font_file));
  }

  write(STDOUT_FILENO, "\n", 1);
  return 0;
}
