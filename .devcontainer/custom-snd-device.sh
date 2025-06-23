#!/bin/bash -x

// TODO: This has to much a real, existing snd device
sudo mknod /tmp/snd-pcmC0D0p c 116 2
sudo chown root:audio /tmp/snd-pcmC0D0p
sudo chmod 660 /tmp/snd-pcmC0D0p
