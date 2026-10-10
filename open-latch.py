#!/usr/bin/env python3
import time
import RPi.GPIO as GPIO

LATCH = 19
OPEN_SECONDS = 0.75

GPIO.setwarnings(False)
GPIO.setmode(GPIO.BCM)
GPIO.setup(LATCH, GPIO.OUT, initial=GPIO.LOW)

try:
    GPIO.output(LATCH, GPIO.HIGH)
    time.sleep(OPEN_SECONDS)
finally:
    GPIO.output(LATCH, GPIO.LOW)
    GPIO.cleanup(LATCH)
