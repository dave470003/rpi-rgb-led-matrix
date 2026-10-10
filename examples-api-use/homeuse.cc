#include "led-matrix.h"
#include "graphics.h"

#include <getopt.h>
#include <signal.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <curl/curl.h>
#include <iomanip>
#include <iostream>
#include <limits>
#include <mutex>
#include <nlohmann/json.hpp>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using namespace rgb_matrix;
using json = nlohmann::json;

// -----------------------------------------------------------------------------
// Home departure board
//
// Three rotating screens:
//   1. Southbound Weaver line departures from Bruce Grove
//   2. Bus departures from both Awlfield Avenue stops
//   3. Lime bikes within 100 m of HOME_LAT/HOME_LON
//
// Required environment variables for the Lime screen:
//   HOME_LAT=...
//   HOME_LON=...
//
// Optional TfL credentials:
//   TFL_APP_ID=...
//   TFL_APP_KEY=...
//
// The LED matrix geometry/mapping is still supplied using the normal
// rpi-rgb-led-matrix command-line flags, e.g. --led-chain=3.
// -----------------------------------------------------------------------------

static const char *BRUCE_GROVE_ID = "910GBRUCGRV";
static const char *AWLFIELD_WEST_ID = "490003521W1";  // towards Turnpike Lane / Wood Green
static const char *AWLFIELD_EAST_ID = "490003521E";   // towards Tottenham / central London

static const char *LIME_DISCOVERY_URL =
    "https://data.lime.bike/api/partners/v2/gbfs/london/gbfs.json";

static const double LIME_RADIUS_METRES = 100.0;
static const double EARTH_RADIUS_METRES = 6371000.0;

static volatile sig_atomic_t interrupt_received = 0;

struct Departure {
  std::string label;
  int seconds_until = 0;
};

struct LimeStatus {
  bool configured = false;
  bool available = false;
  int nearby_count = 0;
  double nearest_distance_m = 0.0;
  double nearest_lat = 0.0;
  double nearest_lon = 0.0;
  std::string nearest_bearing;
};

struct BoardData {
  std::vector<Departure> trains;
  std::vector<Departure> buses;
  LimeStatus lime;

  bool trains_available = false;
  bool buses_available = false;

  time_t trains_updated = 0;
  time_t buses_updated = 0;
  time_t lime_updated = 0;
};

static std::mutex data_mutex;
static BoardData board_data;
static std::atomic<bool> fetch_thread_running(true);

static void InterruptHandler(int signo) {
  (void)signo;
  interrupt_received = 1;
}

static size_t WriteMemoryCallback(void *contents, size_t size, size_t nmemb,
                                  void *userp) {
  static_cast<std::string *>(userp)->append(
      static_cast<char *>(contents), size * nmemb);
  return size * nmemb;
}

static bool http_get_json(const std::string &url, json *result) {
  CURL *curl = curl_easy_init();
  if (!curl) {
    return false;
  }

  std::string response;

  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteMemoryCallback);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 2000L);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 5000L);
  curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
  curl_easy_setopt(
      curl, CURLOPT_USERAGENT,
      "home-led-departure-board/1.0 (personal Raspberry Pi display)");

  const CURLcode res = curl_easy_perform(curl);
  curl_easy_cleanup(curl);

  if (res != CURLE_OK) {
    std::cerr << "HTTP request failed for " << url << ": "
              << curl_easy_strerror(res) << std::endl;
    return false;
  }

  try {
    *result = json::parse(response);
    return true;
  } catch (const json::exception &e) {
    std::cerr << "Invalid JSON from " << url << ": " << e.what() << std::endl;
    return false;
  }
}

static std::string getenv_string(const char *name) {
  const char *value = getenv(name);
  return value ? std::string(value) : std::string();
}

static bool getenv_double(const char *name, double *result) {
  const char *value = getenv(name);
  if (!value || !*value) {
    return false;
  }

  char *end = nullptr;
  const double parsed = strtod(value, &end);

  if (end == value || *end != '\0' || !std::isfinite(parsed)) {
    return false;
  }

  *result = parsed;
  return true;
}

