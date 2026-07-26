#!/usr/bin/env python3
# Добавляет debug=0x100 keepsyms=1 в boot-args активного OpenCore config.plist,
# чтобы kernel panic ЗАМОРОЗИЛ экран (не ребутился) и показал бэктрейс с именами.
# Запуск: sudo python3 tools/add_debug_bootargs.py
# Откат:  та же логика — просто убрать эти два токена (или восстановить .bak).
import plistlib, shutil

p = '/boot/efi/EFI/OC/config.plist'
shutil.copy(p, p + '.bak-before-debug')

with open(p, 'rb') as f:
    d = plistlib.load(f)

guid = '7C436110-AB2A-4BBB-A880-FE41995C9F82'
g = d['NVRAM']['Add'][guid]
ba = g.get('boot-args', '')
print("BOOT-ARGS БЫЛО:", repr(ba))

toks = ba.split()
for t in ('debug=0x100', 'keepsyms=1'):
    # убрать возможные дубли/старые debug=
    toks = [x for x in toks if x != t and not (t.startswith('debug=') and x.startswith('debug='))]
    toks.append(t)
new = ' '.join(toks)
g['boot-args'] = new

# гарантируем, что NVRAM затрётся свежим значением
try:
    dele = d['NVRAM']['Delete'][guid]
    if 'boot-args' not in dele:
        dele.append('boot-args')
except Exception as e:
    print("Delete-секция:", e)

with open(p, 'wb') as f:
    plistlib.dump(d, f)

print("BOOT-ARGS СТАЛО:", repr(new))
print("OK, backup:", p + '.bak-before-debug')
