#!/system/bin/sh
MODDIR=${0%/*}
STATE_DIR="/data/adb/ts-lan-route"
abi="$(getprop ro.product.cpu.abi)"
case "$abi" in
    arm64-v8a|armeabi-v7a|x86_64|x86) ;;
    *) log -t TS-LAN-Route "Unsupported CPU ABI"; exit 1 ;;
esac
WATCH="$MODDIR/bin/$abi/tslr-watch"
if [ ! -x "$WATCH" ]; then
    log -t TS-LAN-Route "Missing watcher: $WATCH"
    exit 1
fi
if [ "$1" = stop ]; then
    exec "$WATCH" stop "$MODDIR" "$STATE_DIR"
fi
[ ! -e "$MODDIR/disable" ] && [ ! -e "$MODDIR/remove" ] || exit 0
"$WATCH" watch "$MODDIR" "$STATE_DIR" </dev/null >/dev/null 2>&1 &