static std::string tfl_url(const std::string &path) {
  std::string url = "https://api.tfl.gov.uk" + path;

  const std::string app_id = getenv_string("TFL_APP_ID");
  const std::string app_key = getenv_string("TFL_APP_KEY");

  const char separator = (url.find('?') == std::string::npos) ? '?' : '&';

  if (!app_id.empty()) {
    url += separator;
    url += "app_id=" + app_id;
    if (!app_key.empty()) {
      url += "&app_key=" + app_key;
    }
  } else if (!app_key.empty()) {
    url += separator;
    url += "app_key=" + app_key;
  }

  return url;
}

static bool parse_iso8601_utc(const std::string &value, time_t *result) {
  if (value.size() < 19) {
    return false;
  }

  struct tm parsed_tm = {};
  char *end = strptime(
      value.substr(0, 19).c_str(),
      "%Y-%m-%dT%H:%M:%S",
      &parsed_tm);

  if (!end || *end != '\0') {
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

static std::string replace_all(std::string text,
                               const std::string &from,
                               const std::string &to) {
  size_t pos = 0;
  while ((pos = text.find(from, pos)) != std::string::npos) {
    text.replace(pos, from.length(), to);
    pos += to.length();
  }
  return text;
}

static std::string shorten_train_destination(std::string destination) {
  destination = replace_all(destination, "London Liverpool Street Rail Station", "Liverpool St");
  destination = replace_all(destination, "Liverpool Street Rail Station", "Liverpool St");
  destination = replace_all(destination, " Rail Station", "");
  destination = replace_all(destination, " Station", "");
  return destination;
}

static std::string shorten_bus_destination(std::string destination) {
  destination = replace_all(destination, "Ilford, Hainault Street", "Ilford");
  destination = replace_all(destination, "Turnpike Lane Station", "Turnpike Ln");
  destination = replace_all(destination, "Wood Green Station", "Wood Green");
  destination = replace_all(destination, "Waterloo Station", "Waterloo");
  destination = replace_all(destination, " Station", "");
  return destination;
}

static bool is_southbound_bruce_grove(const json &item) {
  const std::string platform = item.value("platformName", "");
  const std::string destination = item.value("destinationName", "");

  // TfL's Bruce Grove page currently uses Platform 1 for southbound services.
  if (platform.find("1") != std::string::npos) {
    return true;
  }

  // ArrivalDepartures can occasionally report an unknown platform. Keep the
  // obvious southbound destinations in that case.
  return destination.find("Liverpool Street") != std::string::npos
      || destination.find("Seven Sisters") != std::string::npos
      || destination.find("Hackney Downs") != std::string::npos;
}

static bool fetch_trains(std::vector<Departure> *departures) {
  const std::string path =
      std::string("/StopPoint/") + BRUCE_GROVE_ID
      + "/ArrivalDepartures?lineIds=weaver";

  json response;
  if (!http_get_json(tfl_url(path), &response) || !response.is_array()) {
    return false;
  }

  const time_t now = time(nullptr);
  std::vector<Departure> result;

  for (const auto &item : response) {
    if (!item.is_object() || !is_southbound_bruce_grove(item)) {
      continue;
    }

    const std::string destination = item.value("destinationName", "");
    const std::string eta_string = item.value("estimatedTimeOfArrival", "");

    if (destination.empty() || eta_string.empty()) {
      continue;
    }

    time_t eta = 0;
    if (!parse_iso8601_utc(eta_string, &eta)) {
      continue;
    }

    const int seconds = static_cast<int>(difftime(eta, now));
    if (seconds < -30) {
      continue;
    }

    Departure departure;
    departure.label = shorten_train_destination(destination);
    departure.seconds_until = std::max(0, seconds);
    result.push_back(departure);
  }

  std::sort(
      result.begin(), result.end(),
      [](const Departure &a, const Departure &b) {
        return a.seconds_until < b.seconds_until;
      });

  if (result.size() > 6) {
    result.resize(6);
  }

  *departures = result;
  return true;
}

static bool append_bus_stop(const char *stop_id,
                            std::vector<Departure> *result) {
  const std::string path =
      std::string("/StopPoint/") + stop_id + "/Arrivals";

  json response;
  if (!http_get_json(tfl_url(path), &response) || !response.is_array()) {
    return false;
  }

  for (const auto &item : response) {
    if (!item.is_object()) {
      continue;
    }

    const std::string line = item.value("lineName", "");
    const std::string destination = item.value("destinationName", "");
    const int seconds = item.value("timeToStation", -1);

    if (line.empty() || destination.empty() || seconds < 0) {
      continue;
    }

    Departure departure;
    departure.label = line + " " + shorten_bus_destination(destination);
    departure.seconds_until = seconds;
    result->push_back(departure);
  }

  return true;
}

static bool fetch_buses(std::vector<Departure> *departures) {
  std::vector<Departure> result;

  const bool west_ok = append_bus_stop(AWLFIELD_WEST_ID, &result);
  const bool east_ok = append_bus_stop(AWLFIELD_EAST_ID, &result);

  if (!west_ok && !east_ok) {
    return false;
  }

  std::sort(
      result.begin(), result.end(),
      [](const Departure &a, const Departure &b) {
        return a.seconds_until < b.seconds_until;
      });

  // De-duplicate the same prediction if TfL ever exposes it through both stop
  // records unexpectedly.
  result.erase(
      std::unique(
          result.begin(), result.end(),
          [](const Departure &a, const Departure &b) {
            return a.label == b.label
                && std::abs(a.seconds_until - b.seconds_until) < 20;
          }),
      result.end());

  if (result.size() > 8) {
    result.resize(8);
  }

  *departures = result;
  return true;
}

static std::string find_gbfs_feed_url(const json &node,
                                      const std::string &wanted_name) {
  if (node.is_object()) {
    if (node.contains("name")
        && node.contains("url")
        && node["name"].is_string()
        && node["url"].is_string()
        && node["name"].get<std::string>() == wanted_name) {
      return node["url"].get<std::string>();
    }

    for (auto it = node.begin(); it != node.end(); ++it) {
      const std::string found = find_gbfs_feed_url(it.value(), wanted_name);
      if (!found.empty()) {
        return found;
      }
    }
  } else if (node.is_array()) {
    for (const auto &item : node) {
      const std::string found = find_gbfs_feed_url(item, wanted_name);
      if (!found.empty()) {
        return found;
      }
    }
  }

  return "";
}

static double degrees_to_radians(double degrees) {
  return degrees * 3.14159265358979323846 / 180.0;
}

static double radians_to_degrees(double radians) {
  return radians * 180.0 / 3.14159265358979323846;
}

static double distance_metres(double lat1, double lon1,
                              double lat2, double lon2) {
  const double phi1 = degrees_to_radians(lat1);
  const double phi2 = degrees_to_radians(lat2);
  const double d_phi = degrees_to_radians(lat2 - lat1);
  const double d_lambda = degrees_to_radians(lon2 - lon1);

  const double a =
      sin(d_phi / 2.0) * sin(d_phi / 2.0)
      + cos(phi1) * cos(phi2)
      * sin(d_lambda / 2.0) * sin(d_lambda / 2.0);

  const double c = 2.0 * atan2(sqrt(a), sqrt(1.0 - a));
  return EARTH_RADIUS_METRES * c;
}

static std::string bearing_name(double lat1, double lon1,
                                double lat2, double lon2) {
  const double phi1 = degrees_to_radians(lat1);
  const double phi2 = degrees_to_radians(lat2);
  const double lambda1 = degrees_to_radians(lon1);
  const double lambda2 = degrees_to_radians(lon2);

  const double y = sin(lambda2 - lambda1) * cos(phi2);
  const double x =
      cos(phi1) * sin(phi2)
      - sin(phi1) * cos(phi2) * cos(lambda2 - lambda1);

  double bearing = radians_to_degrees(atan2(y, x));
  bearing = fmod(bearing + 360.0, 360.0);

  static const char *NAMES[] = {
      "N", "NE", "E", "SE", "S", "SW", "W", "NW"
  };

  const int index = static_cast<int>(floor((bearing + 22.5) / 45.0)) % 8;
  return NAMES[index];
}

static std::set<std::string> bicycle_type_ids(const json &vehicle_types) {
  std::set<std::string> ids;

  const json *types = nullptr;

  if (vehicle_types.contains("data")
      && vehicle_types["data"].is_object()
      && vehicle_types["data"].contains("vehicle_types")
      && vehicle_types["data"]["vehicle_types"].is_array()) {
    types = &vehicle_types["data"]["vehicle_types"];
  }

  if (!types) {
    return ids;
  }

  for (const auto &type : *types) {
    if (!type.is_object()) {
      continue;
    }

    const std::string id = type.value("vehicle_type_id", "");
    const std::string form_factor = type.value("form_factor", "");

    if (!id.empty() && form_factor == "bicycle") {
      ids.insert(id);
    }
  }

  return ids;
}

static const json *gbfs_vehicles_array(const json &feed) {
  if (!feed.contains("data") || !feed["data"].is_object()) {
    return nullptr;
  }

  const json &data = feed["data"];

  if (data.contains("bikes") && data["bikes"].is_array()) {
    return &data["bikes"];
  }

  if (data.contains("vehicles") && data["vehicles"].is_array()) {
    return &data["vehicles"];
  }

  return nullptr;
}

static bool fetch_lime(double home_lat, double home_lon, LimeStatus *status) {
  LimeStatus result;
  result.configured = true;

  json discovery;
  if (!http_get_json(LIME_DISCOVERY_URL, &discovery)) {
    return false;
  }

  const std::string free_bike_url =
      find_gbfs_feed_url(discovery, "free_bike_status");
  const std::string vehicle_types_url =
      find_gbfs_feed_url(discovery, "vehicle_types");

  if (free_bike_url.empty()) {
    std::cerr << "Lime GBFS discovery did not contain free_bike_status"
              << std::endl;
    return false;
  }

  json free_bikes;
  if (!http_get_json(free_bike_url, &free_bikes)) {
    return false;
  }

  std::set<std::string> bicycle_ids;
  bool have_type_filter = false;

  if (!vehicle_types_url.empty()) {
    json vehicle_types;
    if (http_get_json(vehicle_types_url, &vehicle_types)) {
      bicycle_ids = bicycle_type_ids(vehicle_types);
      have_type_filter = !bicycle_ids.empty();
    }
  }

  const json *vehicles = gbfs_vehicles_array(free_bikes);
  if (!vehicles) {
    std::cerr << "Lime free_bike_status did not contain bikes/vehicles"
              << std::endl;
    return false;
  }

  result.available = true;

  double nearest_distance = std::numeric_limits<double>::infinity();
  double nearest_lat = 0.0;
  double nearest_lon = 0.0;

  for (const auto &bike : *vehicles) {
    if (!bike.is_object()) {
      continue;
    }

    if (bike.value("is_reserved", false) || bike.value("is_disabled", false)) {
      continue;
    }

    if (have_type_filter) {
      const std::string type_id = bike.value("vehicle_type_id", "");
      if (type_id.empty() || bicycle_ids.count(type_id) == 0) {
        continue;
      }
    }

    if (!bike.contains("lat") || !bike.contains("lon")
        || !bike["lat"].is_number() || !bike["lon"].is_number()) {
      continue;
    }

    const double lat = bike["lat"].get<double>();
    const double lon = bike["lon"].get<double>();
    const double distance = distance_metres(home_lat, home_lon, lat, lon);

    if (distance <= LIME_RADIUS_METRES) {
      ++result.nearby_count;
    }

    if (distance < nearest_distance) {
      nearest_distance = distance;
      nearest_lat = lat;
      nearest_lon = lon;
    }
  }

  if (std::isfinite(nearest_distance)) {
    result.nearest_distance_m = nearest_distance;
    result.nearest_lat = nearest_lat;
    result.nearest_lon = nearest_lon;
    result.nearest_bearing =
        bearing_name(home_lat, home_lon, nearest_lat, nearest_lon);
  }

  *status = result;
  return true;
}

static void fetch_loop(int refresh_seconds,
                       bool lime_configured,
                       double home_lat,
                       double home_lon) {
  while (fetch_thread_running.load()) {
    BoardData next;

    {
      std::lock_guard<std::mutex> lock(data_mutex);
      next = board_data;
    }

    std::vector<Departure> trains;
    if (fetch_trains(&trains)) {
      next.trains = trains;
      next.trains_available = true;
      next.trains_updated = time(nullptr);
    }

    std::vector<Departure> buses;
    if (fetch_buses(&buses)) {
      next.buses = buses;
      next.buses_available = true;
      next.buses_updated = time(nullptr);
    }

    if (lime_configured) {
      LimeStatus lime;
      if (fetch_lime(home_lat, home_lon, &lime)) {
        next.lime = lime;
        next.lime_updated = time(nullptr);
      } else if (!next.lime.available) {
        next.lime.configured = true;
      }
    } else {
      next.lime.configured = false;
      next.lime.available = false;
    }

    {
      std::lock_guard<std::mutex> lock(data_mutex);
      board_data = next;
    }

    const int chunks = std::max(1, refresh_seconds * 10);
    for (int i = 0; i < chunks && fetch_thread_running.load(); ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
  }
}

static int text_width(const rgb_matrix::Font &font,
                      const std::string &text,
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

static std::string fit_text(const rgb_matrix::Font &font,
                            std::string text,
                            int max_width,
                            int letter_spacing) {
  if (max_width <= 0) {
    return "";
  }

  if (text_width(font, text, letter_spacing) <= max_width) {
    return text;
  }

  const std::string suffix = "..";

  while (!text.empty()) {
    text.pop_back();
    if (text_width(font, text + suffix, letter_spacing) <= max_width) {
      return text + suffix;
    }
  }

  return "";
}

static std::string minutes_text(int seconds_until) {
  if (seconds_until <= 30) {
    return "Due";
  }

  const int minutes = std::max(1, (seconds_until + 30) / 60);
  return std::to_string(minutes) + "m";
}

static std::string current_time_text() {
  char buffer[16];
  const time_t now = time(nullptr);
  struct tm local_tm;
  localtime_r(&now, &local_tm);
  strftime(buffer, sizeof(buffer), "%H:%M", &local_tm);
  return buffer;
}

static void draw_header(FrameCanvas *canvas,
                        const rgb_matrix::Font &font,
                        const Color &color,
                        const std::string &title,
                        int letter_spacing) {
  const int width = canvas->width();
  const std::string clock = current_time_text();
  const int clock_width = text_width(font, clock, letter_spacing);
  const int gap = 4;

  const std::string fitted_title =
      fit_text(font, title, width - clock_width - gap, letter_spacing);

  rgb_matrix::DrawText(
      canvas, font,
      0, font.baseline(),
      color, nullptr, fitted_title.c_str(), letter_spacing);

  rgb_matrix::DrawText(
      canvas, font,
      width - clock_width, font.baseline(),
      color, nullptr, clock.c_str(), letter_spacing);
}

static int first_body_baseline(const rgb_matrix::Font &font) {
  return font.height() + 2 + font.baseline();
}

static bool can_draw_baseline(FrameCanvas *canvas,
                              const rgb_matrix::Font &font,
                              int baseline) {
  const int top = baseline - font.baseline();
  return top >= 0 && top + font.height() <= canvas->height();
}

static void draw_departure_row(FrameCanvas *canvas,
                               const rgb_matrix::Font &font,
                               const Color &color,
                               int baseline,
                               const Departure &departure,
                               int letter_spacing) {
  const std::string due = minutes_text(departure.seconds_until);
  const int due_width = text_width(font, due, letter_spacing);
  const int gap = 5;

  const std::string label =
      fit_text(font, departure.label,
               canvas->width() - due_width - gap,
               letter_spacing);

  rgb_matrix::DrawText(
      canvas, font,
      0, baseline,
      color, nullptr, label.c_str(), letter_spacing);

  rgb_matrix::DrawText(
      canvas, font,
      canvas->width() - due_width, baseline,
      color, nullptr, due.c_str(), letter_spacing);
}

static void draw_message_row(FrameCanvas *canvas,
                             const rgb_matrix::Font &font,
                             const Color &color,
                             int baseline,
                             const std::string &message,
                             int letter_spacing) {
  const std::string fitted =
      fit_text(font, message, canvas->width(), letter_spacing);

  rgb_matrix::DrawText(
      canvas, font,
      0, baseline,
      color, nullptr, fitted.c_str(), letter_spacing);
}

static void draw_departure_screen(FrameCanvas *canvas,
                                  const rgb_matrix::Font &font,
                                  const Color &color,
                                  const std::string &title,
                                  bool available,
                                  const std::vector<Departure> &departures,
                                  const std::string &empty_message,
                                  int letter_spacing) {
  draw_header(canvas, font, color, title, letter_spacing);

  int baseline = first_body_baseline(font);
  const int step = font.height() + 1;

  if (!available) {
    if (can_draw_baseline(canvas, font, baseline)) {
      draw_message_row(
          canvas, font, color, baseline, "Waiting for data", letter_spacing);
    }
    return;
  }

  if (departures.empty()) {
    if (can_draw_baseline(canvas, font, baseline)) {
      draw_message_row(
          canvas, font, color, baseline, empty_message, letter_spacing);
    }
    return;
  }

  for (const Departure &departure : departures) {
    if (!can_draw_baseline(canvas, font, baseline)) {
      break;
    }

    draw_departure_row(
        canvas, font, color, baseline, departure, letter_spacing);

    baseline += step;
  }
}

static void draw_lime_screen(FrameCanvas *canvas,
                             const rgb_matrix::Font &font,
                             const Color &color,
                             const LimeStatus &lime,
                             int letter_spacing) {
  draw_header(canvas, font, color, "LIME BIKES", letter_spacing);

  int baseline = first_body_baseline(font);
  const int step = font.height() + 1;

  if (!lime.configured) {
    if (can_draw_baseline(canvas, font, baseline)) {
      draw_message_row(
          canvas, font, color, baseline,
          "Set HOME_LAT + HOME_LON", letter_spacing);
    }
    return;
  }

  if (!lime.available) {
    if (can_draw_baseline(canvas, font, baseline)) {
      draw_message_row(
          canvas, font, color, baseline,
          "Waiting for Lime data", letter_spacing);
    }
    return;
  }

  std::ostringstream nearby;
  nearby << lime.nearby_count << " within "
         << static_cast<int>(LIME_RADIUS_METRES) << "m";

  if (can_draw_baseline(canvas, font, baseline)) {
    draw_message_row(
        canvas, font, color, baseline, nearby.str(), letter_spacing);
  }

  baseline += step;

  if (can_draw_baseline(canvas, font, baseline)) {
    if (lime.nearest_distance_m > 0.0) {
      std::ostringstream nearest;
      nearest << "Nearest "
              << static_cast<int>(std::round(lime.nearest_distance_m))
              << "m " << lime.nearest_bearing;

      draw_message_row(
          canvas, font, color, baseline, nearest.str(), letter_spacing);
    } else {
      draw_message_row(
          canvas, font, color, baseline, "No bikes reported", letter_spacing);
    }
  }
}

static int usage(const char *progname) {
  fprintf(stderr, "usage: %s [options] [matrix options]\n", progname);
  fprintf(stderr,
          "Options:\n"
          "\t-f <font-file>       : BDF font used for all text (required)\n"
          "\t-C <r,g,b>           : Text colour (Default: 250,184,0)\n"
          "\t-S <spacing>         : Extra letter spacing (Default: 0)\n"
          "\t-d <seconds>         : Seconds per screen (Default: 10)\n"
          "\t-r <seconds>         : Data refresh interval (Default: 20)\n"
          "\n"
          "Environment:\n"
          "\tHOME_LAT             : Centre latitude for Lime radius\n"
          "\tHOME_LON             : Centre longitude for Lime radius\n"
          "\tTFL_APP_ID           : Optional TfL application id\n"
          "\tTFL_APP_KEY          : Optional TfL application key\n"
          "\n");
  rgb_matrix::PrintMatrixFlags(stderr);
  return 1;
}

static bool parse_color(Color *color, const char *value) {
  return sscanf(value, "%hhu,%hhu,%hhu",
                &color->r, &color->g, &color->b) == 3;
}

int main(int argc, char *argv[]) {
  RGBMatrix::Options matrix_options;
  rgb_matrix::RuntimeOptions runtime_options;

  if (!rgb_matrix::ParseOptionsFromFlags(
          &argc, &argv, &matrix_options, &runtime_options)) {
    return usage(argv[0]);
  }

  setenv("TZ", "Europe/London", 1);
  tzset();

  const char *font_file = nullptr;
  Color text_color(250, 184, 0);
  Color background_color(0, 0, 0);

  int letter_spacing = 0;
  int screen_seconds = 10;
  int refresh_seconds = 20;

  int opt;
  while ((opt = getopt(argc, argv, "f:C:S:d:r:")) != -1) {
    switch (opt) {
      case 'f':
        font_file = optarg;
        break;
      case 'C':
        if (!parse_color(&text_color, optarg)) {
          fprintf(stderr, "Invalid colour: %s\n", optarg);
          return usage(argv[0]);
        }
        break;
      case 'S':
        letter_spacing = atoi(optarg);
        break;
      case 'd':
        screen_seconds = atoi(optarg);
        if (screen_seconds < 1) {
          fprintf(stderr, "Screen duration must be at least 1 second.\n");
          return usage(argv[0]);
        }
        break;
      case 'r':
        refresh_seconds = atoi(optarg);
        if (refresh_seconds < 5) {
          fprintf(stderr, "Refresh interval must be at least 5 seconds.\n");
          return usage(argv[0]);
        }
        break;
      default:
        return usage(argv[0]);
    }
  }

  if (!font_file) {
    fprintf(stderr, "Need to specify a BDF font using -f\n");
    return usage(argv[0]);
  }

  rgb_matrix::Font font;
  if (!font.LoadFont(font_file)) {
    fprintf(stderr, "Couldn't load font '%s'\n", font_file);
    return 1;
  }

  double home_lat = 0.0;
  double home_lon = 0.0;

  const bool have_lat = getenv_double("HOME_LAT", &home_lat);
  const bool have_lon = getenv_double("HOME_LON", &home_lon);
  const bool lime_configured =
      have_lat && have_lon
      && home_lat >= -90.0 && home_lat <= 90.0
      && home_lon >= -180.0 && home_lon <= 180.0;

  if (!lime_configured) {
    std::cerr
        << "HOME_LAT/HOME_LON are not configured; Lime screen will explain "
        << "that configuration is required." << std::endl;
  }

  if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
    fprintf(stderr, "Could not initialise libcurl\n");
    return 1;
  }

  RGBMatrix *matrix =
      RGBMatrix::CreateFromOptions(matrix_options, runtime_options);

  if (!matrix) {
    curl_global_cleanup();
    return 1;
  }

  FrameCanvas *offscreen = matrix->CreateFrameCanvas();

  signal(SIGTERM, InterruptHandler);
  signal(SIGINT, InterruptHandler);

  {
    std::lock_guard<std::mutex> lock(data_mutex);
    board_data.lime.configured = lime_configured;
  }

  std::thread fetch_thread(
      fetch_loop,
      refresh_seconds,
      lime_configured,
      home_lat,
      home_lon);

  int screen_index = 0;
  time_t next_screen_change = time(nullptr) + screen_seconds;

  while (!interrupt_received) {
    const time_t now = time(nullptr);

    if (now >= next_screen_change) {
      screen_index = (screen_index + 1) % 3;
      next_screen_change = now + screen_seconds;
    }

    BoardData snapshot;
    {
      std::lock_guard<std::mutex> lock(data_mutex);
      snapshot = board_data;
    }

    offscreen->Fill(
        background_color.r,
        background_color.g,
        background_color.b);

    if (screen_index == 0) {
      draw_departure_screen(
          offscreen,
          font,
          text_color,
          "BRUCE GROVE S/B",
          snapshot.trains_available,
          snapshot.trains,
          "No southbound trains",
          letter_spacing);
    } else if (screen_index == 1) {
      draw_departure_screen(
          offscreen,
          font,
          text_color,
          "AWLFIELD AVE",
          snapshot.buses_available,
          snapshot.buses,
          "No buses due",
          letter_spacing);
    } else {
      draw_lime_screen(
          offscreen,
          font,
          text_color,
          snapshot.lime,
          letter_spacing);
    }

    offscreen = matrix->SwapOnVSync(offscreen);
  }

  fetch_thread_running.store(false);

  if (fetch_thread.joinable()) {
    fetch_thread.join();
  }

  delete matrix;
  curl_global_cleanup();

  write(STDOUT_FILENO, "\n", 1);
  return 0;
}
