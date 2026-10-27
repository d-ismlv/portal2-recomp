#!/usr/bin/env python3
"""Génère runtime/ios/steam_stub_generated.c : une définition hôte de chaque
fonction plate de libsteam_api que le pont (runtime/steam_generated.c) peut
appeler, pour le stub Steam du build iOS (runtime/ios/steam_stub.c).

Par défaut une fonction renvoie 0, false, NULL ou "" ; RESPONSES donne les
réponses d'un utilisateur connecté, hors ligne, qui possède Portal 2 (620).
Une fonction qui renvoie un pointeur à partir d'une version d'interface
renvoie l'objet d'interface du stub. Les fonctions déjà écrites à la main
dans steam_stub.c sont sautées.
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
APP_ID = 620
STEAM_ID = 76561197960265729   # compte individuel public valide, univers 1

# Réponses explicites (corps de fonction C), par nom de fonction plate ;
# a0 est l'objet d'interface (self), a1 le premier argument de la méthode.
RESPONSES = {
    'SteamAPI_ISteamUser_BLoggedOn': 'return true;',
    'SteamAPI_ISteamUser_GetSteamID': f'return {STEAM_ID}ull;',
    'SteamAPI_ISteamUser_GetHSteamUser': 'return 1;',
    'SteamAPI_ISteamApps_BIsSubscribed': 'return true;',
    'SteamAPI_ISteamApps_BIsSubscribedApp': f'return a1 == {APP_ID}u;',
    'SteamAPI_ISteamApps_BIsAppInstalled': f'return a1 == {APP_ID}u;',
    'SteamAPI_ISteamApps_BIsDlcInstalled': 'return false;',
    'SteamAPI_ISteamApps_GetCurrentGameLanguage': 'return "english";',
    'SteamAPI_ISteamApps_GetAvailableGameLanguages': 'return "english";',
    'SteamAPI_ISteamApps_GetAppOwner': f'return {STEAM_ID}ull;',
    'SteamAPI_ISteamUtils_GetAppID': f'return {APP_ID}u;',
    'SteamAPI_ISteamUtils_GetIPCountry': 'return "FR";',
    'SteamAPI_ISteamUtils_GetServerRealTime': 'return (uint32_t)time(NULL);',
    'SteamAPI_ISteamUtils_GetCurrentBatteryPower': 'return 255;',
    'SteamAPI_ISteamUtils_GetConnectedUniverse': 'return 1;',
    'SteamAPI_ISteamUtils_GetSteamUILanguage': 'return "english";',
    'SteamAPI_ISteamFriends_GetPersonaName': 'return "Player";',
    'SteamAPI_ISteamFriends_GetPersonaState': 'return 1;',
    'SteamAPI_ISteamFriends_GetFriendPersonaName': 'return "Player";',
    # Connexion au serveur local (aussi en solo) : ticket factice non vide.
    'SteamAPI_ISteamUser_InitiateGameConnection_DEPRECATED': 'return steam_stub_ticket(a1, a2);',
    'SteamAPI_ISteamUser_GetAuthSessionTicket':
        'int32_t n = steam_stub_ticket(a1, a2); if (a3) *(uint32_t *)a3 = (uint32_t)n; return n > 0 ? 1u : 0u;',
    'SteamAPI_ISteamGameServer_GetAuthSessionTicket':
        'int32_t n = steam_stub_ticket(a1, a2); if (a3) *(uint32_t *)a3 = (uint32_t)n; return n > 0 ? 1u : 0u;',
    'SteamAPI_ISteamGameServer_SendUserConnectAndAuthenticate_DEPRECATED':
        f'if (a4) *(uint64_t *)a4 = {STEAM_ID}ull; return true;',
}


def signatures():
    text = (ROOT / 'runtime/steam_generated.c').read_text()
    found = re.findall(r'typedef ([^;]*?) \(\*F\)\(([^)]*)\);\s*static void \*cache;.*?steam_flat\(c, "([A-Za-z0-9_]+)"\)',
                       text, re.S)
    out = {}
    for ret, args, name in found:
        out.setdefault(name, (ret.strip(), [a.strip() for a in args.split(',') if a.strip()]))
    return out


def handwritten():
    text = (ROOT / 'runtime/ios/steam_stub.c').read_text()
    return set(re.findall(r'^EXPORT [^(]*?\b([A-Za-z0-9_]+)\(', text, re.M))


def default(ret):
    if ret == 'void':
        return ''
    if ret == 'const char *':
        return 'return "";'
    if ret.endswith('*'):
        return 'return NULL;'
    if ret == 'bool':
        return 'return false;'
    return 'return 0;'


def main():
    out = Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / 'runtime/ios/steam_stub_generated.c'
    skip = handwritten()
    lines = ['/* Généré par tools/gen_steam_stub.py depuis runtime/steam_generated.c. Ne pas modifier. */',
             '/* Stub Steam du build iOS seulement : voir runtime/ios/steam_stub.c. */',
             '#include <TargetConditionals.h>', '#if TARGET_OS_IPHONE',
             '#include <stdbool.h>', '#include <stdint.h>', '#include <stddef.h>', '#include <time.h>',
             '#define EXPORT __attribute__((visibility("default"), used))',
             'void *steam_stub_interface(const char *version);',
             'int32_t steam_stub_ticket(void *buffer, int32_t capacity);']
    count = 0
    for name, (ret, args) in sorted(signatures().items()):
        if name in skip:
            continue
        params = ', '.join(f'{t}{"" if t.endswith("*") else " "}a{i}' for i, t in enumerate(args)) or 'void'
        unused = ' '.join(f'(void)a{i};' for i in range(len(args)))
        body = RESPONSES.get(name)
        if body is None and ret == 'void *' and args and args[-1] == 'const char *' and 'GetISteam' in name:
            body = f'return steam_stub_interface(a{len(args) - 1});'
        if body is None:
            body = default(ret)
        lines.append(f'EXPORT {ret}{"" if ret.endswith("*") else " "}{name}({params}) {{ {unused} {body} }}')
        count += 1
    lines += ['#endif', '']
    out.write_text('\n'.join(lines))
    print(f'{count} fonctions plates générées ({len(skip)} écrites à la main) -> {out}')


if __name__ == '__main__':
    main()
