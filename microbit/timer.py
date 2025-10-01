from microbit import *
import time
import radio
from key_status import KeyStatus

# 测量microbit的时钟精度

radio.off()
records = []
ks = KeyStatus()

while True:
    ks.update()
    if ks.is_key_a_pressed():
        v = int(time.ticks_ms() / 1000)
        records.append(v)
        display.show(str(v), clear=True)
