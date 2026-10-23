#!/bin/sh
set -eu
explorer=$1
mutant=$2
root=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT HUP INT TERM
mkdir "$tmp/valid" "$tmp/bad" "$tmp/reduced" "$tmp/violated"

jq -nc '
  ["scan","mask","query","classic","le-create","inquiry","auth","encryption","p256","dhkey","virtual"][] as $c
  | [0,1][] as $p | ["normal","sanitized"][] as $b
  | {class:$c,policy:$p,build:$b,depth:12,
     states:(if $c == "scan" then (if $p == 0 then 37374038 else 82398170 end) else 1 end),
     edges:0,violations:0,g:($c == "scan"),h:($c == "scan")}
' > "$tmp/valid/results.jsonl"
sh "$root/tools/check-explorer-results.sh" "$tmp/valid"

reject() {
  jq -cs "$2" "$tmp/valid/results.jsonl" > "$tmp/bad/results.jsonl"
  status=0
  sh "$root/tools/check-explorer-results.sh" "$tmp/bad" > "$tmp/error" 2>&1 || status=$?
  if [ "$status" -ne 1 ]; then
    echo "$1 control returned $status instead of 1" >&2
    exit 1
  fi
}
reject missing '.[1:][]'
reject duplicate '(. + [.[0]])[]'
reject total '(.[0].states += 1) | .[]'
reject violation '(.[0].violations = 1) | .[]'
reject depth '(.[0].depth = 11) | .[]'
reject reachability-g '(.[0].g = false) | .[]'
reject reachability-h '(.[0].h = false) | .[]'
reject identity '(.[0].class = "unknown") | .[]'

jq -n '{classes:["scan"],totals:{"0":3,"1":5}}' > "$tmp/expected.json"
jq -nc '[0,1][] as $p | ["normal","sanitized"][] as $b |
  {class:"scan",policy:$p,build:$b,depth:12,states:(if $p == 0 then 3 else 5 end),
   edges:1,violations:0,g:true,h:true}' > "$tmp/reduced/results.jsonl"
sh "$root/tools/check-explorer-results.sh" --fixture "$tmp/expected.json" "$tmp/reduced"

reject_args() {
  status=0
  "$explorer" "$@" > "$tmp/cli" 2>&1 || status=$?
  if [ "$status" -ne 2 ]; then
    echo "invalid explorer arguments returned $status instead of 2" >&2
    exit 1
  fi
}
reject_args --policy 2 --case scan
reject_args --policy 01 --case scan
reject_args --policy 0 --case unknown
reject_args --policy 0
reject_args --policy 0 --policy 1
reject_args --policy 0 --case scan extra

status=0
"$mutant" --policy 1 --case scan > "$tmp/violated/result.jsonl" 2> "$tmp/trace" || status=$?
test "$status" -eq 1
jq -se 'length == 1 and (.[0] |
  .class == "scan" and .policy == 1 and .build == "normal" and .depth == 12
  and .states > 0 and .edges > 0 and .violations > 0
  and (.g | type) == "boolean" and (.h | type) == "boolean")' \
  "$tmp/violated/result.jsonl" >/dev/null
status=0
sh "$root/tools/check-explorer-results.sh" "$tmp/violated" > "$tmp/error" 2>&1 || status=$?
test "$status" -eq 1

status=0
"$mutant" > "$tmp/default" 2> "$tmp/default-error" || status=$?
test "$status" -eq 1
grep -q '^FAIL ' "$tmp/default"
test ! -s "$tmp/default-error"
echo "explorer result and CLI controls passed"
