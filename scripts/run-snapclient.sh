#!/bin/bash -x
rm snapclient.log
./bin/snapclient --latency 100 --player pipewire -s 'alsa_output.pci-0000_01_00.1.hdmi-stereo' --logsink stdout --logfilter *:trace rist://192.168.10.139:1706 | tee snapclient.log

# tcp://192.168.10.139:1704

# -s 64
# tcp://localhost:1704

# working:
# ./bin/snapclient --player pulse -s 1 tcp://192.168.10.139:1704
# ./bin/snapclient --player pulse -s 1 --logsink stdout --logfilter *:trace tcp://192.168.10.139:1704

# crashing:
# ./bin/snapclient --player pipewire -s 1 tcp://192.168.10.139:1704
