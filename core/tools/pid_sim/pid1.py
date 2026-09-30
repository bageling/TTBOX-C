"""pid1.py — Pid1Controller.hpp 的 Python 精确移植（本地仿真用，不进产品构建）"""
import math


class Pid1:
    def __init__(self, kp, kd, predict, rate, smooth):
        self.kp = kp
        self.kd = kd
        self.predict = predict
        self.rate = rate
        self.smooth = smooth
        self.kb = 10000.0
        self.kp_gain = 0.0
        self.igain = 0.0
        self.u = 0.0
        self.le = 0.0
        self.lu = 0.0
        self.vfx = 0.0
        self.vfp = 1.0
        self.ifx = 0.0
        self.ifp = 1.0

    def st(self, v, bw, os):
        r = v / bw
        q = r * r
        return (r * (1.0 + (4.0 / 15.0) * q) / (1.0 + (3.0 / 5.0) * q)) * os

    def upd(self, e):
        if abs(e) < 0.3:
            e = 0.0
        if abs(e - self.le) > 30.0:
            self.reset()
        ae = abs(e)
        if ae < 50.0:
            r = 1.0 - ae / 50.0
            self.igain += (r - self.igain) * 0.025
        else:
            r = 50.0 / ae
            self.igain += (r * self.igain - self.igain) * 0.1
        self.igain = max(0.0, min(1.0, self.igain))
        if ae < 1920.0:
            r = 1.0 - ae / 1920.0
            self.kp_gain += (r - self.kp_gain) * self.rate
        else:
            r = 1920.0 / ae
            self.kp_gain += (r * self.kp_gain - self.kp_gain) * 0.1
        self.kp_gain = max(0.0, min(1.0, self.kp_gain))
        ed = e - self.le
        tv = ed + self.lu
        px = self.vfx
        pp = self.vfp + 0.01
        k = pp / (pp + 1.0)
        self.vfx = px + k * (tv - px)
        self.vfp = (1 - k) * pp
        rvi = self.vfx
        if abs(e) < 1.0 and abs(ed) < 0.1:
            rvi = ed + self.lu * 0.5
        kr = rvi
        if abs(kr) <= 0.5:
            kr = 0.0
        kr = (kr * self.predict) * self.igain
        px = self.ifx
        pp = self.ifp + 0.5
        k = pp / (pp + 1.0)
        self.ifx = px + k * (kr - px)
        self.ifp = (1 - k) * pp
        kr = self.ifx
        Kp = self.kp * e
        Ki = kr
        Kd = self.kd * ed
        if self.smooth:
            Kp = self.st(Kp, self.kb, self.kb - self.smooth)
            Ki = self.st(Ki, self.kb, self.kb - 1000.0)
            Kd = self.st(Kd, self.kb, self.kb - self.smooth)
        self.u = (Kp + Ki + Kd) * self.kp_gain
        self.lu = self.u
        self.le = e
        return self.u

    def reset(self):
        self.kp_gain = 0.0
        self.igain = 0.0
        self.u = 0.0
        self.le = 0.0
        self.lu = 0.0
        self.vfx = 0.0
        self.vfp = 1.0
        self.ifx = 0.0
        self.ifp = 1.0
