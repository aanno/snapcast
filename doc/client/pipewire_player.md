# pipewire player

## Known Problem

* Auto select of soundcard (index) is broken, currently you have to provide the `-s` flag on cli.


## To continue

Well, if there are no sound data, the player should disconnect from sink after a short grace period (10s). Also this silent detection is on, 
