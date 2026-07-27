#!/usr/bin/env python3
"""
milcorix_stage.py — переключить стадию драйвера MilcorixFB из Linux.

Драйвер читает boot-arg `milcorix=N` и по умолчанию ВЫКЛЮЧЕН, чтобы установка
kext'а сама по себе не могла оставить машину без картинки. Этот скрипт правит
boot-args в активном OpenCore-конфиге, то есть весь цикл отладки сводится к
«поменять число → ребут», без Recovery и без переустановок.

  0  выключен  — kext не подключается, macOS рисует через EFI-фреймбуфер
  1  bring-up  — поднять GSP (слои 2-4), НЕ трогая вывод: проверка, что драйвер
                 работает и не роняет систему; картинка остаётся EFI-шной
  2  полный    — + modeset и наш фреймбуфер в системной памяти как апертура

Запуск:  sudo python3 tools/milcorix_stage.py <0|1|2> [--status]
         sudo python3 tools/milcorix_stage.py --status     (только показать)
"""
import plistlib
import shutil
import subprocess
import sys
import glob
import os

CONFIG = '/boot/efi/EFI/OC/config.plist'
GUID = '7C436110-AB2A-4BBB-A880-FE41995C9F82'
STAGE_NAMES = {
    0: 'выключен (macOS на EFI-фреймбуфере)',
    1: 'bring-up GSP без modeset (вывод не трогаем)',
    2: 'полный: modeset + наш фреймбуфер',
}


def read_nvram_status():
    """Статус последней загрузки, который kext оставил в NVRAM (виден из Linux)."""
    for path in glob.glob(f'/sys/firmware/efi/efivars/milcorix-status-{GUID}'):
        try:
            with open(path, 'rb') as f:
                raw = f.read()[4:]           # 4 байта атрибутов EFI
            # AppleEFINVRAM хранит нашу строку как есть (ASCII), поэтому UTF-8
            # пробуем первым: UTF-16 на ASCII-байтах даёт иероглифы, а не отказ.
            txt = raw.decode('utf-8', 'ignore').strip('\x00').strip()
            if not txt or not txt.isprintable():
                txt = raw.decode('utf-16-le', 'ignore').strip('\x00').strip()
            return txt
        except OSError as e:
            return f'(не прочитать: {e})'
    return None


def read_boot_log():
    """Журнал bring-up'а с тома macOS, если он смонтирован через apfs-fuse."""
    for base in ('/mnt/mac/root', '/mnt/macos'):
        p = os.path.join(base, 'Library/Application Support/Milcorix/lastboot.log')
        if os.path.exists(p):
            return p
    return None


def show_status():
    st = read_nvram_status()
    print('=== ИТОГ ПОСЛЕДНЕЙ ЗАГРУЗКИ (NVRAM) ===')
    print(f'  {st}' if st else '  (переменной нет — kext ещё не отработал либо стадия 0)')
    log = read_boot_log()
    print('=== ЖУРНАЛ BRING-UP\'А ===')
    if log:
        print(f'  {log}')
        try:
            print(f'  ({os.path.getsize(log)} байт; читать: less "{log}")')
        except OSError:
            pass
    else:
        print('  том macOS не смонтирован. Смонтировать:')
        print('    sudo apfs-fuse /dev/nvme0n1p3 /mnt/mac')


def set_stage(stage):
    if not os.path.exists(CONFIG):
        sys.exit(f'нет {CONFIG} — EFI смонтирован? (sudo mount /dev/nvme0n1p1 /boot/efi)')

    shutil.copy(CONFIG, CONFIG + '.bak-milcorix')
    with open(CONFIG, 'rb') as f:
        d = plistlib.load(f)

    g = d['NVRAM']['Add'][GUID]
    old = g.get('boot-args', '')
    toks = [t for t in old.split() if not t.startswith('milcorix=')]
    if stage != 0:
        toks.append(f'milcorix={stage}')
    new = ' '.join(toks)
    g['boot-args'] = new

    # boot-args должен быть в Delete, иначе OpenCore не перезапишет значение в NVRAM.
    try:
        dele = d['NVRAM']['Delete'][GUID]
        if 'boot-args' not in dele:
            dele.append('boot-args')
    except (KeyError, TypeError):
        pass

    with open(CONFIG, 'wb') as f:
        plistlib.dump(d, f)
    subprocess.run(['sync'], check=False)

    print(f'СТАДИЯ: {stage} — {STAGE_NAMES[stage]}')
    print(f'boot-args было:  {old}')
    print(f'boot-args стало: {new}')
    print(f'бэкап: {CONFIG}.bak-milcorix')
    print('\nПерезагрузись в macOS. Если картинки не будет — вернись в Linux и:')
    print('    sudo python3 tools/milcorix_stage.py 0')


def main():
    args = [a for a in sys.argv[1:]]
    want_status = '--status' in args
    nums = [a for a in args if a.isdigit()]

    if not nums:
        show_status()
        if not want_status:
            print(f'\nиспользование: sudo python3 {sys.argv[0]} <0|1|2>')
        return

    stage = int(nums[0])
    if stage not in STAGE_NAMES:
        sys.exit('стадия должна быть 0, 1 или 2')
    set_stage(stage)
    if want_status:
        print()
        show_status()


if __name__ == '__main__':
    main()
