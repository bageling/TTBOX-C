#!/bin/bash
# a9_stress_check.sh — 压力测试收尾检查：温度 / 降频 / gadget / fd / 线程
echo "===== 温度（thermal zones）====="
for z in /sys/class/thermal/thermal_zone*/; do
  t=$(cat $z/temp 2>/dev/null)
  type=$(cat $z/type 2>/dev/null)
  [ -n "$t" ] && echo "  $type: $((t/1000)).$((t%1000))°C"
done

echo "===== 频率（降频检查）====="
for p in /sys/devices/system/cpu/cpufreq/policy*; do
  [ -d "$p" ] || continue
  g=$(cat "$p/scaling_governor" 2>/dev/null)
  f=$(cat "$p/scaling_cur_freq" 2>/dev/null)
  echo "  $(basename "$p"): governor=$g cur=$(( ${f:-0} / 1000 ))MHz"
done
# NPU devfreq 节点名不写死（fdab0000.npu 是板级地址，P8）：按名字含 npu 匹配
for d in /sys/class/devfreq/*; do
  [ -e "$d" ] || continue
  case "$(basename "$d")" in
    *npu*) echo "  NPU $(basename "$d"): $(cat "$d/cur_freq" 2>/dev/null) Hz" ;;
  esac
done

echo "===== HID Gadget 状态 ====="
# UDC 名不写死硬件编号（P8）：优先 USB_PROXY_DEVICE，否则 /sys/class/udc 排序首个
UDC=${USB_PROXY_DEVICE:-$(ls /sys/class/udc 2>/dev/null | sort | head -n1)}
if [ -z "$UDC" ]; then
  echo "  (无 UDC：/sys/class/udc 为空)"
else
  echo "  udc: $UDC state=$(cat "/sys/class/udc/$UDC/state" 2>/dev/null)"
fi
ls -la /dev/hidg* 2>/dev/null

echo "===== 进程/线程（残留检查）====="
ps -eLf 2>/dev/null | grep -E 'hid|worker|test_hid' | grep -v grep | head

echo "===== 系统负载 ====="
uptime
