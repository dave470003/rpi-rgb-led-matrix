#include "led-matrix.h"
#include "graphics.h"

#include <getopt.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <vector>
#include <string>
#include <algorithm>
#include <curl/curl.h>
#include <iostream>
#include <nlohmann/json.hpp>

using namespace rgb_matrix;
using json = nlohmann::json;

// create objectData class. holds two text fields and twod atetime fields.
class ObjectData {
public:
  std::string destination;
  std::string description;
  time_t scheduled_time;
  time_t estimated_time;


// need a function to return scheduled_time as a formatted string, based on the BST timezone
std::string get_scheduled_time() {
  char buf[32];
  struct tm tm_info;

  // Force UK timezone (handles GMT/BST automatically)
  setenv("TZ", "Europe/London", 1);
  tzset();  // reload timezone info

  localtime_r(&scheduled_time, &tm_info);
  strftime(buf, sizeof(buf), "%H:%M", &tm_info);

  return std::string(buf);
}
// need a function to return estimated_time as a formatted string
std::string get_estimated_time() {
  char buf[32];
  struct tm tm_info;

  // Force UK timezone (handles GMT/BST automatically)
  setenv("TZ", "Europe/London", 1);
  tzset();  // reload timezone info

  localtime_r(&estimated_time, &tm_info);
  strftime(buf, sizeof(buf), "%H:%M", &tm_info);

  return std::string(buf);
}
};

volatile bool interrupt_received = false;
static void InterruptHandler(int signo) {
  interrupt_received = true;
}

static bool parseColor(Color *c, const char *str) {
  return sscanf(str, "%hhu,%hhu,%hhu", &c->r, &c->g, &c->b) == 3;
}

static bool FullSaturation(const Color &c) {
  return (c.r == 0 || c.r == 255)
    && (c.g == 0 || c.g == 255)
    && (c.b == 0 || c.b == 255);
}

size_t WriteMemoryCallback(void* contents, size_t size, size_t nmemb, void* userp) {
  ((std::string*)userp)->append((char*)contents, size * nmemb);
  return size * nmemb;
}

/**
 * Entry point for the program. Initializes the RGB LED matrix with options
 * parsed from command line flags and creates a Train instance to display
 * scrolling text. The program runs the Train instance to show text on the
 * LED matrix, then stops and cleans up resources before exiting.
 *
 * @param argc Number of command-line arguments.
 * @param argv Array of command-line arguments.
 * @return Exit code for the program, 0 on success, 1 if initialization fails.
 */

static int usage(const char *progname) {
  fprintf(stderr, "Hello if you're using this! I recommend you use the following options: --led-no-hardware-pulse --led-no-hardware-pulse --led-gpio-mapping=adafruit-hat examples-api-use/runtext.ppm --led-cols=192 --led-slowdown-gpio=2\n");
  fprintf(stderr, "usage: %s [options]\n", progname);
  fprintf(stderr, "Reads text from stdin and displays it. "
          "Empty string: clear screen\n");
  fprintf(stderr, "Options:\n");
  fprintf(stderr,
          "\t                    Can be provided multiple times for multiple "
          "lines\n"
          "\t-f <font-file>    : Use given font.\n"
          "\t-x <x-origin>     : X-Origin of displaying text (Default: 0)\n"
          "\t-y <y-origin>     : Y-Origin of displaying text (Default: 0)\n"
          "\t-s <line-spacing> : Extra spacing between lines when multiple -d given\n"
          "\t-S <spacing>      : Extra spacing between letters (Default: 0)\n"
          "\t-C <r,g,b>        : Color. Default 255,255,0\n"
          "\t-B <r,g,b>        : Background-Color. Default 0,0,0\n"
          "\n"
          );
  rgb_matrix::PrintMatrixFlags(stderr);
  return 1;
}

