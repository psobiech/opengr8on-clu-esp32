#!/bin/sh
# tools/clu_sec.py against the C engine (sec_peer) on loopback: probe, pairing refused / allowed,
# exec over the encrypted transport, pinned-key mismatch. Needs python3 with noiseprotocol + cryptography.
set -u
cd "$(dirname "$0")"
TOOL=../../tools/clu_sec.py
CONF=$(mktemp -d)
export OPENGR8ON_CONF="$CONF"
PORT=${SEC_TEST_PORT:-21235}
fail=0
ok() { echo "ok   $1"; }
bad() { echo "FAIL $1"; fail=1; }
start_peer() { # sn pairing [window seconds]
    ./sec_peer "$PORT" "$1" "$2" 20 ${3:-} > "$CONF/peer.out" &
    PEER=$!
    i=0; while ! grep -q ready "$CONF/peer.out" 2>/dev/null && [ $i -lt 50 ]; do sleep 0.1; i=$((i+1)); done
}
stop_peer() { kill "$PEER" 2>/dev/null; wait "$PEER" 2>/dev/null; }
run() { python3 -I "$TOOL" --port "$PORT" --timeout 1 "$@" 2>&1; }

start_peer 4242 0
out=$(run pubkey 127.0.0.1); echo "$out" | grep -q "sn 4242 .*not paired" && ok "pubkey probe" || bad "pubkey probe: $out"
out=$(run pair 127.0.0.1); echo "$out" | grep -q "refused" && ok "pairing refused outside window" || bad "refused: $out"
stop_peer

start_peer 4242 1
out=$(run pair 127.0.0.1); echo "$out" | grep -q "paired with CLU 4242" && ok "paired" || bad "pair: $out"
out=$(run exec 127.0.0.1 'checkAlive()'); [ "$out" = "echo:checkAlive()" ] && ok "exec over secure channel" || bad "exec: $out"
out=$(run exec 127.0.0.1 'a:b(1,"x:y")'); [ "$out" = 'echo:a:b(1,"x:y")' ] && ok "exec with colons" || bad "exec colons: $out"
stop_peer

start_peer 4242 1 # new process = new key for the same serial
out=$(run exec 127.0.0.1 'x()'); echo "$out" | grep -q "refusing" && ok "changed key refused" || bad "mismatch: $out"
out=$(run forget 4242); echo "$out" | grep -q removed && ok "forget" || bad "forget: $out"
out=$(run pair 127.0.0.1); echo "$out" | grep -q "paired with CLU 4242" && ok "re-pair after forget" || bad "re-pair: $out"
stop_peer

rm -f "$CONF/clus.json"
start_peer 4343 1 2 # pairing window closes after 2 s
out=$(run pair 127.0.0.1); echo "$out" | grep -q "paired with CLU 4343" && ok "paired inside a short window" || bad "pair window: $out"
sleep 2.5
out=$(run exec 127.0.0.1 'later()'); [ "$out" = "echo:later()" ] && ok "pin kept after the window closed" || bad "after window: $out"
stop_peer

rm -rf "$CONF"
[ $fail = 0 ] && echo "PASSED" || echo "FAILED"
exit $fail
