/*
 * vbios_dump.c — слой 2: чтение VBIOS и извлечение дескриптора FWSEC.
 *
 * Зачем: bring-up GSP начинается с FWSEC из VBIOS (docs/gsp-bringup-notes.md).
 * Утилита читает VBIOS, проходит образы PCI ROM и по алгоритму nova-core находит
 * Falcon-ucode FWSEC: PciAt-образ -> BIT (token 0x70) -> указатель falcon data ->
 * PmuLookupTable (app id 0x85, FWSEC PROD) -> FalconUCodeDescV3.
 *
 * Все смещения/структуры взяты из nova-core (ядро Linux), НЕ выдуманы:
 *   drivers/gpu/nova-core/vbios.rs       — образы, PCIR/NPDE, BIT, PmuLookupTable
 *   drivers/gpu/nova-core/firmware.rs    — FalconUCodeDescV2/V3
 * Содержимое источников пересказано в код для соответствия лицензии.
 *
 * Сборка: cc tools/vbios_dump.c -o vbios_dump
 * Запуск:
 *   vbios_dump <rom_dump_file> [--extract fwsec_ucode.bin]
 *   vbios_dump --pci 0000:01:00.0 [--extract ...]   (Linux sysfs)
 *
 * Снять дамп VBIOS:
 *   Linux: echo 1 >/sys/.../rom; cat .../rom > vbios.rom; echo 0 >/sys/.../rom
 *   Windows: GPU-Z -> иконка чипа -> Save to file (.rom)
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define PCI_VENDOR_NVIDIA 0x10DEu
#define ROM_SIG           0xAA55u

#define CODE_TYPE_PCIAT   0x00u
#define CODE_TYPE_EFI     0x03u
#define CODE_TYPE_NBSI    0x70u
#define CODE_TYPE_FWSEC   0xE0u

#define BIT_TOKEN_FALCON_DATA 0x70u
#define PMU_APPID_FWSEC_PROD  0x85u

/* --- маленькие безопасные читатели little-endian --- */
static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static int in_bounds(size_t off, size_t need, size_t len) { return off <= len && need <= len - off; }

static const char *code_type_name(uint8_t t)
{
    switch (t) {
        case CODE_TYPE_PCIAT: return "x86 legacy (PciAt)";
        case CODE_TYPE_EFI:   return "EFI";
        case CODE_TYPE_NBSI:  return "NBSI";
        case CODE_TYPE_FWSEC: return "FWSEC/ucode";
        default:              return "прочее";
    }
}

/* Описание одного образа PCI ROM в дампе. */
struct image {
    size_t  off;        /* смещение начала образа в буфере */
    size_t  size;       /* размер образа в байтах */
    uint16_t vendor;
    uint16_t device;
    uint8_t code_type;
    int     last;
};

/*
 * Размер образа и флаг last с предпочтением NPDE (как в nova-core BiosImage):
 *   NPDE лежит сразу после PCIR, выровнен на 16: (pcir_off + pcir_len + 0xF) & ~0xF.
 *   Если NPDE валиден — берём subimage_len и его last-флаг, иначе PCIR.
 */
static void image_size_and_last(const uint8_t *buf, size_t len, size_t img_off,
                                 size_t pcir_off, uint8_t code_type,
                                 size_t *out_size, int *out_last)
{
    uint16_t pcir_len   = le16(buf + pcir_off + 0x0A);
    uint16_t image_blk  = le16(buf + pcir_off + 0x10);
    uint8_t  pcir_last  = buf[pcir_off + 0x15];

    size_t size = (size_t)image_blk * 512;
    int    last = (pcir_last & 0x80) != 0;

    /* NBSI считается последним образом (nova-core). */
    if (code_type == CODE_TYPE_NBSI) last = 1;

    size_t npde_off = ((pcir_off + pcir_len) + 0x0F) & ~(size_t)0x0F;
    if (in_bounds(npde_off, 0x0B, len) && memcmp(buf + npde_off, "NPDE", 4) == 0) {
        uint16_t subimg = le16(buf + npde_off + 0x08);
        uint8_t  npde_last = buf[npde_off + 0x0A];
        if (subimg != 0) {
            size = (size_t)subimg * 512;
            if (code_type != CODE_TYPE_NBSI) last = (npde_last & 0x80) != 0;
        }
    }
    (void)img_off;
    *out_size = size;
    *out_last = last;
}

