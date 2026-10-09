#!/bin/bash
# a9_loopback_rates.sh — f_hid 背压/转发率多档测试
cd /home/ubuntu/ttbox2/ttbox/core/build
make -j8 2>&1 | tail -1
for r in 125 250 500 1000 2000 4000 8000; do
  echo "== ${r}Hz =="
  sudo ./test_hid_loopback --rate $r --duration 3 2>&1 | grep -E '注入|latency|回环'
done
