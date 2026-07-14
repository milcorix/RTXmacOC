# RTXmacOC — Makefile
#
# Только macOS. Зависит лишь от системных фреймворков (IOKit, CoreFoundation).
#
#   make probe   — собрать утилиту разведки pcie_probe
#   make run     — собрать и запустить
#   make dump    — запустить и сохранить лог в docs/hw-dumps/ с датой
#   make clean   — удалить артефакты

CC      ?= clang
CFLAGS  ?= -Wall -Wextra -O2
FRAMEWORKS = -framework IOKit -framework CoreFoundation

PROBE_SRC = pcie_probe.c
PROBE_BIN = pcie_probe

DUMP_DIR  = docs/hw-dumps
DATE     := $(shell date +%Y%m%d)

.PHONY: probe run dump clean mmio-linux vbios-dump booter-parse-test booter-run-linux gsp-stage-test gsp-boot-linux gsp-rpc-test gsp-rm-test gmmu-test gsp-fifo-test gsp-disp-test

probe: $(PROBE_BIN)

$(PROBE_BIN): $(PROBE_SRC) ada_regs.h
	$(CC) $(CFLAGS) $(PROBE_SRC) $(FRAMEWORKS) -o $(PROBE_BIN)

run: probe
	./$(PROBE_BIN)

dump: probe
	@mkdir -p $(DUMP_DIR)
	./$(PROBE_BIN) | tee "$(DUMP_DIR)/$(DATE)-rtx4070s-pcie_probe.log"

clean:
	rm -f $(PROBE_BIN) tools/nv_mmio_linux tools/vbios_dump tools/booter_parse_test

# Офлайн-проверка разбора контейнера Booter (слой 2, задача 6, фазы 1-2). Linux, без GPU.
#   make booter-parse-test && ./tools/booter_parse_test
booter-parse-test:
	cc -Wall -Wextra -O2 tools/booter_parse_test.c tools/fw_blob_linux.c \
	   driver/gsp/booter.c -o tools/booter_parse_test

# Пробный dry-load Booter на SEC2 через VFIO (слой 2, задача 6, фаза 3). Linux+root.
#   make booter-run-linux && sudo ./tools/booter_run_linux
booter-run-linux:
	cc -Wall -Wextra -O2 tools/booter_run_linux.c tools/fw_blob_linux.c \
	   driver/gsp/falcon.c driver/gsp/booter.c -o tools/booter_run_linux

# Офлайн-проверка подготовки GSP-RM: ELF-секции + bootloader-desc + radix3 (фаза 4). Без GPU.
#   make gsp-stage-test && ./tools/gsp_stage_test
gsp-stage-test:
	cc -Wall -Wextra -O2 tools/gsp_stage_test.c tools/fw_blob_linux.c \
	   driver/gsp/gsp_fw.c driver/gsp/elf64.c -o tools/gsp_stage_test

# Оркестратор загрузки GSP-RM на железе (слой 2, задача 6, фаза 6). Linux+root.
#   make gsp-boot-linux && sudo ./tools/gsp_boot_linux   (прогон: tools/run-gsp-boot-detached.sh)
gsp-boot-linux:
	cc -Wall -Wextra -O2 tools/gsp_boot_linux.c tools/fw_blob_linux.c \
	   driver/gsp/falcon.c driver/gsp/fwsec_locate.c driver/gsp/fwsec_patch.c \
	   driver/gsp/fb_layout.c driver/gsp/booter.c driver/gsp/gsp_fw.c driver/gsp/elf64.c \
	   driver/gsp/gsp_rpc.c driver/gsp/gsp_rm.c driver/gsp/gmmu.c driver/gsp/gsp_fifo.c \
	   driver/gsp/gsp_disp.c \
	   -o tools/gsp_boot_linux

# Офлайн-проверка раскладки очередей GSP-RM (слой 2, задача 7). Без GPU.
gsp-rpc-test:
	cc -Wall -Wextra -O2 tools/gsp_rpc_test.c driver/gsp/gsp_rpc.c -o tools/gsp_rpc_test

# Офлайн-тест RPC слоя 3 (RM-цепочка, static_info, VRAM memlist/map). Без GPU.
#   make gsp-rm-test && ./tools/gsp_rm_test
gsp-rm-test:
	cc -Wall -Wextra -O2 tools/gsp_rm_test.c driver/gsp/gsp_rm.c driver/gsp/gsp_rpc.c -o tools/gsp_rm_test

# Офлайн-тест прямого GMMU (проход D): PRAMIN-окно, кодирование PTE/PDE, построение
# таблиц с read-back. Без GPU.
#   make gmmu-test && ./tools/gmmu_test
gmmu-test:
	cc -Wall -Wextra -O2 tools/gmmu_test.c driver/gsp/gmmu.c -o tools/gmmu_test

# Офлайн-тест слоя 4 (каналы GPFIFO): compile-probe NV_CHANNEL_ALLOC_PARAMS +
# framing channel alloc/bind/schedule. Без GPU.
#   make gsp-fifo-test && ./tools/gsp_fifo_test
gsp-fifo-test:
	cc -Wall -Wextra -O2 tools/gsp_fifo_test.c driver/gsp/gsp_fifo.c \
	   driver/gsp/gsp_rm.c driver/gsp/gsp_rpc.c -o tools/gsp_fifo_test

# Офлайн-тест слоя 5 (дисплей): framing NV04_DISPLAY_COMMON alloc + GET_NUM_HEADS +
# GET_SUPPORTED. Без GPU.
#   make gsp-disp-test && ./tools/gsp_disp_test
gsp-disp-test:
	cc -Wall -Wextra -O2 tools/gsp_disp_test.c driver/gsp/gsp_disp.c \
	   driver/gsp/gsp_rm.c driver/gsp/gsp_rpc.c -o tools/gsp_disp_test

# Чтение/разбор VBIOS карты (слой 2, шаг 1). Портируемо, собирается любым cc.
#   make vbios-dump && ./tools/vbios_dump <rom_file>
vbios-dump:
	cc -Wall -Wextra -O2 tools/vbios_dump.c -o tools/vbios_dump

# Проверка PMC_BOOT_0 на реальной карте из Linux live-USB (без macOS, без Windows).
# Собирать и запускать ИМЕННО на Linux: make mmio-linux && sudo ./tools/nv_mmio_linux
mmio-linux:
	cc -Wall -Wextra -O2 tools/nv_mmio_linux.c -o tools/nv_mmio_linux
