#!/bin/sh
# Replace every installed copy of the shared hub with ~/nbt-hub/diag-hub-qnx
# (all copies must be the same version: an older owner ignores newer clients).
# Previous copies go to ID5/Removed-<ts>/hub-prev/, never deleted. Needs NBT_PW, relay on 2222.
set -e
P=/var/opt/hmi/data/ID5/Plugins; TS=$(date +%Y%m%d-%H%M%S); R=/var/opt/hmi/data/ID5/Removed-$TS/hub-prev
SUM=$(sha256sum ~/nbt-hub/diag-hub-qnx | cut -d' ' -f1)
FILES="BoostDeck/boostdeck-collector-qnx ShiftDeck/shift-deck-qnx PerfTimers/perf-hub-qnx EngineTemps/temps-hub-qnx"
hu "cat > $P/.diag-hub-stage && chmod 755 $P/.diag-hub-stage" < ~/nbt-hub/diag-hub-qnx
GOT=$(hu "cat $P/.diag-hub-stage" | sha256sum | cut -d' ' -f1)
[ "$GOT" = "$SUM" ] || { echo "read-back mismatch; nothing changed"; hu "rm -f $P/.diag-hub-stage"; exit 1; }
hu "mkdir -p $R"
for f in $FILES; do
  d=${f%/*}
  hu "test -d $P/$d" || { echo "skip $f (app not installed)"; continue; }
  hu "if test -f $P/$f; then mkdir -p $R/$d && mv $P/$f $R/$d/; fi; cp $P/.diag-hub-stage $P/$f && chmod 755 $P/$f"
  [ "$(hu "cat $P/$f" | sha256sum | cut -d' ' -f1)" = "$SUM" ] && echo "ok $f" || echo "MISMATCH $f"
done
hu "rm -f $P/.diag-hub-stage"
echo "Previous copies saved in $R"
