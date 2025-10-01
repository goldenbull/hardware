from microbit import *


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
