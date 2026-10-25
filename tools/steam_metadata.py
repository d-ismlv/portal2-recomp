#!/usr/bin/env python3
"""Read only public app/depot metadata from Steam's local appinfo cache.

Header layout reference: https://github.com/xPaw/SteamAppInfo (versions 39-41).
Record access tokens are skipped and are never returned or printed.
"""
import argparse
import json
from pathlib import Path
import struct


def app_metadata(path, wanted):
    data = Path(path).read_bytes()
    magic, _ = struct.unpack_from('<II', data)
    version = magic & 255
    if magic >> 8 != 0x075644 or version not in (39, 40, 41):
        raise ValueError('unsupported appinfo cache version')
    pool = None
    position = 8
    if version == 41:
        table = struct.unpack_from('<Q', data, position)[0]
        position += 8
        count = struct.unpack_from('<I', data, table)[0]
        offset = table + 4
        pool = []
        for _ in range(count):
            end = data.index(0, offset)
            pool.append(data[offset:end].decode('utf-8'))
            offset = end + 1

    def cstring(offset):
        end = data.index(0, offset)
        return data[offset:end].decode('utf-8'), end + 1

    def keyvalues(offset, limit):
        result = {}
        while offset < limit:
            kind = data[offset]; offset += 1
            if kind == 8:
                return result, offset
            if pool is None:
                key, offset = cstring(offset)
            else:
                key = pool[struct.unpack_from('<I', data, offset)[0]]; offset += 4
            if kind == 0:
                value, offset = keyvalues(offset, limit)
            elif kind == 1:
                value, offset = cstring(offset)
            elif kind in (2, 3, 4, 6, 7, 10):
                fmt = {2: '<i', 3: '<f', 4: '<I', 6: '<I', 7: '<Q', 10: '<q'}[kind]
                value = struct.unpack_from(fmt, data, offset)[0]; offset += struct.calcsize(fmt)
            else:
                raise ValueError(f'unsupported KeyValues kind {kind}')
            result[key] = value
        raise ValueError('unterminated app metadata')

    while position + 4 <= len(data):
        appid = struct.unpack_from('<I', data, position)[0]
        if not appid:
            break
        size = struct.unpack_from('<I', data, position + 4)[0]
        end = position + 8 + size
        if end > len(data):
            raise ValueError('truncated app metadata record')
        if appid == wanted:
            body = position + 8 + 40 + (20 if version >= 40 else 0)
            result, _ = keyvalues(body, end)
            return result.get('appinfo', result)
        position = end
    raise ValueError(f'app {wanted} not present in this Steam cache')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('cache', type=Path)
    parser.add_argument('--appid', type=int, default=620)
    args = parser.parse_args()
    metadata = app_metadata(args.cache, args.appid)
    # Expose only fields needed to identify the game binaries.
    print(json.dumps({key: metadata.get(key) for key in ('appid', 'common', 'depots')}, indent=2))
