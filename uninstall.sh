#!/system/bin/sh
MODDIR=${0%/*}
/system/bin/sh "$MODDIR/service.sh" stop || exit 1
/system/bin/sh "$MODDIR/system/bin/ts-lan-route" cleanup
rm -rf /data/adb/ts-lan-route
