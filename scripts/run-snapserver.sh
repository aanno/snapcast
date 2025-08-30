#!/bin/bash -x

rm snapserver.log
./bin/snapserver -c snapserver2.conf | tee snapserver.log
