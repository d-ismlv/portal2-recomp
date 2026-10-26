#!/usr/bin/env python3
"""Signe un paquet .app iOS pour un appareil réel.

Prend le profil de provisionnement le plus récent obtenu par Xcode pour le
CFBundleIdentifier du paquet (voir ios/signing/project.yml), l'embarque et
signe avec l'identité « Apple Development » de son équipe et les droits du
profil.

    python3 ios/signing/sign.py build/ios/device/Portal2.app
"""
import plistlib
import subprocess
import sys
import tempfile
from pathlib import Path

PROFILES = [Path.home() / 'Library/Developer/Xcode/UserData/Provisioning Profiles',
            Path.home() / 'Library/MobileDevice/Provisioning Profiles']


def decode(profile):
    raw = subprocess.run(['security', 'cms', '-D', '-i', str(profile)], capture_output=True, check=True).stdout
    return plistlib.loads(raw)


def find_profile(bundle_id):
    best = None
    for folder in PROFILES:
        for path in folder.glob('*.mobileprovision') if folder.is_dir() else []:
            data = decode(path)
            app_id = data['Entitlements'].get('application-identifier', '')
            if app_id.split('.', 1)[-1] != bundle_id:
                continue
            if best is None or data['CreationDate'] > best[1]['CreationDate']:
                best = (path, data)
    if not best:
        sys.exit(f'aucun profil pour {bundle_id} : lancer xcodebuild sur ios/signing (voir project.yml)')
    return best


def identity(team):
    found = subprocess.run(['security', 'find-identity', '-v', '-p', 'codesigning'],
                           capture_output=True, text=True, check=True).stdout
    for line in found.splitlines():
        if '"Apple Development' in line:
            return line.split()[1]   # empreinte : sans ambiguïté s'il y a plusieurs certificats
    sys.exit(f'aucune identité Apple Development pour l\'équipe {team}')


def main():
    app = Path(sys.argv[1])
    bundle_id = plistlib.loads((app / 'Info.plist').read_bytes())['CFBundleIdentifier']
    path, data = find_profile(bundle_id)
    team = data['TeamIdentifier'][0]
    (app / 'embedded.mobileprovision').write_bytes(path.read_bytes())
    with tempfile.NamedTemporaryFile(suffix='.plist', delete=False) as f:
        plistlib.dump(data['Entitlements'], f)
    subprocess.run(['codesign', '-f', '-s', identity(team), '--entitlements', f.name, '--generate-entitlement-der',
                    str(app)], check=True)
    print(f'{app} signé ({bundle_id}, équipe {team}, profil expirant le {data["ExpirationDate"]:%Y-%m-%d})')


if __name__ == '__main__':
    main()