// create a new function called "pull_from_api"
json pull_from_api() {
  std::string json_string;
  curl_global_init(CURL_GLOBAL_DEFAULT);
  CURL* curl = curl_easy_init();

  if (curl) {
      std::string url = "https://freddyanddavid.com/api/config";
      curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
      curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteMemoryCallback);
      curl_easy_setopt(curl, CURLOPT_WRITEDATA, &json_string);

      CURLcode res = curl_easy_perform(curl);
      if (res != CURLE_OK) {
          std::cerr << "cURL error: " << curl_easy_strerror(res) << std::endl;
      }

      curl_easy_cleanup(curl);
  }
  curl_global_cleanup();

  // Parse JSON
  json root;
  try {
      printf("JSON: %s\n", json_string.c_str());
      root = json::parse(json_string);
  } catch (json::parse_error& e) {
      std::cerr << "Failed to parse JSON: " << e.what() << std::endl;
  }

  return root;
}

int main(int argc, char *argv[]) {
  RGBMatrix::Options matrix_options;
  rgb_matrix::RuntimeOptions runtime_opt;
  if (!rgb_matrix::ParseOptionsFromFlags(&argc, &argv,
                                         &matrix_options, &runtime_opt)) {
    return usage(argv[0]);
  }

  Color color(255, 255, 0);
  Color bg_color(0, 0, 0);
  Color outline_color(0,0,0);

  const char *bdf_font_file = NULL;
  int x_orig = 0;
  int y_orig = 0;
  int letter_spacing = 0;
  int line_spacing = 0;

  int opt;
  while ((opt = getopt(argc, argv, "x:y:f:C:B:O:s:S:d:")) != -1) {
    switch (opt) {
    case 'x': x_orig = atoi(optarg); break;
    case 'y': y_orig = atoi(optarg); break;
    case 'f': bdf_font_file = strdup(optarg); break;
    case 's': line_spacing = atoi(optarg); break;
    case 'S': letter_spacing = atoi(optarg); break;
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

  /*
   * Load font. This needs to be a filename with a bdf bitmap font.
   */
  rgb_matrix::Font font;
  if (!font.LoadFont(bdf_font_file)) {
    fprintf(stderr, "Couldn't load font '%s'\n", bdf_font_file);
    return 1;
  }

  rgb_matrix::Font time_font;
  if (!time_font.LoadFont("fonts/dotbold.bdf")) {
    fprintf(stderr, "Couldn't load time font fonts/dotbold.bdf");
    return 1;
  }

  RGBMatrix *matrix = RGBMatrix::CreateFromOptions(matrix_options, runtime_opt);
  if (matrix == NULL)
    return 1;

  const bool all_extreme_colors = (matrix_options.brightness == 100)
    && FullSaturation(color)
    && FullSaturation(bg_color)
    && FullSaturation(outline_color);
  if (all_extreme_colors)
    matrix->SetPWMBits(1);

  const int x = x_orig;
  int y = y_orig;

  FrameCanvas *offscreen = matrix->CreateFrameCanvas();

  // Create a new canvas to be used with led_matrix_swap_on_vsync
  // FrameCanvas *offscreen_canvas_middle = matrix->CreateFrameCanvas();

  char text_buffer[256];

  signal(SIGTERM, InterruptHandler);
  signal(SIGINT, InterruptHandler);

  auto get_string_or_empty = [](const json& j, const std::string& key) -> std::string {
    if (j.contains(key) && !j[key].is_null()) return j[key].get<std::string>();
    return "";
  };

  // Pull JSON from API
  json root = pull_from_api();

  // Extract arrays and values
  json jokes = root.value("jokes", json::array());
  json affirmations = root.value("affirmations", json::array());
  json events = root.value("events", json::array());
  json slideshow = root.value("slideshow", json::array());
  json gifs = root.value("gifs", json::array());
  json comments = root.value("comments", json::array());
  std::string important_message = get_string_or_empty(root, "importantMessage");
  std::string mode = get_string_or_empty(root, "mode");
  json secrets = root.value("secrets", json::array());

  std::string main_screen;
  std::string current_screen;
  std::string last_alt_screen;
  // make an array called alt_screens;
  std::vector<std::string> alt_screens;

  std::string message;

  //switch on mode
  if (mode == "events_only") {
    main_screen = "events";
  } else if (mode == "important_message_only") {
    main_screen = "importantMessage";
  } else {
    main_screen = "events";

    //if important_message is not empty
    if (!important_message.empty()) {
      alt_screens.push_back("importantMessage");
    } else {
      if (jokes.size() > 0) {
        alt_screens.push_back("jokes");
      }
      if (affirmations.size() > 0) {
        alt_screens.push_back("affirmations");
      }
      if (slideshow.size() > 0) {
        alt_screens.push_back("slideshow");
      }
      if (gifs.size() > 0) {
        alt_screens.push_back("gifs");
      }
      if (comments.size() > 0) {
        alt_screens.push_back("comments");
      }
      if (secrets.size() > 0) {
        alt_screens.push_back("secrets");
      }
    }
  }

  current_screen = main_screen;

  // Convert events to ObjectData vector
  std::vector<ObjectData> objects;
  for (auto& event : events) {
      ObjectData object;
      object.destination = event.value("destination", "");
      object.description = event.value("description", "");
      object.scheduled_time = event.value("scheduled_time", 0);
      object.estimated_time = event.value("estimated_time", 0);
      objects.push_back(object);
  }

  /* x_origin is set by default just right of the screen */
  const int x_mid_default_start = (matrix_options.chain_length
    * matrix_options.cols) + 5;
  int x_mid_orig = x_mid_default_start;
  int scroll_speed = 30000;
  float screen_speed = 300;
  float fetch_speed = 100;

  int delay_speed_usec = 1000000;
  if (scroll_speed > 0) {
    delay_speed_usec = 1000000 / scroll_speed / font.CharacterWidth('W');
  } else if (x_mid_orig == x_mid_default_start) {
    // There would be no scrolling, so text would never appear. Move to front.
    x_mid_orig = 0;
  }

  int x_mid = x_mid_orig;
  int middle_length = 0;
  // add a string variable called 'line'
  std::string line;

  // an array of object data
  // std::vector<ObjectData> objects;

  // // push a new objectData to objects
  // objects.push_back(ObjectData{"Leeds", "This train is formed of 4 carriages. Calling at: Horsforth, Longlevens, Elmore Court, Tottenham, Monaco and the Moon. Doesn't stop at Leeds.", time(nullptr), time(nullptr)});

  // sort objects by scheduled time
  std::sort(objects.begin(), objects.end(), [](const ObjectData& a, const ObjectData& b) {
    return a.scheduled_time < b.scheduled_time;
  });

  // filter objects that have an estimated time in the future from a given date
  // set the given date as 2026-09-05 14:20
  // Force UK timezone (handles GMT/BST automatically)
  setenv("TZ", "Europe/London", 1);
  tzset();  // reload timezone info

  time_t now = time(nullptr);
  struct tm now_tm = *localtime(&now);
  now_tm.tm_year = 2026 - 1900;
  now_tm.tm_mon = 9 - 1;
  now_tm.tm_mday = 5;
  now_tm.tm_hour = 15;
  now_tm.tm_min = 25;
  now = mktime(&now_tm);

  objects.erase(std::remove_if(objects.begin(), objects.end(), [now](const ObjectData& object) {
    return object.estimated_time < now;
  }), objects.end());

  int scroll_l = 0;
  int screen_l = 0;
  int fetch_l = 0;

  while (!interrupt_received) {
    offscreen->Fill(bg_color.r, bg_color.g, bg_color.b);

    int line_offset = 0;

    if (fetch_l > (1000000 / fetch_speed)) {
      printf("Fetching new data\n");
      fetch_l = 0;
      // Pull JSON from API
      root = pull_from_api();

      // Extract arrays and values
      jokes = root.value("jokes", json::array());
      affirmations = root.value("affirmations", json::array());
      events = root.value("events", json::array());
      slideshow = root.value("slideshow", json::array());
      gifs = root.value("gifs", json::array());
      comments = root.value("comments", json::array());
      important_message = get_string_or_empty(root, "importantMessage");
      mode = get_string_or_empty(root, "mode");
      secrets = root.value("secrets", json::array());

      // Convert events to ObjectData vector
      for (auto& event : events) {
          ObjectData object;
          object.destination = event.value("destination", "");
          object.description = event.value("description", "");
          object.scheduled_time = event.value("scheduled_time", 0);
          object.estimated_time = event.value("estimated_time", 0);
          objects.push_back(object);
      }

      //switch on mode
      if (mode == "events_only") {
        main_screen = "events";
      } else if (mode == "important_message_only") {
        main_screen = "importantMessage";
      } else {
        main_screen = "events";

        //if important_message is not empty
        if (!important_message.empty()) {
          alt_screens.push_back("importantMessage");
        } else {
          if (jokes.size() > 0) {
            alt_screens.push_back("jokes");
          }
          if (affirmations.size() > 0) {
            alt_screens.push_back("affirmations");
          }
          if (slideshow.size() > 0) {
            alt_screens.push_back("slideshow");
          }
          if (gifs.size() > 0) {
            alt_screens.push_back("gifs");
          }
          if (comments.size() > 0) {
            alt_screens.push_back("comments");
          }
          if (secrets.size() > 0) {
            alt_screens.push_back("secrets");
          }
        }
      }
    }

    if (screen_l > (1000000 / screen_speed)) {
      screen_l = 0;

      if (alt_screens.size() == 0) {
        current_screen = main_screen;
      } else if (current_screen != main_screen) {
        last_alt_screen = current_screen;
        current_screen = main_screen;
      //} else if (last_alt_screen is empty
      } else if (last_alt_screen.empty()) {
        current_screen = alt_screens[0];
      } else {
        // find in array
        auto it = std::find(alt_screens.begin(), alt_screens.end(), last_alt_screen);
        if (it == alt_screens.end() || ++it == alt_screens.end()) {
            current_screen = alt_screens[0];
        } else {
            current_screen = *it;
        }
      }
    }

    if (current_screen == "events") {

      // if objects has at least one object, grab the first one
      if (!objects.empty()) {
        std::string sch_text_1 = "1st " + objects[0].get_scheduled_time();
        strncpy(text_buffer, sch_text_1.c_str(), 192);

        rgb_matrix::DrawText(offscreen, font,
                            x, y + font.baseline() + line_offset,
                            color, NULL, text_buffer,
                            letter_spacing);

        std::string dest_text_1 = objects[0].destination;
        strncpy(text_buffer, dest_text_1.c_str(), 192);

        rgb_matrix::DrawText(offscreen, font,
                            x + 45, y + font.baseline() + line_offset,
                            color, NULL, text_buffer,
                            letter_spacing);

        std::string est_text_1 = "Exp " + objects[0].get_estimated_time();
        strncpy(text_buffer, est_text_1.c_str(), 192);

        rgb_matrix::DrawText(offscreen, font,
                            192 - 45, y + font.baseline() + line_offset,
                            color, NULL, text_buffer,
                            letter_spacing);
        line_offset += font.height() + line_spacing;


        line = objects[0].description;

        // offscreen_canvas_middle->Fill(bg_color.r, bg_color.g, bg_color.b);
        // length = holds how many pixels our text takes up
        middle_length = rgb_matrix::DrawText(offscreen, font,
                                      x_mid, y + font.baseline() + line_offset,
                                      color, nullptr,
                                      line.c_str(), letter_spacing);

        line_offset += font.height() + line_spacing;

        if (scroll_l > (1000000 / scroll_speed)) {
          if (scroll_speed > 0 && --x_mid + middle_length < 0) {
            x_mid = x_mid_orig;
          }
          scroll_l = 0;
        }

        // if there's a second object, grab it
        if (objects.size() > 1) {
          std::string sch_text_1 = "2nd " + objects[1].get_scheduled_time();
          strncpy(text_buffer, sch_text_1.c_str(), 192);

          rgb_matrix::DrawText(offscreen, font,
                              x, y + font.baseline() + line_offset,
                              color, NULL, text_buffer,
                              letter_spacing);

          std::string dest_text_1 = objects[1].destination;
          strncpy(text_buffer, dest_text_1.c_str(), 192);

          rgb_matrix::DrawText(offscreen, font,
                              x + 45, y + font.baseline() + line_offset,
                              color, NULL, text_buffer,
                              letter_spacing);

          std::string est_text_1 = "Exp " + objects[1].get_estimated_time();
          strncpy(text_buffer, est_text_1.c_str(), 192);

          rgb_matrix::DrawText(offscreen, font,
                              192 - 45, y + font.baseline() + line_offset,
                              color, NULL, text_buffer,
                              letter_spacing);
          line_offset += font.height() + line_spacing;
        } else {
          // display the current time
          time_t now = time(nullptr);
          strftime(text_buffer, sizeof(text_buffer), "%H:%M:%S", localtime(&now));
          rgb_matrix::DrawText(offscreen, time_font,
                              x + 58, y + time_font.baseline() + line_offset,
                              color, NULL, text_buffer,
                              letter_spacing);
          line_offset += font.height() + line_spacing;
        }
      } else {
        // display the current time
        time_t now = time(nullptr);
        line_offset += font.height() + line_spacing;

        strftime(text_buffer, sizeof(text_buffer), "%H:%M:%S", localtime(&now));
        rgb_matrix::DrawText(offscreen, time_font,
                            x + 58, y + time_font.baseline() + line_offset,
                            color, NULL, text_buffer,
                            letter_spacing);
        line_offset += font.height() + line_spacing;
      }
    } else if (current_screen == "jokes") {
      // the jokes array consists of an array of objects, one property is message. grab the message from a random joke.

      int index = rand() % jokes.size();

      if (jokes[index].contains("message") && jokes[index]["message"].is_string()) {
          message = jokes[index]["message"].get<std::string>();
      }

      line_offset += font.height() + line_spacing;

      middle_length = rgb_matrix::DrawText(offscreen, font,
        x_mid, y + font.baseline() + line_offset,
        color, nullptr,
        message.c_str(), letter_spacing);

      if (scroll_l > (1000000 / scroll_speed)) {
        if (scroll_speed > 0 && --x_mid + middle_length < 0) {
          x_mid = x_mid_orig;
        }
        scroll_l = 0;
      }
    } else if (current_screen == "affirmations") {
      // the affirmations array consists of an array of objects, one property is message. grab the message from a random joke.
      int index = rand() % affirmations.size();

      if (affirmations[index].contains("message") && affirmations[index]["message"].is_string()) {
          message = affirmations[index]["message"].get<std::string>();
      }

      line_offset += font.height() + line_spacing;

      middle_length = rgb_matrix::DrawText(offscreen, font,
        x_mid, y + font.baseline() + line_offset,
        color, nullptr,
        message.c_str(), letter_spacing);

      if (scroll_l > (1000000 / scroll_speed)) {
        if (scroll_speed > 0 && --x_mid + middle_length < 0) {
          x_mid = x_mid_orig;
        }
        scroll_l = 0;
      }
    }

    // Atomic swap with double buffer
    offscreen = matrix->SwapOnVSync(offscreen);
    scroll_l++;
    screen_l++;
    fetch_l++;

    // print scroll_l
    std::cout << "scroll_l: " << scroll_l << std::endl;

    // usleep(delay_speed_usec);
  }

  // Finished. Shut down the RGB matrix.
  delete matrix;

  write(STDOUT_FILENO, "\n", 1);  // Create a fresh new line after ^C on screen
  return 0;
}
