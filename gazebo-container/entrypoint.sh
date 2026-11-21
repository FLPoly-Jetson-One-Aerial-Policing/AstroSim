#!/bin/bash

WORLD_FILE="default.sdf"

"/usr/libexec/gz/sim${1}/gz-sim-server" /opt/astrosim/worlds/${WORLD_FILE} &
GZ=$!

trap 'kill $GZ 2>/dev/null' TERM INT
wait -n $GZ
kill $GZ 2>/dev/null
wait
