#!/bin/bash -x
./bin/snapclient --player pipewire -s 'alsa_output.pci-0000_01_00.1.hdmi-stereo' -logsink stdout --logfilter *:trace tcp://192.168.10.139:1704

# -s 64
# tcp://localhost:1704

# working:
# ./bin/snapclient --player pulse -s 1 tcp://192.168.10.139:1704
# ./bin/snapclient --player pulse -s 1 --logsink stdout --logfilter *:trace tcp://192.168.10.139:1704

# crashing:
# ./bin/snapclient --player pipewire -s 1 tcp://192.168.10.139:1704
