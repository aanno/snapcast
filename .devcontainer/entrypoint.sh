#!/bin/bash

# Start D-Bus session
export DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/1000/bus
mkdir -p /run/user/1000
dbus-daemon --session --address="$DBUS_SESSION_BUS_ADDRESS" --nofork --nopidfile --syslog-only &

# Start PipeWire
pipewire &

# Start WirePlumber
wireplumber &

# Keep the container running (or launch your application)
exec "$@"
