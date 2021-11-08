from microbit import *
import time, music, radio


class KeyStatus:
    """
    记录各个按键的触发状态，同步+轮询，get and clear模式
    难点在于正确处理ab同时按下的事件
    完整的行为应该通过状态机描述，但此处简单处理
    """

    def __init__(self):
        self._prev_a_is_down = False
        self._prev_b_is_down = False
        self._prev_ab_is_down = False
        self._prev_logo_is_down = False

        self._event_a = False
        self._event_b = False
        self._event_ab = False
        self._event_logo = False

    def update(self):
        # 根据之前的状态和当前的状态，决定是否设置触发状态
        cur_key_a_is_down = button_a.is_pressed()
        cur_key_b_is_down = button_b.is_pressed()
        cur_logo_is_down = pin_logo.is_touched()

        # 首先特殊处理ab同时按下的情况
        if self._prev_ab_is_down:
            if not cur_key_a_is_down and not cur_key_b_is_down:
                # 曾经同时按下，现在都抬起了
                self._prev_ab_is_down = False
                self._event_ab = True
            else:
                # 还有一个键没松开，等着松开
                pass
        else:
            if cur_key_a_is_down and cur_key_b_is_down:
                # 同时按下
                self._prev_ab_is_down = True
            else:
                # 单独按键
                if self._prev_a_is_down and not cur_key_a_is_down:
                    self._event_a = True
                if self._prev_b_is_down and not cur_key_b_is_down:
                    self._event_b = True

        # logo的逻辑相对简单
        if self._prev_logo_is_down and not cur_logo_is_down:
            self._event_logo = True

        self._prev_a_is_down = cur_key_a_is_down
        self._prev_b_is_down = cur_key_b_is_down
        self._prev_logo_is_down = cur_logo_is_down

    def is_key_a_pressed(self):
        v = self._event_a
        if v:
            self._event_a = False
        return v

    def is_key_b_pressed(self):
        v = self._event_b
        if v:
            self._event_b = False
        return v

    def is_key_ab_pressed(self):
        # 同时clear单个键的状态
        v = self._event_ab
        if v:
            self._event_ab = False
            self._event_a = False
            self._event_b = False
        return v

    def is_logo_pressed(self):
        v = self._event_logo
        if v:
            self._event_logo = False
        return v


class Clock:
    # all modes
    ScreenOn = 1
    AdjustHour = 2
    AdjustMinute = 3
    AdjustSecond = 4
    ScreenOff = 5

    def __init__(self):
        self._chip_base_ts = time.ticks_ms()
        self._realworld_base_sec = 0
        self._bright = 5
        self.cur_mode = Clock.ScreenOn

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

    def set_bright(self, v):
        if v > 9:
            v = 9
        if v < 1:
            v = 1
        self._bright = v

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
        b = self._bright
        if self.cur_mode == Clock.AdjustHour:
            b = 9
        elif self.cur_mode in [Clock.AdjustMinute, Clock.AdjustSecond]:
            b = 1
        for y in range(5):
            pt = v % 2
            if pt != 0:
                display.set_pixel(0, 4 - y, b)
            else:
                display.set_pixel(0, 4 - y, 0)
            v = v // 2

    def show_minute(self, minute):
        v = minute
        b = self._bright
        if self.cur_mode == Clock.AdjustMinute:
            b = 9
        elif self.cur_mode in [Clock.AdjustHour, Clock.AdjustSecond]:
            b = 1
        for x in range(1, 3):
            for y in range(5):
                pt = v % 2
                if pt != 0:
                    display.set_pixel(3 - x, 4 - y, b)
                else:
                    display.set_pixel(3 - x, 4 - y, 0)
                v = v // 2

    def show_second(self, second):
        v = second
        b = self._bright
        if self.cur_mode == Clock.AdjustSecond:
            b = 9
        elif self.cur_mode in [Clock.AdjustHour, Clock.AdjustMinute]:
            b = 1
        for x in range(3, 5):
            for y in range(5):
                pt = v % 2
                if pt != 0:
                    display.set_pixel(7 - x, 4 - y, b)
                else:
                    display.set_pixel(7 - x, 4 - y, 0)
                v = v // 2

    def alarm(self, t0, t1):
        h0, m0, s0 = t0
        h1, m1, s1 = t1
        ts0 = h0 * 3600 + m0 * 60 + s0
        ts1 = h1 * 3600 + m1 * 60 + s1
        if ts0 <= self.now() <= ts1:
            music.play(["C6:1", "D6:1", "E6:1", ])


# init_system
pin_logo.set_touch_mode(pin_logo.CAPACITIVE)
radio.off()  # to save energy

# variables
clock = Clock()
ks = KeyStatus()

# init time for test
clock.set_hour(10)
clock.set_minute(34)
clock.set_bright(15)


def debug_1():
    screen_on = True
    while True:
        ks.update()

        if ks.is_logo_pressed():
            screen_on = not screen_on
            if screen_on:
                display.on()
            else:
                display.off()
        if ks.is_key_a_pressed():
            clock.dec_bright()
            # clock.inc_hour()
        if ks.is_key_b_pressed():
            clock.inc_bright()
            # clock.inc_minute()
        if ks.is_key_ab_pressed():
            clock.inc_second()
        clock.show()
        sleep(100)


def main():
    while True:
        # update key status
        ks.update()

        # process key events in each mode, switch mode if needed
        if clock.cur_mode == Clock.ScreenOn:
            if ks.is_logo_pressed():
                # switch on/off
                clock.cur_mode = Clock.ScreenOff
                music.play(["E4:1", "D4:1", "C4:1", ])
                display.off()
            elif ks.is_key_ab_pressed():
                clock.cur_mode = Clock.AdjustHour
                music.play(["C3:1", ])
            elif ks.is_key_a_pressed():
                clock.dec_bright()
            elif ks.is_key_b_pressed():
                clock.inc_bright()
        elif clock.cur_mode == Clock.AdjustHour:
            if ks.is_key_ab_pressed():
                clock.cur_mode = Clock.AdjustMinute
                music.play(["C4:1", ])
            elif ks.is_key_a_pressed():
                clock.dec_hour()
            elif ks.is_key_b_pressed():
                clock.inc_hour()
        elif clock.cur_mode == Clock.AdjustMinute:
            if ks.is_key_ab_pressed():
                clock.cur_mode = Clock.AdjustSecond
                music.play(["C5:1", ])
            elif ks.is_key_a_pressed():
                clock.dec_minute()
            elif ks.is_key_b_pressed():
                clock.inc_minute()
        elif clock.cur_mode == Clock.AdjustSecond:
            if ks.is_key_ab_pressed():
                clock.cur_mode = Clock.ScreenOn  # back to normal
                music.play(["C4:1", "D4:1", "E4:1", ])
            elif ks.is_key_a_pressed():
                clock.dec_second()
            elif ks.is_key_b_pressed():
                clock.inc_second()
        elif clock.cur_mode == Clock.ScreenOff:
            if ks.is_logo_pressed():
                clock.cur_mode = Clock.ScreenOn
                music.play(["C4:1", "D4:1", "E4:1", ])
                display.on()

        # output
        clock.show()

        # 闹钟
        clock.alarm((7, 30, 0), (7, 30, 10))

        sleep(100)


main()
