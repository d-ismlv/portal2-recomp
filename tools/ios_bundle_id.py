#!/usr/bin/env python3
"""Identifiant de l'app iOS (CFBundleIdentifier).

Chaque compte Apple doit signer sous son propre identifiant : P2_BUNDLE_ID,
sinon "ios_bundle_id" dans config/local.json, sinon un identifiant d'exemple
qui suffit au simulateur. La sonde (ios/probe) prend l'identifiant suivi de
« .probe ».

    python3 tools/ios_bundle_id.py [--probe]
"""
import json
import os
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DEFAULT = 'com.example.portal2-recomp'


def bundle_id(probe=False):
    value = os.environ.get('P2_BUNDLE_ID')
    if not value:
        try:
            value = json.loads((ROOT / 'config/local.json').read_text()).get('ios_bundle_id')
        except FileNotFoundError:
            value = None
    value = value or DEFAULT
    return value + '.probe' if probe else value


if __name__ == '__main__':
    print(bundle_id('--probe' in sys.argv[1:]))
