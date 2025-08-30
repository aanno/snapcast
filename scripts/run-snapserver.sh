#!/bin/bash -x

rm snapserver.log
./bin/snapserver -z -c snapserver2.conf | tee snapserver.log
