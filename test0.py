from microbit import *
import time


class KeyStatus:
    """
    记录各个按键的触发状态，get and clear模式
    """

    def __init__(self):
        self._prev_key_a_is_down = False
        self._prev_key_b_is_down = False
        self._prev_logo_is_down = False

        self._key_a_pressed = False
        self._key_b_pressed = False
        self._key_ab_pressed = False
        self._logo_pressed = False

    def update(self):
        # 根据之前的状态和当前的状态，决定是否设置触发状态
        cur_key_a_is_down = button_a.is_pressed()
        cur_key_b_is_down = button_b.is_pressed()
        cur_logo_is_down = pin_logo.is_touched()

        if self._prev_key_a_is_down and not cur_key_a_is_down:
            self._key_a_pressed = True
        if self._prev_key_b_is_down and not cur_key_b_is_down:
            self._key_b_pressed = True
        if (self._prev_key_a_is_down and self._prev_key_b_is_down) \
                and not (cur_key_a_is_down and cur_key_b_is_down):
            self._key_ab_pressed = True
        if self._prev_logo_is_down and not cur_logo_is_down:
            self._logo_pressed = True

        self._prev_key_a_is_down = cur_key_a_is_down
        self._prev_key_b_is_down = cur_key_b_is_down
        self._prev_logo_is_down = cur_logo_is_down

    def is_key_a_pressed(self):
        v = self._key_a_pressed
        self._key_a_pressed = False
        return v

    def is_key_b_pressed(self):
        v = self._key_b_pressed
        self._key_b_pressed = False
        return v

    def is_key_ab_pressed(self):
        # 同时clear a和b的状态
        v = self._key_ab_pressed
        self._key_ab_pressed = False
        self._key_a_pressed = False
        self._key_b_pressed = False
        return v

    def is_logo_pressed(self):
        v = self._logo_pressed
        self._logo_pressed = False
        return v


class Clock:

    def __init__(self):
        self._chip_base_ts = time.ticks_ms()
        self._realworld_base_sec = 0
        self._bright = 5

    def hour(self):
        return self.now() // 3600

    def minute(self):
        return self.now() // 60 % 60

    def second(self):
        return self.now() % 60

    def set_hour(self, hour):
        self._realworld_base_sec = hour * 3600 + self.now() % 3600
        self._chip_base_ts = time.ticks_ms()

    def set_minute(self, minute):
        self._realworld_base_sec = self.hour() * 3600 + minute * 60 + self.second()
        self._chip_base_ts = time.ticks_ms()

    def set_second(self, second):
        self._realworld_base_sec = self.now() // 60 * 60 + second
        self._chip_base_ts = time.ticks_ms()

    def inc_hour(self):
        v = self.hour() + 1
        if v == 24:
            v = 0
        self.set_hour(v)

    def dec_hour(self):
        v = self.hour() - 1
        if v == -1:
            v = 23
        self.set_hour(v)

    def inc_minute(self):
        v = self.minute() + 1
        if v == 60:
            v = 0
        self.set_minute(v)

    def dec_minute(self):
        v = self.minute() - 1
        if v == -1:
            v = 59
        self.set_minute(v)

    def inc_second(self):
        v = self.second() + 1
        if v == 60:
            v = 0
        self.set_second(v)

    def dec_second(self):
        v = self.second() - 1
        if v == -1:
            v = 59
        self.set_second(v)

    def inc_bright(self):
        v = self._bright + 1
        if v > 9:
            v = 9
        self._bright = v

    def dec_bright(self):
        v = self._bright - 1
        if v <= 0:
            v = 1
        self._bright = v

    def now(self) -> int:
        # get current datetime by delta ticks
        chip_cur_ts = time.ticks_ms()
        delta_seconds = (chip_cur_ts - self._chip_base_ts) / 1000
        cur_realworld_sec = self._realworld_base_sec + delta_seconds
        return int(cur_realworld_sec)

    def show(self):
        self.show_hour(self.hour())
        self.show_minute(self.minute())
        self.show_second(self.second())

    def show_hour(self, hour):
        v = hour
        for y in range(5):
            pt = v % 2
            if pt != 0:
                display.set_pixel(0, 4 - y, self._bright)
            else:
                display.set_pixel(0, 4 - y, 0)
            v = v // 2

    def show_minute(self, minute):
        v = minute
        for x in range(1, 3):
            for y in range(5):
                pt = v % 2
                if pt != 0:
                    display.set_pixel(3 - x, 4 - y, self._bright)
                else:
                    display.set_pixel(3 - x, 4 - y, 0)
                v = v // 2

    def show_second(self, second):
        v = second
        for x in range(3, 5):
            for y in range(5):
                pt = v % 2
                if pt != 0:
                    display.set_pixel(7 - x, 4 - y, self._bright)
                else:
                    display.set_pixel(7 - x, 4 - y, 0)
                v = v // 2


# all modes
ScreenOn = 1
AdjustHour = 2
AdjustMinute = 3
AdjustSecond = 4
ScreenOff = 5

# init system
pin_logo.set_touch_mode(pin_logo.CAPACITIVE)

# variables
clock = Clock()
ks = KeyStatus()
cur_mode = ScreenOn

# init time for test
clock.set_hour(23)
clock.set_minute(29)

while True:
    # update key status
    ks.update()

    # process key events in each mode, switch mode if needed
    if cur_mode == ScreenOn:
        if ks.is_logo_pressed():
            # switch on/off
            cur_mode = ScreenOff
            display.off()
        elif ks.is_key_ab_pressed():
            cur_mode = AdjustHour
        elif ks.is_key_a_pressed():
            clock.dec_bright()
        elif ks.is_key_b_pressed():
            clock.inc_bright()
    elif cur_mode == AdjustHour:
        if ks.is_key_ab_pressed():
            cur_mode = AdjustMinute
        elif ks.is_key_a_pressed():
            clock.dec_hour()
        elif ks.is_key_b_pressed():
            clock.inc_hour()
    elif cur_mode == AdjustMinute:
        if ks.is_key_ab_pressed():
            cur_mode = AdjustSecond
        elif ks.is_key_a_pressed():
            clock.dec_minute()
        elif ks.is_key_b_pressed():
            clock.inc_minute()
    elif cur_mode == AdjustSecond:
        if ks.is_key_ab_pressed():
            cur_mode = ScreenOn  # back to normal
        elif ks.is_key_a_pressed():
            clock.dec_minute()
        elif ks.is_key_b_pressed():
            clock.inc_minute()
    elif cur_mode == ScreenOff:
        cur_mode = ScreenOn
        display.on()

    # output
    clock.show()

    sleep(100)
