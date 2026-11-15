#!/bin/sh
# Put the official workers back from ID5/Removed-<ts>/ (the hub copies are moved aside, not deleted).
set -e
[ -n "$1" ] || { echo "usage: restore.sh <timestamp printed by swap.sh>"; exit 2; }
P=/var/opt/hmi/data/ID5/Plugins; R=/var/opt/hmi/data/ID5/Removed-$1; A=/var/opt/hmi/data/ID5/Removed-hub-$(date +%Y%m%d-%H%M%S)
hu "test -f $R/BoostDeck/boostdeck-collector-qnx && test -f $R/ShiftDeck/shift-deck-qnx"
hu "mkdir -p $A/BoostDeck $A/ShiftDeck && mv $P/BoostDeck/boostdeck-collector-qnx $A/BoostDeck/ && mv $P/ShiftDeck/shift-deck-qnx $A/ShiftDeck/ && mv $R/BoostDeck/boostdeck-collector-qnx $P/BoostDeck/ && mv $R/ShiftDeck/shift-deck-qnx $P/ShiftDeck/"
echo "official workers restored"
