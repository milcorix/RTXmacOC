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

.PHONY: probe run dump clean mmio-linux vbios-dump booter-parse-test booter-run-linux gsp-stage-test gsp-boot-linux gsp-rpc-test

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
	   -o tools/gsp_boot_linux

# Офлайн-проверка раскладки очередей GSP-RM (слой 2, задача 7). Без GPU.
gsp-rpc-test:
	cc -Wall -Wextra -O2 tools/gsp_rpc_test.c driver/gsp/gsp_rpc.c -o tools/gsp_rpc_test

# Чтение/разбор VBIOS карты (слой 2, шаг 1). Портируемо, собирается любым cc.
#   make vbios-dump && ./tools/vbios_dump <rom_file>
vbios-dump:
	cc -Wall -Wextra -O2 tools/vbios_dump.c -o tools/vbios_dump

# Проверка PMC_BOOT_0 на реальной карте из Linux live-USB (без macOS, без Windows).
# Собирать и запускать ИМЕННО на Linux: make mmio-linux && sudo ./tools/nv_mmio_linux
mmio-linux:
	cc -Wall -Wextra -O2 tools/nv_mmio_linux.c -o tools/nv_mmio_linux
