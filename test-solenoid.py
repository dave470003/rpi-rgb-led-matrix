import RPi.GPIO as GPIO
import time

LATCH = 19

GPIO.setmode(GPIO.BCM)
GPIO.setup(LATCH, GPIO.OUT, initial=GPIO.LOW)

try:
    print("Locked")
    time.sleep(2)

    print("OPEN")
    GPIO.output(LATCH, GPIO.HIGH)

    time.sleep(0.75)

    GPIO.output(LATCH, GPIO.LOW)
    print("Locked again")

finally:
    GPIO.output(LATCH, GPIO.LOW)
    GPIO.cleanup()
