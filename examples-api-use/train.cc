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

using namespace rgb_matrix;

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
  bool with_outline = false;

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

  int speed = 1;
  int delay_speed_usec = 1000000;
  if (speed > 0) {
    delay_speed_usec = 1000000 / speed / font.CharacterWidth('W');
  } else {
    // There would be no scrolling, so text would never appear. Move to front.
    x_orig = 0;
  }

  int x_mid = x_orig;
  int middle_length = 0;
  // add a string variable called 'line'
  std::string line;

  while (!interrupt_received) {
    offscreen->Fill(bg_color.r, bg_color.g, bg_color.b);

    int line_offset = 0;
    strncpy(text_buffer, "1st 14:03 Leeds               Exp 14:05", 192);

    rgb_matrix::DrawText(offscreen, font,
                        x, y + font.baseline() + line_offset,
                        color, NULL, text_buffer,
                        letter_spacing);
    line_offset += font.height() + line_spacing;

    line = "This train is formed of 4 carriages. Calling at: Horsforth, Longlevens, Elmore Court, Tottenham, Monaco and the Moon. Doesn't stop at Leeds.";

    // offscreen_canvas_middle->Fill(bg_color.r, bg_color.g, bg_color.b);
    // length = holds how many pixels our text takes up
    middle_length = rgb_matrix::DrawText(offscreen, font,
                                  x_mid, y + font.baseline() + line_offset,
                                  color, nullptr,
                                  line.c_str(), letter_spacing);

    line_offset += font.height() + line_spacing;

    if (speed > 0 && --x_mid + middle_length < 0) {
      x_mid = x_orig;
    }

    strncpy(text_buffer, "2nd 14:18 Leeds                On Time", 192);

    rgb_matrix::DrawText(offscreen, font,
                        x, y + font.baseline() + line_offset,
                        color, NULL, text_buffer,
                        letter_spacing);
    line_offset += font.height() + line_spacing;

    // Atomic swap with double buffer
    offscreen = matrix->SwapOnVSync(offscreen);
  }

  // Finished. Shut down the RGB matrix.
  delete matrix;

  write(STDOUT_FILENO, "\n", 1);  // Create a fresh new line after ^C on screen
  return 0;
}
