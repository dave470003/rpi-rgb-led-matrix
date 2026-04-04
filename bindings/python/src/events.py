#!/usr/bin/env python
# Display a runtext with double-buffering.
from samplebase import SampleBase
from rgbmatrix import graphics
import time


class RunText(SampleBase):
    nameOffset = 36
    timeOffset = 18
    etaOffset = 147

    def __init__(self, *args, **kwargs):
        super(RunText, self).__init__(*args, **kwargs)
        self.parser.add_argument("-t", "--text", help="The text to scroll on the RGB LED panel", default="Hello world!")

    def run(self):
        offscreen_canvas = self.matrix.CreateFrameCanvas()
        font = graphics.Font()
        font.LoadFont("../../../fonts/dottrain.bdf")
        textColor = graphics.Color(255, 255, 255)
        event0DescPos = offscreen_canvas.width
        events = self.args.events

        while True:
            if events[0] is not None:
                offscreen_canvas.Clear()
                graphics.DrawText(offscreen_canvas, font, 0, 1, textColor, "1st")
                graphics.DrawText(offscreen_canvas, font, self.timeOffset, 1, textColor, events[0].time)
                graphics.DrawText(offscreen_canvas, font, self.nameOffset, 1, textColor, events[0].name)
                graphics.DrawText(offscreen_canvas, font, self.etaOffset, 1, textColor, events[0].eta)

                len = graphics.DrawText(offscreen_canvas, font, event0DescPos, 10, textColor, events[0].description[0])
                event0DescPos -= 1
                if (event0DescPos + len < 0):
                    event0DescPos = offscreen_canvas.width

            if events[1] is not None:
                offscreen_canvas.Clear()
                graphics.DrawText(offscreen_canvas, font, 0, 19, textColor, "2nd")
                graphics.DrawText(offscreen_canvas, font, self.timeOffset, 19, textColor, events[1].time)
                graphics.DrawText(offscreen_canvas, font, self.nameOffset, 19, textColor, events[1].name)
                graphics.DrawText(offscreen_canvas, font, self.etaOffset, 19, textColor, events[1].eta)

            time.sleep(0.05)
            offscreen_canvas = self.matrix.SwapOnVSync(offscreen_canvas)


# Main function
if __name__ == "__main__":
    run_text = RunText()
    if (not run_text.process()):
        run_text.print_help()
