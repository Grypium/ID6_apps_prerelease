#!/bin/sh
# Put the shared hub in place of BoostDeck's and ShiftDeck's workers.
# Originals are moved to ID5/Removed-<ts>/, never deleted. Needs NBT_PW, relay on 2222.
set -e
P=/var/opt/hmi/data/ID5/Plugins; TS=$(date +%Y%m%d-%H%M%S); R=/var/opt/hmi/data/ID5/Removed-$TS
SUM=$(sha256sum ${HUB_BIN:-$HOME/nbt-hub/diag-hub-qnx} | cut -d' ' -f1)
hu "test -f $P/BoostDeck/boostdeck-collector-qnx && test -f $P/ShiftDeck/shift-deck-qnx"
hu "cat > $P/.diag-hub-stage && chmod 755 $P/.diag-hub-stage" < ${HUB_BIN:-$HOME/nbt-hub/diag-hub-qnx}
GOT=$(hu "cat $P/.diag-hub-stage" | sha256sum | cut -d' ' -f1)
[ "$GOT" = "$SUM" ] || { echo "read-back mismatch; nothing changed"; hu "rm -f $P/.diag-hub-stage"; exit 1; }
hu "mkdir -p $R/BoostDeck $R/ShiftDeck && mv $P/BoostDeck/boostdeck-collector-qnx $R/BoostDeck/ && mv $P/ShiftDeck/shift-deck-qnx $R/ShiftDeck/ && cp $P/.diag-hub-stage $P/BoostDeck/boostdeck-collector-qnx && mv $P/.diag-hub-stage $P/ShiftDeck/shift-deck-qnx && chmod 755 $P/BoostDeck/boostdeck-collector-qnx $P/ShiftDeck/shift-deck-qnx"
for f in BoostDeck/boostdeck-collector-qnx ShiftDeck/shift-deck-qnx; do
  [ "$(hu "cat $P/$f" | sha256sum | cut -d' ' -f1)" = "$SUM" ] && echo "ok $f" || echo "MISMATCH $f"
done
echo "Originals saved in $R (restore: restore.sh $TS)"
