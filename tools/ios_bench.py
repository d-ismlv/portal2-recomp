#!/usr/bin/env python3
"""Banc de cadence sur l'iPhone : enchaîne des configurations et mesure.

Chaque configuration fixe l'antialiasing (setting.mat_antialias de
update/cfg/video.txt), l'échelle de rendu (P2_IOS_RENDER_SCALE) et la
résolution assortie (932×430 points × échelle sur iPhone 15 Pro Max), lance
le jeu (Documents/launch.args choisit la salle), attend, puis donne la
moyenne de la seconde moitié des mesures P2_FRAME_LOG. Le video.txt
d'origine est remis à la fin.

    python3 tools/ios_bench.py --seconds 120 msaa4:4:3 msaa0:0:3 msaa0-x2:0:2
    (nom:antialias:échelle ; échelle « native » ou un nombre)
"""
import argparse
import json
import re
import statistics
import subprocess
import sys
import time
from pathlib import Path

APP = 'fr.kanapitsas.portal2'
VIDEO = 'Documents/portal2-userdata/update/cfg/video.txt'
OUT = Path('build/ios/device/bench')


def device():
    listing = subprocess.run(['xcrun', 'devicectl', 'list', 'devices'], capture_output=True, text=True).stdout
    for line in listing.splitlines():
        if 'physical' in line and ('connected' in line or 'available (paired)' in line):
            m = re.search(r'[0-9A-F]{8}-[0-9A-F]{16}', line)
            if m: return m.group(0)
    sys.exit('aucun iPhone connecté')


def devicectl(udid, *args, check=True):
    sub, rest = list(args[:3]), list(args[3:])
    return subprocess.run(['xcrun', 'devicectl', *sub, '--device', udid, *rest], capture_output=True, text=True,
                          check=check)


def pull(udid, source, destination):
    devicectl(udid, 'device', 'copy', 'from', '--domain-type', 'appDataContainer', '--domain-identifier', APP,
              '--source', source, '--destination', str(destination))


def push(udid, source, destination):
    devicectl(udid, 'device', 'copy', 'to', '--domain-type', 'appDataContainer', '--domain-identifier', APP,
              '--source', str(source), '--destination', destination)


def with_settings(text, values):
    for key, value in values.items():
        pattern = rf'("setting\.{key}"\s+)"[^"]*"'
        if re.search(pattern, text): text = re.sub(pattern, rf'\g<1>"{value}"', text)
        else: text = text.replace('}', f'\t"setting.{key}"\t\t"{value}"\n}}', 1)
    return text


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('configs', nargs='+', help='nom:antialias:échelle')
    ap.add_argument('--seconds', type=int, default=120)
    ap.add_argument('--points', default='932x430', help="taille de l'écran en points (paysage)")
    args = ap.parse_args()
    udid = device()
    OUT.mkdir(parents=True, exist_ok=True)
    original = OUT / 'video.orig.txt'
    pull(udid, VIDEO, original)
    base = original.read_text()
    pw, ph = (int(v) for v in args.points.split('x'))
    results = []
    try:
        for spec in args.configs:
            name, aa, scale = spec.split(':')
            factor = 3.0 if scale == 'native' else float(scale)
            video = OUT / f'video.{name}.txt'
            video.write_text(with_settings(base, {'mat_antialias': aa, 'mat_aaquality': '0',
                                                  'defaultres': round(pw * factor), 'defaultresheight': round(ph * factor)}))
            push(udid, video, VIDEO)
            env = {'P2_LOG_FILE': 'Documents/game.log', 'P2_FRAME_LOG': '1', 'P2_IOS_RENDER_SCALE': scale}
            launched = devicectl(udid, 'device', 'process', 'launch', '--terminate-existing', '--environment-variables',
                                 json.dumps(env), APP, check=False)
            if launched.returncode:
                sys.exit(f'{name} : lancement refusé (iPhone verrouillé ?)\n{launched.stderr[-400:]}')
            time.sleep(args.seconds)
            log = OUT / f'game.{name}.log'
            pull(udid, 'Documents/game.log', log)
            fps = [float(m) for m in re.findall(r'\[diag\] frame \d+, ([0-9.]+) fps', log.read_text())]
            size = re.search(r'contexte ES 3, FBO système \d+, (\d+x\d+)', log.read_text())
            steady = fps[len(fps) // 2:] or [0]
            results.append((name, size.group(1) if size else '?', statistics.mean(steady), min(steady), max(steady)))
            print(f'{name:12} {results[-1][1]:>10}  moyenne {results[-1][2]:5.1f}  min {results[-1][3]:5.1f}  '
                  f'max {results[-1][4]:5.1f} i/s', flush=True)
    finally:
        push(udid, original, VIDEO)
        print('video.txt d\'origine remis')


if __name__ == '__main__':
    main()
