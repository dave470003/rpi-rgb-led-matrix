#include "led-matrix.h"
#include "graphics.h"

#include <getopt.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <curl/curl.h>
#include <fstream>
#include <iostream>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>
#include <thread>
#include <vector>

using namespace rgb_matrix;
using json = nlohmann::json;

static const char *API_URL = "https://freddyanddavid.com/api/config";
static const char *CACHE_FILE = "config-cache.json";

static volatile sig_atomic_t interrupt_received = 0;

static std::mutex api_mutex;
static json latest_api_data = json::object();
static std::atomic<bool> new_api_data(false);
static std::atomic<bool> api_thread_running(true);

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

static std::string get_string_or_empty(const json &j, const std::string &key) {
  if (j.contains(key) && j[key].is_string()) {
    return j[key].get<std::string>();
  }
  return "";
}

static time_t get_time_value(const json &event, const char *primary_key,
                             const char *fallback_key) {
  try {
    if (event.contains(primary_key) && event[primary_key].is_number()) {
      return event[primary_key].get<time_t>();
    }
    if (event.contains(fallback_key) && event[fallback_key].is_number()) {
      return event[fallback_key].get<time_t>();
    }
  } catch (const json::exception &) {
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

static std::string first_event_key(const std::vector<ObjectData> &objects) {
  if (objects.empty()) {
    return "";
  }

  const ObjectData &object = objects.front();
  return std::to_string(static_cast<long long>(object.scheduled_time))
      + "|" + object.destination
      + "|" + object.description;
}

static std::vector<ObjectData> build_events(const json &events) {
  std::vector<ObjectData> objects;
  const time_t now = time(nullptr);

  if (!events.is_array()) {
    return objects;
  }

  for (const auto &event : events) {
    if (!event.is_object()) {
      continue;
    }

    ObjectData object;
    object.destination = event.value("destination", "");
    object.description = event.value("description", "");

    // Support both the current *_time names and the *_ts names from the API spec.
    object.scheduled_time = get_time_value(event, "scheduled_time", "scheduled_ts");
    object.estimated_time = get_time_value(event, "estimated_time", "estimated_ts");

    // Only future events are shown. The estimated time is the authoritative
    // time for whether an event is still relevant.
    if (object.estimated_time > now) {
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

static void api_fetch_loop(int interval_seconds) {
  while (api_thread_running.load()) {
    json fetched;

    if (fetch_from_api(fetched)) {
      // Saving happens on the background thread too, so neither the network
      // nor the disk write can pause the display/render loop.
      if (!save_cache(fetched)) {
        std::cerr << "Warning: fetched API data but could not update cache"
                  << std::endl;
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

  if (mode == "events_only" || mode == "important_message_only") {
    return alt_screens;
  }

  // Preserve the existing behaviour: an important message takes over the
  // alternate slot while one is present.
  if (!important_message.empty()) {
    alt_screens.push_back("importantMessage");
    return alt_screens;
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
          "\t-C <r,g,b>           : Text colour (Default: 250,184,0)\n"
          "\t-B <r,g,b>           : Background colour (Default: 0,0,0)\n"
          "\n");
  rgb_matrix::PrintMatrixFlags(stderr);
  return 1;
}

int main(int argc, char *argv[]) {
  RGBMatrix::Options matrix_options;
  rgb_matrix::RuntimeOptions runtime_opt;

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

  int opt;
  while ((opt = getopt(argc, argv, "x:y:f:C:B:O:s:S:v:r:")) != -1) {
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

  json jokes = root.value("jokes", json::array());
  json affirmations = root.value("affirmations", json::array());
  json events = root.value("events", json::array());
  json slideshow = root.value("slideshow", json::array());
  json gifs = root.value("gifs", json::array());
  json comments = root.value("comments", json::array());
  json secrets = root.value("secrets", json::array());

  std::string important_message = get_string_or_empty(root, "importantMessage");
  std::string mode = get_string_or_empty(root, "mode");
  bool enable_secret_screen = root.value("enable_secret_screen", false);

  std::vector<ObjectData> objects = build_events(events);
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
      if (!objects.empty()) {
        next_screen_update = std::min(
            objects.front().estimated_time,
            now + event_screen_interval);
      } else {
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
    api_thread = std::thread(api_fetch_loop, fetch_interval);
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

      root = new_root;
      jokes = root.value("jokes", json::array());
      affirmations = root.value("affirmations", json::array());
      events = root.value("events", json::array());
      slideshow = root.value("slideshow", json::array());
      gifs = root.value("gifs", json::array());
      comments = root.value("comments", json::array());
      secrets = root.value("secrets", json::array());
      important_message = get_string_or_empty(root, "importantMessage");
      mode = get_string_or_empty(root, "mode");
      enable_secret_screen = root.value("enable_secret_screen", false);

      objects = build_events(events);
      main_screen = main_screen_for_mode(mode);
      alt_screens = build_alt_screens(
          mode,
          important_message,
          jokes,
          affirmations,
          comments,
          secrets,
          enable_secret_screen);

      // If the event at the top of the board changed, restart its description
      // from the right so the new text is guaranteed a complete pass.
      if (current_screen == "events"
          && old_first_event != first_event_key(objects)) {
        reset_scroll();
        set_screen_deadline();
      }
    }

    // Events can expire between API updates. Remove them immediately so an
    // event whose estimated time has passed is never displayed.
    {
      const std::string old_first_event = first_event_key(objects);
      const time_t now = time(nullptr);

      objects.erase(
          std::remove_if(
              objects.begin(),
              objects.end(),
              [now](const ObjectData &object) {
                return object.estimated_time <= now;
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
        // No future events: retain the clock display on the event screen.
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