/* Пройти цепочку образов PCI ROM. Возвращает количество (<= max). */
static int walk_images(const uint8_t *buf, size_t len, struct image *imgs, int max)
{
    size_t off = 0;
    int n = 0;

    while (n < max && in_bounds(off, 0x1A, len)) {
        if (le16(buf + off) != ROM_SIG) break;

        uint16_t pcir_ptr = le16(buf + off + 0x18);
        size_t pcir = off + pcir_ptr;
        if (!in_bounds(pcir, 0x16, len) || memcmp(buf + pcir, "PCIR", 4) != 0) {
            /* NVIDIA расширения иногда используют "NPDS" как сигнатуру PCIR. */
            if (!(in_bounds(pcir, 0x16, len) && memcmp(buf + pcir, "NPDS", 4) == 0))
                break;
        }

        struct image *im = &imgs[n];
        im->off       = off;
        im->vendor    = le16(buf + pcir + 0x04);
        im->device    = le16(buf + pcir + 0x06);
        im->code_type = buf[pcir + 0x14];
        image_size_and_last(buf, len, off, pcir, im->code_type, &im->size, &im->last);

        printf("  образ #%d @0x%06zx: vendor=0x%04X device=0x%04X type=0x%02X (%s) "
               "size=0x%zx%s%s\n",
               n, off, im->vendor, im->device, im->code_type,
               code_type_name(im->code_type), im->size,
               im->vendor == PCI_VENDOR_NVIDIA ? "  [NVIDIA]" : "",
               im->last ? "  [LAST]" : "");

        n++;
        if (im->size == 0 || im->last) break;
        off += im->size;
        off = (off + 511) & ~(size_t)511; /* выравнивание на 512 */
    }
    return n;
}

/*
 * Найти сигнатуру BIT в данных PciAt-образа: FF B8 'B' 'I' 'T' 00.
 * Возвращает смещение сигнатуры внутри образа или (size_t)-1.
 */
static size_t find_bit(const uint8_t *img, size_t img_len)
{
    static const uint8_t sig[6] = { 0xFF, 0xB8, 'B', 'I', 'T', 0x00 };
    if (img_len < sizeof(sig)) return (size_t)-1;
    for (size_t i = 0; i + sizeof(sig) <= img_len; i++)
        if (memcmp(img + i, sig, sizeof(sig)) == 0) return i;
    return (size_t)-1;
}

/*
 * Полный анализ FWSEC по алгоритму nova-core.
 * buf/len — весь VBIOS. imgs/n — разобранные образы.
 * out_desc_abs — абсолютное смещение дескриптора Falcon ucode в буфере (для extract).
 */
