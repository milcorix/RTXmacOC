# Отладка задачи 7 (GSP_INIT_DONE) — заметки 2026-06-30

## Состояние
- Задача 6 ✅: FWSEC-FRTS→WPR2, Booter mbox0=0, GSP RISC-V active=1.
- Очереди RPC + rmargs выставлены (shm 0x81000, ptes=129, msgCount=63).
- GSP-RM **исполняет init**: LOGINIT put=0x3e32 (~16КБ, libos-root/партиции),
  LOGRM put=0x185 (~12 записей RM-задачи), затем застрял. msgq.writePtr=0 (нет INIT_DONE).
  RISC-V active (не halted) → крутится в ошибке, не падает в halt.

## Декодирование логов — ВЫПОЛНИМО
Прошивка `gsp-535.113.01` содержит читаемые формат-строки (2957 с '%', libos-v3.1.0):
записи LOGRM ссылаются на метаописания (VA 0x20a053xx, шаг 0x18) → формат-строка.
Релевантные кандидаты-сбои в прошивке:
  "Failed to map initArgs: %u"          (initArgs = rmargs!)
  "Failed to receive GspFwTaskMappings: %d"
  "Failed to map TASK_INIT_LOG_MEM %u"
  "Failed to map VGPU ELF %s %u" / "Failed to map VGPU ELF %u"
  "Runtime failure: ... @ .../libos-v3.1.0/root/root.c:%d" (ассерты)

## Следующий шаг
Декодер libos-v3-логов: распарсить формат лог-буфера (записи {metadata_va, args...}),
найти массив метаописаний и task-VA-маппинг внутри .fwimage, декодировать LOGRM →
прочитать точную строку сбоя → исправить rmargs/очереди/маппинг. Затем HW-прогон.
