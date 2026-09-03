"""轻量 psutil 兼容层（OHOS 变体）：仅覆盖 ComfyUI 0.34.0 用到的表面。"""
import os, sys
from collections import namedtuple

svmem = namedtuple("svmem", ["total", "available", "percent", "used", "free", "active", "inactive", "buffers", "cached", "shared"])
sswap = namedtuple("sswap", ["total", "used", "free", "percent", "sin", "sout"])

def _pagesize():
    return os.sysconf("SC_PAGESIZE")

def virtual_memory():
    try:
        total = os.sysconf("SC_PHYS_PAGES") * _pagesize()
        avail = os.sysconf("SC_AVPHYS_PAGES") * _pagesize()
    except (ValueError, OSError, KeyError):
        total = 0
        avail = 0
    used = total - avail
    pct = (used / total * 100.0) if total else 0.0
    return svmem(total, avail, pct, used, avail, None, None, None, None, None)

def swap_memory():
    return sswap(0, 0, 0, 0.0, 0, 0)

def cpu_count(logical=True):
    return os.cpu_count() or 1

def cpu_percent(interval=None):
    return 0.0

class Process:
    def __init__(self, pid=None):
        self.pid = pid if pid is not None else os.getpid()
    def memory_info(self):
        return namedtuple("meminfo", ["rss", "vms"])(0, 0)
    def cpu_percent(self, interval=None):
        return 0.0
    def cmdline(self):
        return []
    def kill(self):
        os.kill(self.pid, 9)
