#!/bin/bash -x

systemctl --user stop pipewire pipewire-pulse wireplumber

pkill -x pipewire
pkill -x pipewire-pulse
pkill -x wireplumber

rm -f /run/user/1000/pipewire-*.lock
