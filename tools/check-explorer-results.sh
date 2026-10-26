#!/bin/sh
set -eu

classes='["scan","mask","query","classic","le-create","inquiry","auth","encryption","p256","dhkey","virtual"]'
totals='{"0":51085930,"1":121269723}'
# Reduced contracts are only for local fixtures; host uses the fixed contract.
if [ "$#" -eq 3 ] && [ "$1" = "--fixture" ]; then
  classes=$(jq -ce '.classes' "$2")
  totals=$(jq -ce '.totals' "$2")
  shift 2
fi
if [ "$#" -ne 1 ]; then
  echo "usage: $0 [--fixture expected.json] results-directory" >&2
  exit 2
fi
directory=$1
set --
for file in "$directory"/*.jsonl "$directory"/*/*.jsonl; do
  if [ -f "$file" ]; then set -- "$@" "$file"; fi
done
if [ "$#" -eq 0 ]; then
  echo "no explorer results" >&2
  exit 1
fi
if ! jq -s -e --argjson classes "$classes" --argjson totals "$totals" '
  def identity: [.policy, .class, .build];
  . as $results
  | [$classes[] as $c | [0,1][] as $p | ["normal","sanitized"][] as $b |
      [$p,$c,$b]] as $expected
  | ($classes | length > 0 and length == (unique | length))
    and all($results[];
      (.class | type) == "string"
      and (.policy == 0 or .policy == 1)
      and (.build == "normal" or .build == "sanitized")
      and .depth == 12
      and (.states | type) == "number" and .states > 0
      and .states == (.states | floor)
      and (.edges | type) == "number" and .edges >= 0
      and .edges == (.edges | floor)
      and .violations == 0
      and (.g | type) == "boolean" and (.h | type) == "boolean")
    and ([$results[] | identity] | sort) == ($expected | sort)
    and all([0,1][] as $p | ["normal","sanitized"][] as $b | {p:$p,b:$b};
      . as $group
      | [$results[] | select(.policy == $group.p and .build == $group.b)] as $rows
      | ($rows | map(.states) | add) == $totals[($group.p | tostring)]
        and any($rows[]; .g) and any($rows[]; .h))
' "$@" >/dev/null; then
  echo "invalid, incomplete or duplicate explorer results" >&2
  exit 1
fi
echo "explorer results verified"
