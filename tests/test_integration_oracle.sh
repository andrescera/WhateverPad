#!/bin/sh
set -eu
generator=$1
wrapped=$2
selection=${3:-all}
directory=$(dirname "$generator")

run() {
    name=$1
    expected=$2
    fault=$3
    shift 3
    receipt="$directory/integration-oracle-$name.log"
    status=0
    if [ "$fault" = none ]; then
        "$generator" "$@" > "$receipt" 2>&1 || status=$?
    else
        INTEGRATION_ORACLE_FAULT=$fault "$wrapped" "$@" > "$receipt" 2>&1 || status=$?
    fi
    if [ "$status" -ne "$expected" ]; then
        printf 'FAIL oracle-control=%s expected-exit=%s actual-exit=%s\n' "$name" "$expected" "$status" >&2
        exit 1
    fi
    if [ "$fault" != none ]; then
        awk -v fault="$fault" '
          $1 == "ORACLE_FAULT" && $2 == "name=" fault {
            split($3, count, "="); if (count[2] > 0) found=1
          }
          END { exit !found }
        ' "$receipt"
    fi
}

if [ "$selection" = all ] || [ "$selection" = exact-stop ]; then
    run exact-stop 1 duplicate-stop --seed 0
    grep -q '^FAIL integration ' "$receipt"
    echo 'ORACLE_REGRESSION exact-stop PASS fault-exit=1'
fi

if [ "$selection" = all ] || [ "$selection" = b1-tags ]; then
    run b1-tags 0 none --seed 4
    awk '
      $1 == "ASSUMPTIONS" && $2 == "L1=respected" && $3 == "B1=respected" &&
        $4 == "single-producer=respected" && $5 == "schedules=1" { tags=1 }
      $1 == "PROPERTY" && $2 == "policy=SINGLE_PRODUCER-projection" &&
        $3 == "undo-needs-own-ack-barrier-and-version" &&
        $5 == "eligible-violations=1" && $6 == "residual-violations=0" { eligible=1 }
      $1 == "INTEGRATION" {
        for (i=2; i<=NF; i++) if ($i == "two-duplicate-traces=1") copies=1
      }
      END { exit !(tags && eligible && copies) }
    ' "$receipt" || { echo 'FAIL oracle-control=b1-tags' >&2; exit 1; }
    echo 'ORACLE_REGRESSION b1-tags PASS seed=4 copies=2 eligible=1'
fi

if [ "$selection" = all ] || [ "$selection" = strict-xpass ]; then
    run strict-fixture-complete 1 none --strict-fixture complete
    grep -q '^STRICT expected=2 xfail=2 xpass=0$' "$receipt"
    run strict-xpass 2 none --strict-fixture missing
    grep -q '^XPASS policy=fixture property=fixture-disappears todo=0$' "$receipt"
    grep -q '^STRICT expected=2 xfail=1 xpass=1$' "$receipt"
    run strict-ledger 1 none --expected-failures
    grep -q '^STRICT expected=21 xfail=21 xpass=0$' "$receipt"
    test "$(grep -c '^SHRUNK ' "$receipt")" -eq 11
    awk '
      $1 == "PROPERTY" && $3 == "undelivered-retry-is-not-admitted" {
        if ($4 != "violations=0/500" || $7 != "PASS") exit 1
        passed++
      }
      END { if (passed != 2) exit 1 }
    ' "$receipt"
    grep -q '^INTEGRATION .* hard=0 xpass=0 ' "$receipt"
    echo 'ORACLE_REGRESSION strict-xpass PASS fixture-exit=2 baseline-exit=1 expected=21'
fi

if [ "$selection" = all ] || [ "$selection" = reassignment ]; then
    run reassignment 1 omit-reassignment --seed 8
    grep -q '^FAIL integration ' "$receipt"
    echo 'ORACLE_REGRESSION reassignment PASS fault-exit=1'
fi

if [ "$selection" = all ] || [ "$selection" = missing-cross ]; then
    run missing-cross 1 missing-cross --seed 0
    grep -q '^FAIL integration ' "$receipt"
    echo 'ORACLE_REGRESSION missing-cross PASS fault-exit=1'
fi

if [ "$selection" = all ]; then
    "$generator" --seed 4 > "$directory/integration-oracle-seed4.log"
    env -u INTEGRATION_ORACLE_FAULT "$wrapped" --seed 4 > "$directory/integration-oracle-unmodified.log"
    cmp "$directory/integration-oracle-seed4.log" "$directory/integration-oracle-unmodified.log"
    echo 'ORACLE_REGRESSION unmodified-wrapper PASS cmp-exit=0'
fi
