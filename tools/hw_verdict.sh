#!/usr/bin/env bash
#
# hw_verdict.sh — короткий вердикт по логу прогона на железе.
#
# Лог tools/gsp-boot.log — это несколько тысяч строк трассировки. Здесь из него
# вынимается ровно то, что отвечает на вопрос «что заработало»: по слою на
# строку, плюс данные, которые нужны для следующего шага (контекстные буферы
# графического движка, таблица прерываний, список движков карты).
#
# Запуск:  tools/hw_verdict.sh [путь-к-логу]
set -u

LOG="${1:-$(cd "$(dirname "$0")" && pwd)/gsp-boot.log}"
if [ ! -f "$LOG" ]; then
    echo "лога нет: $LOG"
    echo "Сначала прогон на железе (GUI пропадёт на минуту и вернётся):"
    echo "  sudo systemd-run --unit=rtx-gsp --collect bash $(cd "$(dirname "$0")" && pwd)/run-gsp-boot-detached.sh"
    exit 1
fi

has() { grep -q -- "$1" "$LOG"; }
line() { grep -m1 -- "$1" "$LOG" | sed 's/^[[:space:]]*//'; }
verdict() { # verdict "<название>" "<признак успеха>"
    if has "$2"; then printf '  \033[32m✔\033[0m %s\n' "$1"
    else               printf '  \033[31m✘\033[0m %s\n' "$1"; fi
}

echo "=== ВЕРДИКТ ПО ПРОГОНУ НА ЖЕЛЕЗЕ ==="
echo "лог: $LOG ($(wc -l < "$LOG") строк, $(date -r "$LOG" '+%F %T'))"
echo ""

echo "Слои:"
verdict "2  GSP в RISC-V стартовал"                 "GSP reset(RISC-V) OK"
verdict "2  GSP-RM ответил по RPC (INIT_DONE)"      "GSP_INIT_DONE"
verdict "3  двусторонний RPC + RM-цепочка"          "СЛОЙ 3 (проход A)"
verdict "3  прямой GMMU (page-tables во VRAM)"      "СЛОЙ 3 (проход D)"
verdict "4  канал GPFIFO создан и запланирован"     "СЛОЙ 4 (проход A)"
verdict "4  первая команда GPU исполнена"           "ПЕРВАЯ КОМАНДА GPU"
verdict "5  дисплеи перечислены"                    "СЛОЙ 5 (A0)"
verdict "5  modeset проглочен"                      "СЛОЙ 5 (C.4d)"
verdict "5  вывод подтверждён железом"              "ВЫВОД ПОДТВЕРЖДЁН ЖЕЛЕЗОМ"
verdict "6  GPU перенёс данные по нашей команде"    "вычислительный путь ЖИВ"
if has "сабмит прошёл, но вывода НЕТ"; then
    printf '  \033[33m!\033[0m %s\n' "5  modeset проглочен, но голова НЕ сканирует — апертура намеренно не отдана"
    line "сабмит прошёл, но вывода НЕТ" | sed 's/^/      /'
fi
echo ""

echo "Слой 6 — что нужно графическому движку:"
if has "контекстные буферы GR доступны"; then
    line "контекстные буферы GR доступны"
    grep -- "ctxbuf\[" "$LOG" | head -12 | sed 's/^/    /'
else
    echo "    (не получено — см. строку KGR_GET_CONTEXT_BUFFERS_INFO в логе)"
    grep -m1 -- "KGR_GET_CONTEXT_BUFFERS_INFO" "$LOG" | sed 's/^/    /'
fi
echo ""

echo "Слой 6 — прерывания (фундамент recovery):"
if has "таблица прерываний получена"; then
    line "таблица прерываний получена"
    grep -- "intr\[" "$LOG" | head -6 | sed 's/^/    /'
else
    grep -m1 -- "INTR_GET_KERNEL_TABLE" "$LOG" | sed 's/^/    /' || echo "    (нет данных)"
fi
echo ""

echo "Движки карты (есть ли NVDEC/NVENC для аппаратного видео):"
grep -- "engn\[" "$LOG" | sed 's/^/    /' || echo "    (таблица движков не прочитана)"
echo ""

if has "СЛОЙ 6: копия НЕ прошла"; then
    echo "Разбор неудачи слоя 6:"
    line "СЛОЙ 6: копия НЕ прошла" | sed 's/^/    /'
    grep -m1 -- "GPU-копия: таймаут" "$LOG" | sed 's/^/    /'
    echo ""
fi

echo "Последние строки лога:"
tail -5 "$LOG" | sed 's/^/    /'
