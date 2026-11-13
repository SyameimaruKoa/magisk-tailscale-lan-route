import os
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MODULE = ROOT / 'system/bin/ts-lan-route'

MOCK_IP = '''#!/usr/bin/env python3
import json
import os
import sys
from pathlib import Path
p = Path(os.environ['MOCK_ROUTE_FILE'])
args = sys.argv[1:]
rows = json.loads(p.read_text())
if args[:2] == ['-4', '-o'] and args[2:5] == ['route', 'show', 'table'] and args[5] == 'all':
    print('\\n'.join([
        '192.168.55.0/24 dev wlan0 table wlan0_local proto static scope link',
        '169.254.1.0/24 dev eth1 table eth1 proto static scope link',
        '169.254.2.0/24 dev eth0 table eth0 proto static scope link',
        '172.21.0.0/16 dev docker0 proto kernel scope link',
        '100.64.0.0/10 dev tailscale0 scope link',
        '192.168.55.0/24 dev wlan0 proto kernel scope link',
        *rows
    ]))
elif args[0] == '-4' and args[1:4] == ['route', 'show', 'table'] and args[4] == '52':
    print('\\n'.join(rows))
elif args[:3] == ['-4', '-o', 'route'] and args[3:6] == ['show', 'table', '52']:
    print('\\n'.join(rows))
elif args[:2] == ['-4', 'route'] and args[2] == 'add':
    if os.environ.get('MOCK_FAIL_ADD') == '1':
        print('RTNETLINK answers: Operation not permitted', file=sys.stderr)
        sys.exit(2)
    cidr, dev = args[3], args[5]
    row = f'{cidr} dev {dev} proto 242 scope link'
    if row in rows:
        sys.exit(1)
    rows.append(row)
    p.write_text(json.dumps(rows))
elif args[:2] == ['-4', 'route'] and args[2] == 'del':
    cidr, dev = args[3], args[5]
    matching = [r for r in rows if r.startswith(f'{cidr} dev {dev} proto 242')]
    if not matching:
        sys.exit(1)
    rows.remove(matching[0])
    p.write_text(json.dumps(rows))
else:
    print('unexpected ip args:', args, file=sys.stderr)
    sys.exit(4)
'''

MOCK_TS = '''#!/usr/bin/env python3
import os
import sys
from pathlib import Path
if sys.argv[1:] != ['debug', 'prefs']:
    sys.exit(4)
value = Path(os.environ['MOCK_PREF_FILE']).read_text().strip()
if value == 'invalid':
    sys.exit(1)
if value.startswith('{'):
    print(value)
else:
    print('{"ExitNodeAllowLANAccess": ' + value + '}')
'''


class RouteTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.routes = self.root / 'routes.json'
        self.pref = self.root / 'prefs.txt'
        self.ip = self.root / 'ip'
        self.tailscale = self.root / 'tailscale'
        self.routes.write_text(json_dump(['default dev tailscale0', 'throw 192.168.55.0/24']))
        self.pref.write_text('true')
        self.ip.write_text(MOCK_IP)
        self.tailscale.write_text(MOCK_TS)
        self.ip.chmod(0o755)
        self.tailscale.chmod(0o755)
        self.fake_id = self.root / 'id'
        self.fake_id.write_text('#!/bin/sh\n[ "$1" = "-u" ] && { echo 0; exit 0; }\nexec /usr/bin/id "$@"\n')
        self.fake_id.chmod(0o755)
        self.env = {**os.environ, 'PATH': f'{self.root}{os.pathsep}{os.environ.get("PATH", "")}', 'TSLR_STATE_DIR': str(self.root / 'state'), 'TSLR_IP_CMD': str(self.ip), 'TSLR_TAILSCALE_CMD': str(self.tailscale), 'MOCK_ROUTE_FILE': str(self.routes), 'MOCK_PREF_FILE': str(self.pref)}

    def run_cli(self, *args):
        return subprocess.run(['/bin/sh', str(MODULE), *args], env=self.env, text=True, capture_output=True)

    def get_routes(self):
        import json
        return json.loads(self.routes.read_text())

    def test_add_and_no_duplicates(self):
        self.assertEqual(self.run_cli('apply').returncode, 0)
        rows = self.get_routes()
        self.assertIn('169.254.1.0/24 dev eth1 proto 242 scope link', rows)
        self.assertIn('169.254.2.0/24 dev eth0 proto 242 scope link', rows)
        self.assertNotIn('192.168.55.0/24 dev wlan0 proto 242 scope link', rows)
        self.assertEqual(self.run_cli('apply').returncode, 0)
        self.assertEqual(rows, self.get_routes())

    def test_false_cleans_only_own(self):
        self.run_cli('apply')
        self.pref.write_text('false')
        self.assertEqual(self.run_cli('apply').returncode, 0)
        self.assertEqual(self.get_routes(), ['default dev tailscale0', 'throw 192.168.55.0/24'])

    def test_no_exit_cleans_only_own(self):
        self.run_cli('apply')
        self.routes.write_text(json_dump([r for r in self.get_routes() if r != 'default dev tailscale0']))
        self.assertEqual(self.run_cli('apply').returncode, 0)
        self.assertEqual(self.get_routes(), ['throw 192.168.55.0/24'])

    def test_invalid_prefs_preserves_routes(self):
        self.run_cli('apply')
        before = self.get_routes()
        self.pref.write_text('invalid')
        self.assertNotEqual(self.run_cli('apply').returncode, 0)
        self.assertEqual(before, self.get_routes())

    def test_cleanup(self):
        self.run_cli('apply')
        self.assertEqual(self.run_cli('cleanup').returncode, 0)
        self.assertEqual(self.get_routes(), ['default dev tailscale0', 'throw 192.168.55.0/24'])

    def test_action_resolves_installed_tailscale_binary_without_path(self):
        installed = self.root / 'magisk-bin'
        installed.mkdir()
        (installed / 'tailscale').symlink_to(self.tailscale)
        self.env.pop('TSLR_TAILSCALE_CMD')
        self.env['TSLR_TAILSCALE_BIN_DIR'] = str(installed)
        result = self.run_cli('apply')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('169.254.1.0/24 dev eth1 proto 242 scope link', self.get_routes())

    def test_action_missing_binary_preserves_routes(self):
        self.env['TSLR_TAILSCALE_CMD'] = str(self.root / 'missing' / 'tailscale')
        result = self.run_cli('apply')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('Tailscale CLIが見つからない', result.stderr)
        self.assertEqual(self.get_routes(), ['default dev tailscale0', 'throw 192.168.55.0/24'])

    def test_action_cli_failure_reports_binary_path(self):
        self.pref.write_text('invalid')
        result = self.run_cli('apply')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn(str(self.tailscale), result.stderr)
        self.assertIn('debug prefs', result.stderr)

    def test_pref_parser_without_gnu_sed_extensions(self):
        sed = self.root / 'sed'
        sed.write_text('#!/bin/sh\necho "sed: unsupported BRE alternation" >&2\nexit 1\n')
        sed.chmod(0o755)
        result = self.run_cli('apply')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('169.254.1.0/24 dev eth1 proto 242 scope link', self.get_routes())

    def test_pref_parser_compact_json(self):
        self.pref.write_text('{"Other": false, "ExitNodeAllowLANAccess": true, "RouteAll": true}')
        result = self.run_cli('apply')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('169.254.2.0/24 dev eth0 proto 242 scope link', self.get_routes())

    def test_pref_parser_missing_field_keeps_routes(self):
        self.pref.write_text('{"Other": true}')
        result = self.run_cli('apply')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('ExitNodeAllowLANAccessが見つからない', result.stderr)
        self.assertEqual(self.get_routes(), ['default dev tailscale0', 'throw 192.168.55.0/24'])

    def test_pref_parser_rejects_non_boolean(self):
        self.pref.write_text('{"ExitNodeAllowLANAccess": "true"}')
        result = self.run_cli('apply')
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(self.get_routes(), ['default dev tailscale0', 'throw 192.168.55.0/24'])

    def test_action_logs_added_existing_and_summary(self):
        first = self.run_cli('apply')
        self.assertEqual(first.returncode, 0, first.stderr)
        self.assertIn('[INFO] Tailscale CLI:', first.stdout)
        self.assertIn('[INFO] ExitNodeAllowLANAccess: true', first.stdout)
        self.assertIn('[INFO] Exit Node経路: 有効', first.stdout)
        self.assertIn('[INFO] 対象の直結IPv4 LAN: 3 件', first.stdout)
        self.assertIn('[ADD] 169.254.1.0/24 dev eth1', first.stdout)
        self.assertIn('[ADD] 169.254.2.0/24 dev eth0', first.stdout)
        self.assertIn('[SKIP] 192.168.55.0/24 dev wlan0', first.stdout)
        self.assertIn('[RESULT] 追加=2 / 既存=1 / 削除=0 / 失敗=0', first.stdout)
        again = self.run_cli('apply')
        self.assertEqual(again.returncode, 0, again.stderr)
        self.assertIn('[RESULT] 追加=0 / 既存=3 / 削除=0 / 失敗=0', again.stdout)

    def test_action_logs_false_cleanup_result(self):
        self.assertEqual(self.run_cli('apply').returncode, 0)
        self.pref.write_text('false')
        result = self.run_cli('apply')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('[INFO] ExitNodeAllowLANAccess: false', result.stdout)
        self.assertIn('[WARN] LANアクセス許可が無効', result.stdout)
        self.assertIn('[RESULT] 追加=0 / 既存=0 / 削除=2 / 失敗=0', result.stdout)

    def test_action_logs_no_exit_cleanup(self):
        self.assertEqual(self.run_cli('apply').returncode, 0)
        self.routes.write_text(json_dump([r for r in self.get_routes() if r != 'default dev tailscale0']))
        result = self.run_cli('apply')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('[WARN] Exit Node経路: 見つからない', result.stdout)
        self.assertIn('[RESULT] 追加=0 / 既存=0 / 削除=2 / 失敗=0', result.stdout)

    def test_route_add_failure_is_reported(self):
        self.env['MOCK_FAIL_ADD'] = '1'
        result = self.run_cli('apply')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('[RESULT] 追加=0 / 既存=1 / 削除=0 / 失敗=2', result.stdout)
        self.assertIn('RTNETLINK answers: Operation not permitted', result.stderr)
        self.assertIn('一部の経路更新に失敗', result.stderr)

    def test_cleanup_shows_result(self):
        self.assertEqual(self.run_cli('apply').returncode, 0)
        result = self.run_cli('cleanup')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('[REMOVE] 169.254.1.0/24 dev eth1', result.stdout)
        self.assertIn('[RESULT] 追加=0 / 既存=0 / 削除=2 / 失敗=0', result.stdout)

    def test_help(self):
        self.assertIn('使い方:', self.run_cli('--help').stdout)
        self.assertIn('使い方:', self.run_cli().stdout)
        self.assertNotIn('auto on', self.run_cli('--help').stdout)


def json_dump(rows):
    import json
    return json.dumps(rows)


if __name__ == '__main__':
    unittest.main()
