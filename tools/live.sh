#!/bin/sh
# Talks to a running WhateverPad PS5 through its page (HTTP, local network only).
# Made for people and for scripts alike.
#
#   WHATEVERPAD_IP=192.168.x.y tools/live.sh watch            follow the log in real time (saved in ./logs/)
#   WHATEVERPAD_IP=...         tools/live.sh state            state of WhateverPad (JSON)
#   WHATEVERPAD_IP=...         tools/live.sh settings         the settings that can be changed while it runs (JSON)
#   WHATEVERPAD_IP=...         tools/live.sh set <name> <v>   change one of them
#   WHATEVERPAD_IP=...         tools/live.sh hci <op> [hex]   one HCI command (op in hex, e.g. 0C1B). Never Reset.
#   WHATEVERPAD_IP=...         tools/live.sh pair             press "Pair" (60 s)
#   WHATEVERPAD_IP=...         tools/live.sh stop             stop WhateverPad PS5
#
# The console's address is shown in the notification WhateverPad displays when it
# starts, and on its page ("http://<ip>:8095/"). Port 8095 unless WHATEVERPAD_PORT is set.
IP="${WHATEVERPAD_IP:?set WHATEVERPAD_IP to the console's address, e.g. WHATEVERPAD_IP=192.168.1.20}"
URL="http://$IP:${WHATEVERPAD_PORT:-8095}"
H='X-WhateverPad: 1'     # every change must carry this header

case "$1" in
watch)
    D="${WHATEVERPAD_LOGS:-./logs}"
    mkdir -p "$D"
    OUT="$D/live-$(date +%Y%m%d-%H%M%S).log"
    N=0; FAILS=0
    echo "following $URL (saving in $OUT)"
    while :; do
        R="$(curl "$URL/api/live?from=$N" -s --max-time 5)"
        if [ -z "$R" ]; then
            FAILS=$((FAILS + 1))
            [ "$FAILS" -eq 3 ] && { echo "--- WhateverPad does not answer (stopped, or not started yet)"; N=0; }
            sleep 2; continue
        fi
        FAILS=0
        NX="$(printf '%s\n' "$R" | head -1 | sed -n 's/^#next //p')"
        [ -n "$NX" ] || NX=0
        if [ "$NX" -lt "$N" ]; then            # WhateverPad was restarted: its log starts again from zero
            echo "--- WhateverPad restarted"; N=0; continue
        fi
        N="$NX"
        printf '%s\n' "$R" | tail -n +2 | sed '/^$/d' | tee -a "$OUT"
        sleep 1
    done
    ;;
state)    curl "$URL/api/state" -s --max-time 5; echo ;;
settings) curl "$URL/api/tune" -s --max-time 5; echo ;;
set)      curl "$URL/api/tune?$2=$3" -s --max-time 5 -X POST -H "$H"; echo ;;
hci)      curl "$URL/api/hci?op=$2&p=$3" -s --max-time 8 -X POST -H "$H"; echo ;;
pair)     curl "$URL/api/pair" -s --max-time 5 -X POST -H "$H"; echo ;;
stop)     curl "$URL/api/stop" -s --max-time 5 -X POST -H "$H"; echo ;;
*) sed -n '2,13p' "$0"; exit 2 ;;
esac