static int analyze_fwsec(const uint8_t *buf, size_t len,
                         const struct image *imgs, int n,
                         size_t *out_desc_abs, size_t *out_ucode_abs, size_t *out_ucode_size)
{
    /* 1. PciAt-образ (первый type 0x00) и первый FWSEC-образ (type 0xE0). */
    const struct image *pci_at = NULL;
    const struct image *fwsec  = NULL;
    for (int i = 0; i < n; i++) {
        if (!pci_at && imgs[i].code_type == CODE_TYPE_PCIAT) pci_at = &imgs[i];
        if (!fwsec  && imgs[i].code_type == CODE_TYPE_FWSEC) fwsec  = &imgs[i];
    }
    if (!pci_at) { printf("  [нет PciAt-образа]\n"); return -1; }
    if (!fwsec)  { printf("  [нет FWSEC-образа (0xE0) — урезанный дамп?]\n"); return -1; }

    size_t pci_at_abs = pci_at->off, pci_at_size = pci_at->size;
    size_t fwsec_abs  = fwsec->off;
    /* FWSEC-секция (nova-core): от первого FWSEC до конца. */
    size_t fwsec_len  = len - fwsec_abs;

    /* 2. BIT в PciAt-образе. */
    if (!in_bounds(pci_at_abs, pci_at_size, len)) { printf("  [PciAt вне границ]\n"); return -1; }
    const uint8_t *pci_at_data = buf + pci_at_abs;
    size_t bit_off = find_bit(pci_at_data, pci_at_size);
    if (bit_off == (size_t)-1) { printf("  [BIT не найден в PciAt]\n"); return -1; }

    uint8_t bit_header_size  = pci_at_data[bit_off + 8];
    uint8_t bit_token_size   = pci_at_data[bit_off + 9];
    uint8_t bit_token_count  = pci_at_data[bit_off + 10];
    printf("  BIT @0x%zx: header_size=%u token_size=%u tokens=%u\n",
           bit_off, bit_header_size, bit_token_size, bit_token_count);

    /* 3. Токен FALCON_DATA (0x70). */
    size_t tokens_start = bit_off + bit_header_size;
    uint16_t falcon_tok_off = 0;
    int found_tok = 0;
    for (unsigned i = 0; i < bit_token_count; i++) {
        size_t e = tokens_start + (size_t)i * bit_token_size;
        if (!in_bounds(e, 6, pci_at_size)) break;
        if (pci_at_data[e] == BIT_TOKEN_FALCON_DATA) {
            falcon_tok_off = le16(pci_at_data + e + 4); /* data_offset */
            found_tok = 1;
            break;
        }
    }
    if (!found_tok) { printf("  [токен FALCON_DATA (0x70) не найден]\n"); return -1; }

    /* 4. Указатель falcon data -> смещение относительно FWSEC-секции. */
    if (!in_bounds(falcon_tok_off, 4, pci_at_size)) { printf("  [falcon ptr вне PciAt]\n"); return -1; }
    uint32_t falcon_ptr = le32(pci_at_data + falcon_tok_off);
    if (falcon_ptr < pci_at_size) { printf("  [falcon ptr < PciAt size]\n"); return -1; }
    size_t falcon_data_off = falcon_ptr - pci_at_size; /* в FWSEC-секции */

    /* 5. PmuLookupTable -> запись app id 0x85. */
    size_t plt_abs = fwsec_abs + falcon_data_off;
    if (!in_bounds(plt_abs, 4, len)) { printf("  [PmuLookupTable вне границ]\n"); return -1; }
    uint8_t plt_hdr_len   = buf[plt_abs + 1];
    uint8_t plt_entry_len = buf[plt_abs + 2];
    uint8_t plt_entries   = buf[plt_abs + 3];
    printf("  PmuLookupTable @fwsec+0x%zx: hdr_len=%u entry_len=%u entries=%u\n",
           falcon_data_off, plt_hdr_len, plt_entry_len, plt_entries);

    uint32_t ucode_ptr = 0;
    int found_app = 0;
    for (unsigned i = 0; i < plt_entries; i++) {
        size_t e = plt_abs + plt_hdr_len + (size_t)i * plt_entry_len;
        if (!in_bounds(e, 6, len)) break;
        if (buf[e] == PMU_APPID_FWSEC_PROD) {
            ucode_ptr = le32(buf + e + 2); /* поле data (packed) */
            found_app = 1;
            break;
        }
    }
    if (!found_app) { printf("  [запись FWSEC PROD (app 0x85) не найдена]\n"); return -1; }
    if (ucode_ptr < pci_at_size) { printf("  [ucode ptr < PciAt size]\n"); return -1; }
    size_t falcon_ucode_off = ucode_ptr - pci_at_size; /* в FWSEC-секции */

    /* 6. Дескриптор Falcon ucode. Версия — байт +1 (nova-core header()). */
    size_t desc_abs = fwsec_abs + falcon_ucode_off;
    if (!in_bounds(desc_abs, 4, len)) { printf("  [дескриптор вне границ]\n"); return -1; }
    uint8_t ver = buf[desc_abs + 1];
    uint32_t hdr = le32(buf + desc_abs);
    uint32_t hdr_size = (hdr >> 16) & 0xFFFF; /* desc.size() */

    printf("  FWSEC Falcon ucode desc @fwsec+0x%zx (abs 0x%zx): version=%u hdr_size=%u\n",
           falcon_ucode_off, desc_abs, ver, hdr_size);

    uint32_t imem_load = 0, dmem_load = 0, sig_versions = 0;
    uint8_t  sig_count = 0, ucode_id = 0;
    uint16_t engine_mask = 0;
    size_t desc_struct_size = 0;

    if (ver == 3) {
        /* FalconUCodeDescV3 (firmware.rs), 44 байта. */
        if (!in_bounds(desc_abs, 44, len)) { printf("  [V3 вне границ]\n"); return -1; }
        imem_load    = le32(buf + desc_abs + 20);
        dmem_load    = le32(buf + desc_abs + 32);
        engine_mask  = le16(buf + desc_abs + 36);
        ucode_id     = buf[desc_abs + 38];
        sig_count    = buf[desc_abs + 39];
        sig_versions = le16(buf + desc_abs + 40);
        desc_struct_size = 44;
    } else if (ver == 2) {
        /* FalconUCodeDescV2 (firmware.rs), 60 байт; подписей нет. */
        if (!in_bounds(desc_abs, 60, len)) { printf("  [V2 вне границ]\n"); return -1; }
        imem_load = le32(buf + desc_abs + 24);
        dmem_load = le32(buf + desc_abs + 48);
        desc_struct_size = 60;
    } else {
        printf("  [неизвестная версия дескриптора FWSEC: %u]\n", ver);
        return -1;
    }

    uint32_t ucode_size = imem_load + dmem_load;
    printf("  imem_load=0x%X dmem_load=0x%X ucode_size=0x%X\n", imem_load, dmem_load, ucode_size);
    if (ver == 3)
        printf("  engine_id_mask=0x%X ucode_id=%u signature_count=%u signature_versions=0x%X\n",
               engine_mask, ucode_id, sig_count, sig_versions);

    /* ucode идёт после заголовка дескриптора (hdr_size), длина imem+dmem. */
    size_t ucode_abs = desc_abs + hdr_size;
    /* подписи (V3): сразу после структуры дескриптора, по 384 байта. */
    size_t sigs_abs  = desc_abs + desc_struct_size;
    if (ver == 3 && sig_count)
        printf("  signatures @abs 0x%zx: %u x 384 байт\n", sigs_abs, sig_count);

    *out_desc_abs   = desc_abs;
    *out_ucode_abs  = ucode_abs;
    *out_ucode_size = ucode_size;

    printf("  ==> FWSEC ucode @abs 0x%zx, size 0x%X. Готов к загрузке на Falcon GSP (слой2 шаг3).\n",
           ucode_abs, ucode_size);
    return 0;
}

