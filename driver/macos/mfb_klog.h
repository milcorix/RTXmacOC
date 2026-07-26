/*
 * mfb_klog.h — журнал bring-up'а, переживающий чёрный экран и перезагрузку.
 *
 * Зачем. Отладка драйвера дисплея имеет неприятное свойство: когда он ломает
 * вывод, IOLog читать НЕЧЕМ — экран чёрный, а unified log это бинарный tracev3,
 * который снаружи (из Linux) не разобрать. Поэтому весь трейс bring-up'а
 * дублируется в кольцевой буфер и сбрасывается двумя путями:
 *
 *   1) текстовый файл  /Library/Application Support/Milcorix/lastboot.log
 *      — полный лог; читается из Linux через apfs-fuse;
 *   2) переменная NVRAM  milcorix-status (Apple GUID)
 *      — одна строка итога; читается из Linux прямо в /sys/firmware/efi/efivars
 *      и переживает даже случай, когда macOS вообще не догрузилась.
 *
 * Сброс делается «оппортунистически»: на раннем boot'е корневая ФС может быть
 * ещё не смонтирована, поэтому попытка повторяется из методов, которые
 * IOGraphics дёргает уже после старта системы.
 */
#ifndef MILCORIX_KLOG_H
#define MILCORIX_KLOG_H

#include <stdarg.h>

/* Выделить буфер журнала. Повторный вызов безвреден. */
void mfb_klog_init(void);

/* Дописать строку (printf-формат). Безопасно при неинициализированном буфере. */
void mfb_klog_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void mfb_klog_vprintf(const char *fmt, va_list ap);

/*
 * Попытаться сбросить журнал в файл. Возврат 0 — записано.
 * Вызывать можно многократно: файл каждый раз перезаписывается целиком, поэтому
 * поздний вызов даёт более полный лог.
 */
int  mfb_klog_flush(void);

/*
 * Оппортунистический сброс: делает не больше mfb_klog_flush_budget() попыток за
 * загрузку и молчит при неудаче. Дёргается из «горячих» методов IOFramebuffer.
 */
void mfb_klog_flush_lazy(void);

/* Записать однострочный итог в NVRAM (виден из Linux в efivarfs). */
void mfb_klog_status(const char *line);

/* Освободить буфер (stop). */
void mfb_klog_free(void);

#endif /* MILCORIX_KLOG_H */
