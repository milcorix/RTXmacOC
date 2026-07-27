/*
 * milcorix_gpu.c — userspace-утилита слоя 6: обычная программа заставляет
 * видеокарту выполнить работу.
 *
 * Собирается и запускается НА macOS:
 *   clang -O2 -framework IOKit -framework CoreFoundation \
 *         tools/milcorix_gpu.c -o build/milcorix_gpu
 *   ./build/milcorix_gpu
 *
 * Что делает: подключается к нашему драйверу, кладёт узнаваемый узор в память
 * GPU, просит движок копирования перенести его в другое место, читает результат
 * обратно и сверяет побайтно. Это не эмуляция и не имитация — данные реально
 * перемещает видеокарта по нашей команде.
 *
 * Скорость здесь заведомо низкая: данные ходят через 32-битное окно PRAMIN,
 * потому что BAR1 сейчас раскрыт лишь на консольный регион. Значение имеет факт
 * исполнения, а не пропускная способность. Когда включат Resizable BAR, обмен
 * станет обычным маппингом памяти.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <IOKit/IOKitLib.h>

#define MILCORIX_CONNECT_TYPE   0x4D4C4358u   /* 'MLCX' */

enum {
    kMilcorixMethodGetInfo = 0,
    kMilcorixMethodWrite   = 1,
    kMilcorixMethodRead    = 2,
    kMilcorixMethodCopy    = 3,
};

typedef struct {
    uint32_t ready;
    uint32_t channel;
    uint32_t copy_engine;
    uint32_t reserved;
    uint64_t scratch_size;
} MilcorixGpuInfo;

static io_connect_t open_driver(void)
{
    CFMutableDictionaryRef match = IOServiceMatching("MilcorixFB");
    if (!match) { fprintf(stderr, "не удалось составить критерий поиска\n"); return 0; }

    io_service_t svc = IOServiceGetMatchingService(kIOMainPortDefault, match);
    if (!svc) {
        fprintf(stderr,
            "драйвер MilcorixFB не найден.\n"
            "  - установлен ли kext (sudo tools/macos_install.sh)?\n"
            "  - задан ли boot-arg milcorix=2? По умолчанию драйвер выключен.\n");
        return 0;
    }

    io_connect_t conn = 0;
    kern_return_t kr = IOServiceOpen(svc, mach_task_self(), MILCORIX_CONNECT_TYPE, &conn);
    IOObjectRelease(svc);
    if (kr != KERN_SUCCESS) {
        fprintf(stderr, "IOServiceOpen не прошёл: 0x%x\n", kr);
        return 0;
    }
    return conn;
}

int main(int argc, char **argv)
{
    uint32_t bytes = (argc > 1) ? (uint32_t)strtoul(argv[1], NULL, 0) : (256u * 1024u);

    io_connect_t conn = open_driver();
    if (!conn) return 1;

    /* --- 1. Что за карта и сколько памяти нам дали --- */
    MilcorixGpuInfo info;
    memset(&info, 0, sizeof(info));   /* иначе при коротком ответе читаем мусор */
    size_t infoSize = sizeof(info);
    kern_return_t kr = IOConnectCallStructMethod(conn, kMilcorixMethodGetInfo,
                                                 NULL, 0, &info, &infoSize);
    if (kr != KERN_SUCCESS) { fprintf(stderr, "GetInfo: 0x%x\n", kr); return 1; }
    if (infoSize < sizeof(info)) {
        fprintf(stderr, "GetInfo вернул %zu байт вместо %zu — версии драйвера и "
                        "утилиты разошлись\n", infoSize, sizeof(info));
        return 1;
    }

    printf("GPU-контекст: %s, канал=0x%08x CE=0x%08x, память под данные %llu КиБ\n",
           info.ready ? "готов" : "НЕ готов", info.channel, info.copy_engine,
           (unsigned long long)(info.scratch_size >> 10));
    if (!info.ready) {
        fprintf(stderr, "канал не поднят — смотри журнал драйвера\n");
        return 1;
    }

    /* Два блока внутри доступной области: источник и приёмник. */
    if ((uint64_t)bytes * 2ull > info.scratch_size) {
        bytes = (uint32_t)(info.scratch_size / 2ull) & ~0xFFFu;
        printf("размер урезан до %u КиБ по объёму доступной памяти\n", bytes >> 10);
    }
    uint64_t srcOff = 0, dstOff = bytes;

    /* --- 2. Узор, который невозможно спутать с мусором --- */
    uint8_t *src = malloc(bytes), *back = malloc(bytes);
    if (!src || !back) { fprintf(stderr, "нет памяти\n"); return 1; }
    for (uint32_t i = 0; i < bytes; i += 4) {
        uint32_t v = i ^ 0x4D494C43u;
        memcpy(src + i, &v, (bytes - i >= 4) ? 4 : (bytes - i));
    }

    uint64_t scalarIn[3];
    scalarIn[0] = srcOff;
    kr = IOConnectCallMethod(conn, kMilcorixMethodWrite, scalarIn, 1,
                             src, bytes, NULL, NULL, NULL, NULL);
    if (kr != KERN_SUCCESS) { fprintf(stderr, "Write: 0x%x\n", kr); return 1; }

    /* Приёмник обнуляем, чтобы совпадение нельзя было получить случайно. */
    memset(back, 0, bytes);
    scalarIn[0] = dstOff;
    kr = IOConnectCallMethod(conn, kMilcorixMethodWrite, scalarIn, 1,
                             back, bytes, NULL, NULL, NULL, NULL);
    if (kr != KERN_SUCCESS) { fprintf(stderr, "Write(dst): 0x%x\n", kr); return 1; }

    /* --- 3. Работу выполняет видеокарта --- */
    uint64_t scalarOut[1] = { 0 };
    uint32_t outCnt = 1;
    scalarIn[0] = srcOff; scalarIn[1] = dstOff; scalarIn[2] = bytes;
    kr = IOConnectCallScalarMethod(conn, kMilcorixMethodCopy, scalarIn, 3,
                                   scalarOut, &outCnt);
    if (kr != KERN_SUCCESS) { fprintf(stderr, "Copy: 0x%x\n", kr); return 1; }

    /* --- 4. Забираем результат и сверяем --- */
    size_t backSize = bytes;
    scalarIn[0] = dstOff;
    kr = IOConnectCallMethod(conn, kMilcorixMethodRead, scalarIn, 1,
                             NULL, 0, NULL, NULL, back, &backSize);
    if (kr != KERN_SUCCESS) { fprintf(stderr, "Read: 0x%x\n", kr); return 1; }

    if (memcmp(src, back, bytes) != 0) {
        uint32_t i = 0;
        while (i < bytes && src[i] == back[i]) i++;
        fprintf(stderr, "РАСХОЖДЕНИЕ с байта %u: ожидали 0x%02x, получили 0x%02x\n",
                i, src[i], back[i]);
        return 1;
    }

    double us = (double)scalarOut[0] / 1000.0;
    printf("\n*** ВИДЕОКАРТА ВЫПОЛНИЛА КОМАНДУ: %u КиБ скопировано движком CE, "
           "сверено побайтно ***\n", bytes >> 10);
    printf("    время выполнения на GPU: %.1f мкс", us);
    if (us > 0.0) printf("  (%.2f ГБ/с)", (double)bytes / (us * 1000.0));
    printf("\n");

    IOServiceClose(conn);
    free(src); free(back);
    return 0;
}