/* --- ввод/вывод --- */
static uint8_t *read_file(const char *path, size_t *out_len)
{
    FILE *f = fopen(path, "rb");
    if (!f) { perror("fopen"); return NULL; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0) { fclose(f); return NULL; }
    uint8_t *buf = malloc((size_t)sz);
    if (!buf) { fclose(f); return NULL; }
    size_t got = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    *out_len = got;
    return buf;
}

#ifdef __linux__
static uint8_t *read_sysfs_rom(const char *bdf, size_t *out_len)
{
    char path[256];
    snprintf(path, sizeof(path), "/sys/bus/pci/devices/%s/rom", bdf);
    FILE *en = fopen(path, "w");
    if (en) { fputc('1', en); fclose(en); }
    uint8_t *buf = read_file(path, out_len);
    FILE *dis = fopen(path, "w");
    if (dis) { fputc('0', dis); fclose(dis); }
    return buf;
}
#endif

int main(int argc, char **argv)
{
    printf("=== RTXmacOC :: VBIOS -> FWSEC (слой 2) ===\n");

    const char *src = NULL;
    const char *extract = NULL;
    int use_pci = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--pci") == 0 && i + 1 < argc) { use_pci = 1; src = argv[++i]; }
        else if (strcmp(argv[i], "--extract") == 0 && i + 1 < argc) { extract = argv[++i]; }
        else src = argv[i];
    }
    if (!src) {
        fprintf(stderr, "usage: %s <rom_file> [--extract out.bin]\n"
                        "       %s --pci 0000:01:00.0 [--extract out.bin]\n",
                argv[0], argv[0]);
        return 1;
    }

    uint8_t *buf = NULL;
    size_t len = 0;
#ifdef __linux__
    if (use_pci) buf = read_sysfs_rom(src, &len); else buf = read_file(src, &len);
#else
    if (use_pci) { fprintf(stderr, "--pci поддерживается только на Linux\n"); return 1; }
    buf = read_file(src, &len);
#endif
    if (!buf) { fprintf(stderr, "не удалось прочитать VBIOS\n"); return 2; }

    printf("Прочитано %zu байт VBIOS.\n--- Образы PCI ROM ---\n", len);

    struct image imgs[32];
    int n = walk_images(buf, len, imgs, 32);
    if (n == 0) { printf("Образы не распознаны (не PCI ROM / урезан).\n"); free(buf); return 3; }

    printf("--- Анализ FWSEC ---\n");
    size_t desc_abs = 0, ucode_abs = 0, ucode_size = 0;
    int rc = analyze_fwsec(buf, len, imgs, n, &desc_abs, &ucode_abs, &ucode_size);

    if (rc == 0 && extract) {
        if (in_bounds(ucode_abs, ucode_size, len) && ucode_size > 0) {
            FILE *o = fopen(extract, "wb");
            if (o) {
                fwrite(buf + ucode_abs, 1, ucode_size, o);
                fclose(o);
                printf("Извлечён FWSEC ucode -> %s (0x%zx байт)\n", extract, ucode_size);
            } else perror("fopen extract");
        } else {
            printf("[extract пропущен: ucode вне границ дампа]\n");
        }
    }

    free(buf);
    printf("Готово.%s\n", rc == 0 ? " FWSEC локализован." : " FWSEC не извлечён (см. сообщения).");
    return rc == 0 ? 0 : 4;
}
