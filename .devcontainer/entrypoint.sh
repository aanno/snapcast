#!/bin/bash

# Start D-Bus session
export DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/1000/bus
mkdir -p /run/user/1000
dbus-daemon --session --address="$DBUS_SESSION_BUS_ADDRESS" --nofork --nopidfile --syslog-only &

# Start PipeWire
# pipewire &

# Start WirePlumber
# wireplumber &

# dbus checks
pgrep -x "dbus-daemon"
busctl --user

# pipewire checks
# pgrep -x "pipewire"
# pw-top
# pw-link -o -i -l -I

# pulseaudio checks
# pactl info

# wireplumber checks
# pgrep -x "wireplumber"

# alsa checks
# aplay -l

# Keep the container running (or launch your application)
exec "$@"
