#!/system/bin/sh
MODDIR=${0%/*}
version=$(awk -F= '$1 == "version" { print $2; exit }' "$MODDIR/module.prop" 2>/dev/null)
printf '%s\n' '========================================'
printf '  Tailscale Exit Node LAN Route Fix %s\n' "${version:-unknown}"
printf '%s\n' '========================================'
printf '[INFO] 実行時刻: %s\n' "$(date '+%Y-%m-%d %H:%M:%S')"
/system/bin/sh "$MODDIR/system/bin/ts-lan-route" apply
result=$?
if [ "$result" -eq 0 ]; then
    printf '%s\n' '[SUCCESS] Action完了'
else
    printf '[FAILED] Action終了 (終了コード: %s)\n' "$result"
fi
printf '%s\n' '========================================'
exit "$result"
