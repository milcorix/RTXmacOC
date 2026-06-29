/*
 * fwsec_locate.c — реализация поиска дескриптора FWSEC (см. fwsec_locate.h).
 * Портировано из nova-core vbios.rs (пересказано для лицензии). Смещения —
 * docs/PORTING-MAP.md.
 */
#include "fwsec_locate.h"
#include <string.h>

/*
 * Сигнатура начала ROM-образа. nova vbios.rs (PciRomHeader): допускаются ДВЕ —
 *   0xAA55 — стандартный PCI expansion ROM (образы PciAt/EFI);
 *   0x4E56 — "NV", NVIDIA-расширенные образы (NBSI/FWSEC) — у них тот же layout
 *            (pci_data_ptr@0x18 → PCIR/NPDS, code_type@0x14), но иной магик.
 * Подтверждено дампом RTX 4070S: FWSEC-образ (code_type 0xE0) начинается с 0x4E56.
 * (docs/hw-dumps/20260629-rtx4070s-prom-shadow.bin).
 */
#define ROM_SIG_PCI          0xAA55u
#define ROM_SIG_NV           0x4E56u
#define CODE_TYPE_PCIAT      0x00u
#define CODE_TYPE_NBSI       0x70u
#define CODE_TYPE_FWSEC      0xE0u
#define BIT_TOKEN_FALCON_DATA 0x70u
#define PMU_APPID_FWSEC_PROD  0x85u

static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static int fits(size_t off, size_t need, size_t len) { return off <= len && need <= len - off; }

struct image { size_t off, size; uint8_t code_type; int last; };

/* Размер образа и last-флаг с предпочтением NPDE (vbios.rs BiosImage). */
static void image_size_and_last(const uint8_t *buf, size_t len, size_t pcir_off,
                                uint8_t code_type, size_t *out_size, int *out_last)
{
    uint16_t pcir_len  = le16(buf + pcir_off + 0x0A);
    uint16_t image_blk = le16(buf + pcir_off + 0x10);
    uint8_t  pcir_last = buf[pcir_off + 0x15];
    size_t size = (size_t)image_blk * 512;
    int last = (pcir_last & 0x80) != 0;
    if (code_type == CODE_TYPE_NBSI) last = 1;

    size_t npde_off = ((pcir_off + pcir_len) + 0x0F) & ~(size_t)0x0F;
    if (fits(npde_off, 0x0B, len) && memcmp(buf + npde_off, "NPDE", 4) == 0) {
        uint16_t subimg = le16(buf + npde_off + 0x08);
        uint8_t  npde_last = buf[npde_off + 0x0A];
        if (subimg != 0) {
            size = (size_t)subimg * 512;
            if (code_type != CODE_TYPE_NBSI) last = (npde_last & 0x80) != 0;
        }
    }
    *out_size = size;
    *out_last = last;
}

static int walk_images(const uint8_t *buf, size_t len, struct image *imgs, int max)
{
    size_t off = 0;
    int n = 0;
    while (n < max && fits(off, 0x1A, len)) {
        uint16_t sig = le16(buf + off);
        if (sig != ROM_SIG_PCI && sig != ROM_SIG_NV) break;
        uint16_t pcir_ptr = le16(buf + off + 0x18);
        size_t pcir = off + pcir_ptr;
        if (!fits(pcir, 0x16, len)) break;
        if (memcmp(buf + pcir, "PCIR", 4) != 0 && memcmp(buf + pcir, "NPDS", 4) != 0) break;

        struct image *im = &imgs[n];
        im->off = off;
        im->code_type = buf[pcir + 0x14];
        image_size_and_last(buf, len, pcir, im->code_type, &im->size, &im->last);
        n++;
        if (im->size == 0 || im->last) break;
        off += im->size;
        off = (off + 511) & ~(size_t)511;
    }
    return n;
}

static size_t find_bit(const uint8_t *img, size_t img_len)
{
    static const uint8_t sig[6] = { 0xFF, 0xB8, 'B', 'I', 'T', 0x00 };
    if (img_len < sizeof(sig)) return (size_t)-1;
    for (size_t i = 0; i + sizeof(sig) <= img_len; i++)
        if (memcmp(img + i, sig, sizeof(sig)) == 0) return i;
    return (size_t)-1;
}

