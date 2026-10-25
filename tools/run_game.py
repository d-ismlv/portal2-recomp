#!/usr/bin/env python3
"""Run the native executable against the user's real game data and log output."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--build', action='store_true')
parser.add_argument('--timeout', type=int, default=60)
parser.add_argument('--show', action='store_true', help='afficher la fenêtre (sinon P2_HIDDEN_WINDOWS=1)')
parser.add_argument('arguments', nargs=argparse.REMAINDER)
args = parser.parse_args()
if args.build:
    result = subprocess.run([sys.executable, 'tools/build_game.py'])
    if result.returncode: sys.exit(result.returncode)
config = json.loads(Path('config/local.json').read_text())
game_args = args.arguments
if game_args[:1] == ['--']: game_args = game_args[1:]
if not game_args: game_args = ['-game', 'portal2', '-novid', '-windowed', '-w', '1280', '-h', '720']
command = [str(Path('build/portal2-native').resolve()), '--images', str(Path('build/game/prep').resolve()),
           '--game-dir', config['game_root'], '--linux-root', config['linux_root'],
           '--user-dir', str(Path('../work/portal2-userdata').resolve()), '--', *game_args]
log = Path('build/game/launch.log')
with log.open('w') as output:
    try:
        env = dict(os.environ)
        if not args.show: env.setdefault('P2_HIDDEN_WINDOWS', '1')
        result = subprocess.run(command, stdout=output, stderr=subprocess.STDOUT, timeout=args.timeout, env=env)
        status = result.returncode
    except subprocess.TimeoutExpired:
        status = 124
        output.write('\nNative launch exceeded the diagnostic timeout. This is not proof of a running game.\n')
print(log.read_text()[-12000:])
print(f'native process exit: {status}')
sys.exit(status if status >= 0 else 128 - status)
