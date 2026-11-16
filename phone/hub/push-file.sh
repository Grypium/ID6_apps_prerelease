#!/bin/sh
# Replace one installed plugin file with the staged copy: push-file.sh App/file
# (from ~/nbt-newapps). Old copy -> ID5/Removed-<ts>/<App>/, read back and checked.
# Needs NBT_PW, relay on 2222.
set -e
f=$1; P=/var/opt/hmi/data/ID5/Plugins; TS=$(date +%Y%m%d-%H%M%S); R=/var/opt/hmi/data/ID5/Removed-$TS
SUM=$(sha256sum ~/nbt-newapps/$f | cut -d' ' -f1)
hu "cat > $P/.push-stage" < ~/nbt-newapps/$f
[ "$(hu "cat $P/.push-stage" | sha256sum | cut -d' ' -f1)" = "$SUM" ] || { hu "rm -f $P/.push-stage"; echo "read-back mismatch; nothing changed"; exit 1; }
hu "mkdir -p $R/${f%/*} && mv $P/$f $R/${f%/*}/ && mv $P/.push-stage $P/$f && chmod 644 $P/$f"
[ "$(hu "cat $P/$f" | sha256sum | cut -d' ' -f1)" = "$SUM" ] && echo "ok $f (old copy in $R)" || echo "MISMATCH $f"