int nv_fwsec_locate(const uint8_t *vbios, size_t len, nv_fwsec_location_t *out)
{
    if (!vbios || !out) return NV_FWSEC_LOC_ERR_BOUNDS;

    /*
     * TODO: verify на стенде. nova vbios.rs перед PCI-образами умеет пропускать
     * IFR-заголовок (сигнатура "NVGI" по BAR0+0x300000) — это нужно в основном на
     * GA100. На Ada/AD104 IFR-смещение обычно уже применено к PROM-теневой копии,
     * поэтому скан начинается с 0 и первый word = ROM_SIG(0xAA55). Если дамп ROM на
     * железе начнётся с "NVGI" — здесь потребуется разбор IFR (rom_offset).
     */
    struct image imgs[32];
    int n = walk_images(vbios, len, imgs, 32);
    if (n == 0) return NV_FWSEC_LOC_ERR_NOSIG;

    const struct image *pci_at = NULL, *fwsec = NULL;
    for (int i = 0; i < n; i++) {
        if (!pci_at && imgs[i].code_type == CODE_TYPE_PCIAT) pci_at = &imgs[i];
        if (!fwsec  && imgs[i].code_type == CODE_TYPE_FWSEC) fwsec  = &imgs[i];
    }
    if (!pci_at || !fwsec) return NV_FWSEC_LOC_ERR_NOIMG;

    size_t pci_at_abs = pci_at->off, pci_at_size = pci_at->size, fwsec_abs = fwsec->off;
    if (!fits(pci_at_abs, pci_at_size, len)) return NV_FWSEC_LOC_ERR_BOUNDS;

    const uint8_t *pci_at_data = vbios + pci_at_abs;
    size_t bit_off = find_bit(pci_at_data, pci_at_size);
    if (bit_off == (size_t)-1) return NV_FWSEC_LOC_ERR_NOSIG;

    uint8_t bit_header_size = pci_at_data[bit_off + 8];
    uint8_t bit_token_size  = pci_at_data[bit_off + 9];
    uint8_t bit_token_count = pci_at_data[bit_off + 10];

    size_t tokens_start = bit_off + bit_header_size;
    uint16_t falcon_tok_off = 0;
    int found = 0;
    for (unsigned i = 0; i < bit_token_count; i++) {
        size_t e = tokens_start + (size_t)i * bit_token_size;
        if (!fits(e, 6, pci_at_size)) break;
        if (pci_at_data[e] == BIT_TOKEN_FALCON_DATA) { falcon_tok_off = le16(pci_at_data + e + 4); found = 1; break; }
    }
    if (!found) return NV_FWSEC_LOC_ERR_PARSE;
    if (!fits(falcon_tok_off, 4, pci_at_size)) return NV_FWSEC_LOC_ERR_BOUNDS;

    uint32_t falcon_ptr = le32(pci_at_data + falcon_tok_off);
    if (falcon_ptr < pci_at_size) return NV_FWSEC_LOC_ERR_BOUNDS;
    size_t falcon_data_off = falcon_ptr - pci_at_size;

    size_t plt_abs = fwsec_abs + falcon_data_off;
    if (!fits(plt_abs, 4, len)) return NV_FWSEC_LOC_ERR_BOUNDS;
    uint8_t plt_hdr_len   = vbios[plt_abs + 1];
    uint8_t plt_entry_len = vbios[plt_abs + 2];
    uint8_t plt_entries   = vbios[plt_abs + 3];

    uint32_t ucode_ptr = 0;
    found = 0;
    for (unsigned i = 0; i < plt_entries; i++) {
        size_t e = plt_abs + plt_hdr_len + (size_t)i * plt_entry_len;
        if (!fits(e, 6, len)) break;
        if (vbios[e] == PMU_APPID_FWSEC_PROD) { ucode_ptr = le32(vbios + e + 2); found = 1; break; }
    }
    if (!found) return NV_FWSEC_LOC_ERR_PARSE;
    if (ucode_ptr < pci_at_size) return NV_FWSEC_LOC_ERR_BOUNDS;

    size_t desc_abs = fwsec_abs + (ucode_ptr - pci_at_size);
    if (!fits(desc_abs, 4, len)) return NV_FWSEC_LOC_ERR_BOUNDS;

    out->desc_abs = desc_abs;
    return NV_FWSEC_LOC_OK;
}
