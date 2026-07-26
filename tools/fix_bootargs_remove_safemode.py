#!/usr/bin/env python3
# Убирает ВСЕ -x (safe mode) из boot-args активного OpenCore config.plist.
# Запуск: sudo python3 tools/fix_bootargs_remove_safemode.py
import plistlib, shutil, sys

p = '/boot/efi/EFI/OC/config.plist'
shutil.copy(p, p + '.bak-before-recovery')

with open(p, 'rb') as f:
    d = plistlib.load(f)

guid = '7C436110-AB2A-4BBB-A880-FE41995C9F82'
g = d['NVRAM']['Add'][guid]
ba = g.get('boot-args', '')
print("BOOT-ARGS БЫЛО:", repr(ba))

new = ' '.join(x for x in ba.split() if x != '-x')
g['boot-args'] = new

# boot-args уже есть в NVRAM.Delete (проверено) — старое значение в NVRAM затрётся.
try:
    dele = d['NVRAM']['Delete'][guid]
    if 'boot-args' not in dele:
        dele.append('boot-args')
    print("NVRAM.Delete boot-args:", 'boot-args' in dele)
except Exception as e:
    print("Delete-секция:", e)

with open(p, 'wb') as f:
    plistlib.dump(d, f)

print("BOOT-ARGS СТАЛО:", repr(new))
print("csr-active-config:", g.get('csr-active-config'))
print("Misc.Boot.HideAuxiliary:", d.get('Misc', {}).get('Boot', {}).get('HideAuxiliary'))
print("OK, backup:", p + '.bak-before-recovery')
