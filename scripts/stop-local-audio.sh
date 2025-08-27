#!/bin/bash -x

systemctl --user stop pipewire pipewire-pulse wireplumber
systemctl --user stop pipewire.socket
systemctl --user stop pipewire-pulse.socket

pkill -x pipewire
pkill -x pipewire-pulse
pkill -x wireplumber

rm -f /run/user/1000/pipewire-*.lock
