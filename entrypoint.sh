#!/bin/bash
# Runs gz sim (server, headless) and the gateway; exits when either does so the container dies with it.
gz sim -s -r -v 2 /opt/astrosim/worlds/poc.sdf &
GZ=$!
gateway 8765 600 &
GW=$!
trap 'kill $GZ $GW 2>/dev/null' TERM INT
wait -n $GZ $GW
kill $GZ $GW 2>/dev/null
wait
