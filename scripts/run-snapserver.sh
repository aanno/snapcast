#!/bin/bash -x

rm snapserver.log
./bin/snapserver -z -c snapserver2.conf > snapserver.log 2>&1 &
#| tee snapserver.log
pid=$(pidof snapserver)

if [ -z "$pid" ]; then
  echo "snapserver not running"
  exit 1
fi

pidstat 1 -p $pid -d -r -R -H -vwus -o JSON --human >cpu.json &
less snapserver.log
