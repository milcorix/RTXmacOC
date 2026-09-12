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
#include <errno.h>
#include <IOKit/IOKitLib.h>
#include "../driver/macos/MilcorixABI.h"

_Static_assert(sizeof(MilcorixGpuInfo) == 24, "legacy ABI");
_Static_assert(sizeof(MilcorixMemoryInfo) == 40, "memory ABI");

static kern_return_t buffer_read(io_connect_t conn, uint64_t handle, uint64_t offset,
                                  void *data, size_t bytes)
{
    uint64_t args[2] = {handle, offset};
    size_t actual = bytes;
    kern_return_t kr = IOConnectCallMethod(conn, kMilcorixMethodReadBuffer, args, 2,
                                          NULL, 0, NULL, NULL, data, &actual);
    return kr == KERN_SUCCESS && actual != bytes ? KERN_FAILURE : kr;
}

static int check_vram(io_connect_t conn)
{
    const uint64_t gib = 1ull << 30;
    const size_t bytes = 65536;
    MilcorixMemoryInfo info = {0};
    size_t size = sizeof(info);
    kern_return_t kr = IOConnectCallStructMethod(conn, kMilcorixMethodGetMemoryInfo,
                                                 NULL, 0, &info, &size);
    if (kr || size != sizeof(info) || info.size != sizeof(info) ||
        info.version != MILCORIX_MEMORY_ABI_VERSION || !(info.flags & MILCORIX_MEMORY_READY) ||
        info.pool_bytes < gib) {
        fprintf(stderr, "Ресурс 1 ГиБ не готов: GetMemoryInfo=0x%x flags=0x%x bytes=%llu.\n"
                        "Нужен новый kext и milcorix=2 milcorixvram=1024.\n", kr, info.flags,
                (unsigned long long)info.pool_bytes);
        return 1;
    }
    printf("macOS VRAM ABI=%u pool=%llu MiB flags=0x%x\n", info.version,
           (unsigned long long)(info.pool_bytes >> 20), info.flags);
    uint8_t *src = malloc(bytes), *back = malloc(bytes);
    uint64_t handle = 0, allocated[2] = {0}, allocArgs[2] = {gib, 65536};
    uint32_t count = 2;
    int result = 1;
    if (!src || !back) goto done;
    kr = IOConnectCallScalarMethod(conn, kMilcorixMethodAlloc, allocArgs, 2, allocated, &count);
    if (kr || count != 2 || !allocated[0] || allocated[1] != gib) {
        fprintf(stderr, "Alloc 1 GiB: 0x%x\n", kr); goto done;
    }
    handle = allocated[0];
    printf("macOS allocation: handle=%llu bytes=%llu\n",
           (unsigned long long)handle, (unsigned long long)allocated[1]);
    uint64_t destinations[] = {bytes, gib / 2 - bytes / 2, gib - bytes};
    for (unsigned probe = 0; probe < 3; probe++) {
        /* До записи ресурс должен быть очищен, включая дальние страницы. */
        memset(back, 0xa5, bytes);
        kr = buffer_read(conn, handle, destinations[probe], back, bytes);
        if (kr) { fprintf(stderr, "Read zero: 0x%x\n", kr); goto done; }
        for (size_t i = 0; i < bytes; i++) if (back[i]) {
            fprintf(stderr, "Новый ресурс содержит ненулевые данные @%llu\n",
                    (unsigned long long)(destinations[probe] + i)); goto done;
        }
        for (size_t i = 0; i < bytes; i++) src[i] = (uint8_t)(i * 37 + (i >> 8) + probe + 1);
        uint64_t writeArgs[2] = {handle, 0};
        kr = IOConnectCallMethod(conn, kMilcorixMethodWriteBuffer, writeArgs, 2,
                                 src, bytes, NULL, NULL, NULL, NULL);
        if (kr) { fprintf(stderr, "WriteBuffer: 0x%x\n", kr); goto done; }
        uint64_t copyArgs[5] = {handle, 0, handle, destinations[probe], bytes}, ns = 0;
        count = 1;
        kr = IOConnectCallScalarMethod(conn, kMilcorixMethodCopyBuffer, copyArgs, 5, &ns, &count);
        if (kr || count != 1) { fprintf(stderr, "CopyBuffer: 0x%x\n", kr); goto done; }
        memset(back, 0, bytes);
        kr = buffer_read(conn, handle, destinations[probe], back, bytes);
        if (kr || memcmp(src, back, bytes)) {
            fprintf(stderr, "Проверка результата не прошла: Read=0x%x dst=%llu\n",
                    kr, (unsigned long long)destinations[probe]); goto done;
        }
        printf("macOS GPU copy: dst=%llu bytes=%zu MATCH; host fence wait=%llu ns\n",
               (unsigned long long)destinations[probe], bytes, (unsigned long long)ns);
    }
    if (buffer_read(conn, handle, gib, back, 4) != kIOReturnBadArgument) {
        fprintf(stderr, "Доступ за границей ресурса не отвергнут\n"); goto done;
    }
    kr = IOConnectCallScalarMethod(conn, kMilcorixMethodFree, &handle, 1, NULL, NULL);
    if (kr) { fprintf(stderr, "Free: 0x%x\n", kr); goto done; }
    if (buffer_read(conn, handle, 0, back, 4) != kIOReturnBadArgument) {
        fprintf(stderr, "Освобождённый handle не отвергнут\n"); handle = 0; goto done;
    }
    handle = 0;
    printf("macOS resource test PASS: 1 GiB allocation, 3 GPU copy probes, bounds and free.\n");
    result = 0;
done:
    if (handle) IOConnectCallScalarMethod(conn, kMilcorixMethodFree, &handle, 1, NULL, NULL);
    free(src); free(back);
    return result;
}

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
    if (argc == 2 && !strcmp(argv[1], "--vram-test")) {
        io_connect_t conn = open_driver();
        if (!conn) return 1;
        int result = check_vram(conn);
        IOServiceClose(conn);
        return result;
    }
    uint32_t bytes = 256u * 1024u;
    if (argc > 1) {
        char *end = NULL; errno = 0;
        unsigned long value = strtoul(argv[1], &end, 0);
        if (argc != 2 || errno || !end || *end || !value || value > MILCORIX_MAX_XFER) {
            fprintf(stderr, "usage: milcorix_gpu [1..4194304 bytes | --vram-test]\n"); return 2;
        }
        bytes = (uint32_t)value;
    }

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
