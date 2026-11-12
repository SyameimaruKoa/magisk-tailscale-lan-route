#!/system/bin/sh
MODDIR=${0%/*}
exec /system/bin/sh "$MODDIR/system/bin/ts-lan-route" apply
