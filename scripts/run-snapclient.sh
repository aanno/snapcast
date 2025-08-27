#!/bin/bash -x
./bin/snapclient --player pipewire -s 1 -logsink stdout --logfilter *:trace tcp://localhost:1704
