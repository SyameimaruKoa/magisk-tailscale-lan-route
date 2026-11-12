# Tailscale Exit Node LAN Route Fix

[magisk-tailscaled](https://github.com/anasfanani/magisk-tailscaled) などのroot権限で稼働するTailscaleで、Exit Node利用中にUSB・Wi-Fi・有線などの直結LANへ接続できなくなる問題を補完するMagisk/KernelSUモジュール。

**手動実行専用**。起動時の自動処理や常駐監視はしない。

## インストール

[Releases](https://github.com/SyameimaruKoa/magisk-tailscale-lan-route/releases) からZIPを取得し、Magisk/KernelSUのモジュール画面でインストールして再起動する。

## 実行

- **Actionボタン**：`apply` を1回実行。
- `su -c 'ts-lan-route apply'`：必要な経路を補完。
- `su -c 'ts-lan-route plan'`：候補を確認（変更なし）。
- `su -c 'ts-lan-route status'`：現在の設定と追加経路を確認。
- `su -c 'ts-lan-route cleanup'`：モジュールが追加した経路のみ削除。
- `su -c 'ts-lan-route --help'`：使い方を表示。

PATHの都合でコマンドが見つからない場合：

```sh
su -c 'sh /data/adb/modules/ts-lan-route/system/bin/ts-lan-route apply'
```

## 動作条件

`tailscale debug prefs` の **`ExitNodeAllowLANAccess=true`** かつ **テーブル52に`default dev tailscale0`が存在する** 場合だけ経路を追加する。falseやExit Node無効時に`apply`すると、本モジュールが追加した経路を削除する。設定取得失敗時は変更しない。

`ip -4 -o route show table all` の `scope link` から、直接接続されたIPv4 LANを検出する。対象は`10.0.0.0/8`、`172.16.0.0/12`、`192.168.0.0/16`、`169.254.0.0/16`。ループバック、Tailscale、携帯回線、VPN・トンネル類のインターフェースは除外する。

既存の経路（Tailscaleが追加した`throw`を含む）は変更せず、テーブル52に不足分だけ追加する。追加経路は`proto 242`で識別し、`cleanup`や次回`apply`で自分の経路だけを削除できるようにする。

例：`eth0=169.254.2.10/24`、`eth1=169.254.1.10/24` がある場合は次の経路を補完する。

```text
169.254.1.0/24 dev eth1 proto 242 scope link
169.254.2.0/24 dev eth0 proto 242 scope link
```

## 制限

- **自動監視・起動時適用なし**。USB再接続、インターフェース変更、Tailscaleの経路再生成後は、Actionボタンまたは`apply`を再実行する。
- IPv4のみ。IPv6の経路は変更しない。
- Androidアプリごとのnetd/fwmark設定などにより、追加した経路だけでは通信が復旧しない場合がある。
- ローカルLAN宛て通信はExit Nodeを経由しない。

## アンインストール

モジュールをアンインストールすると、自身が追加した経路を削除する。手動で先に削除する場合は`cleanup`を実行する。

## ZIPの自動生成

GitHub Actionsの[Build Magisk ZIP](https://github.com/SyameimaruKoa/magisk-tailscale-lan-route/actions/workflows/build.yml)で生成する。`main`へのpush、PR、手動実行でテストとZIPビルドを行い、ActionsのArtifactsに保存する。

`module.prop`の`version`と同じタグ（例：`v1.1.0`）をpushすると、同じZIPをGitHub Releasesのアセットにも自動登録する。タグと`module.prop`のバージョンが一致しない場合はビルドを失敗させる。

生成した`dist/`のZIPはGit管理しない。ActionsのArtifactsはGitHub提供のアーカイブ形式であるため、インストール用にはReleasesのZIPを使用する。
