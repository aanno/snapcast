#!/bin/bash -x
# https://code.visualstudio.com/docs/configure/settings-sync#_other-linux-desktop-environments

// TODO: This has to much a real, existing snd device
sudo mknod /tmp/snd-pcmC0D0p c 116 2
sudo chown root:audio /tmp/snd-pcmC0D0p
sudo chmod 660 /tmp/snd-pcmC0D0p
