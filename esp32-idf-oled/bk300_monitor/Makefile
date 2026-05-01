# Makefile-обёртка над ESP-IDF для проекта bk300_oled_monitor (OLED-вариант).
# Цель — на чистой Ubuntu из-под одной команды получить рабочее окружение и собрать прошивку.
#
# Быстрый старт:
#   make setup              # один раз: ставит apt-зависимости + клонирует ESP-IDF + ставит тулчейны
#   make TARGET=esp32s3 build
#   make PORT=/dev/ttyACM0 flash monitor
#
# Все цели, где нужен IDF, неявно делают `source $(IDF_PATH)/export.sh` в bash-сабшелле.

SHELL := /bin/bash
.SHELLFLAGS := -eo pipefail -c

# ---- Параметры (можно переопределять из CLI) ----
# TARGET вычисляется ЛЕНИВО (`=`, не `:=`), чтобы shortcut'ы вроде `make esp32c3`,
# которые меняют sdkconfig в процессе выполнения, корректно влияли на `make package`
# в той же make-сессии. Приоритет:
#   1) CLI / env  (например `make TARGET=esp32 build` или `export TARGET=esp32c3`)
#   2) текущий sdkconfig (CONFIG_IDF_TARGET="...") — то, что реально настроил idf.py
#   3) фолбэк esp32s3, если sdkconfig ещё нет (свежий клон).
ifndef TARGET
TARGET        = $(or $(shell sed -n 's/^CONFIG_IDF_TARGET="\([^"]*\)"$$/\1/p' sdkconfig 2>/dev/null),esp32s3)
endif
PORT         ?= /dev/ttyUSB0
BAUD         ?= 460800
IDF_VERSION  ?= v5.3.2
IDF_PATH     ?= $(HOME)/esp/esp-idf
IDF_TOOLS_PATH ?= $(HOME)/.espressif

# ---- Параметры пакета (для `make package`) ----
PROJECT_NAME ?= bk300_oled_monitor
PKG_VERSION  ?= $(shell git -C $(CURDIR) describe --tags --always --dirty 2>/dev/null || date +%Y%m%d-%H%M%S)
# Тоже lazy (`=`), чтобы $(TARGET) и $(PKG_VERSION) перевычислялись в момент package,
# а не парсинга — иначе CHIP=esp32s3 «прилипает» к flash.env даже после смены target.
PKG_NAME      = $(PROJECT_NAME)-$(TARGET)-$(PKG_VERSION)
DIST_DIR     ?= dist
PKG_DIR        = $(DIST_DIR)/$(PKG_NAME)
PKG_ARCHIVE    = $(DIST_DIR)/$(PKG_NAME).zip

# Перечень чипов, под которые ставим тулчейны (через запятую). Если нужны все — `all`.
IDF_TARGETS_INSTALL ?= esp32,esp32s3,esp32c3

# Вспомогательная активация: bash-only, поэтому SHELL=/bin/bash выше.
ACTIVATE     := source $(IDF_PATH)/export.sh > /dev/null

# Apt-пакеты по официальной инструкции Espressif для ESP-IDF на Ubuntu/Debian.
APT_PKGS     := git wget flex bison gperf python3 python3-pip python3-venv \
                cmake ninja-build ccache libffi-dev libssl-dev dfu-util \
                libusb-1.0-0 udev

.DEFAULT_GOAL := help

# ---- Help ----
.PHONY: help
help:
	@echo "ESP-IDF make-обёртка для bk300_oled_monitor (OLED-вариант)."
	@echo ""
	@echo "Параметры (export или make VAR=value):"
	@echo "  TARGET=$(TARGET)              # esp32 / esp32s3 / esp32c3 / esp32c6 / esp32h2"
	@echo "  PORT=$(PORT)         # путь до COM-порта"
	@echo "  IDF_VERSION=$(IDF_VERSION)         # тег/ветка ESP-IDF"
	@echo "  IDF_PATH=$(IDF_PATH)"
	@echo "  IDF_TOOLS_PATH=$(IDF_TOOLS_PATH)"
	@echo "  IDF_TARGETS_INSTALL=$(IDF_TARGETS_INSTALL)"
	@echo ""
	@echo "Цели:"
	@echo "  setup           apt-deps + clone ESP-IDF + install.sh (одной командой)"
	@echo "  install-deps    только apt-зависимости (sudo)"
	@echo "  install-idf     только clone esp-idf и install.sh"
	@echo "  add-udev        правила udev для CP210x/CH34x/FTDI/USB JTAG (sudo)"
	@echo "  set-target          idf.py set-target \$$TARGET (default $(TARGET))"
	@echo "  esp32 / esp32s3 / esp32c3 / esp32c6 / esp32h2 / esp32s2"
	@echo "                      shortcut: то же что 'set-target TARGET=<chip>' + чистка sdkconfig"
	@echo "  reconfigure         idf.py reconfigure"
	@echo "  refresh-sdkconfig   удалить sdkconfig + set-target (после правки sdkconfig.defaults)"
	@echo "  build               idf.py build (incremental, sdkconfig.defaults НЕ перечитывает)"
	@echo "  rebuild             refresh-sdkconfig + build + package одной командой"
	@echo "  flash           idf.py -p \$$PORT flash"
	@echo "  monitor         idf.py -p \$$PORT monitor"
	@echo "  flash-monitor   flash + monitor одним вызовом"
	@echo "  erase-flash     idf.py -p \$$PORT erase-flash"
	@echo "  size            idf.py size  (-components / -files доступны вручную)"
	@echo "  menuconfig      idf.py menuconfig"
	@echo "  clean           idf.py clean"
	@echo "  fullclean       idf.py fullclean"
	@echo "  package         собрать ZIP с бинарями + flash.sh/.bat для прошивки с другого ПК"
	@echo "  package-clean   удалить $(DIST_DIR)/"
	@echo "  doctor          печатает версии и проверяет окружение"

# ---- Установка ----
.PHONY: install-deps
install-deps:
	@echo ">> apt install: $(APT_PKGS)"
	sudo apt-get update
	sudo apt-get install -y $(APT_PKGS)
	@if ! groups "$$USER" | grep -q dialout; then \
		echo ">> добавляю $$USER в группу dialout (нужно для доступа к /dev/ttyUSB*)"; \
		sudo usermod -aG dialout "$$USER"; \
		echo ">> ВАЖНО: перезайди в систему / перезапусти shell, чтобы группа dialout применилась"; \
	fi

.PHONY: install-idf
install-idf:
	@if [ ! -d "$(IDF_PATH)/.git" ]; then \
		echo ">> Клонирую ESP-IDF $(IDF_VERSION) в $(IDF_PATH)"; \
		mkdir -p "$$(dirname $(IDF_PATH))"; \
		git clone -b $(IDF_VERSION) --recurse-submodules https://github.com/espressif/esp-idf.git "$(IDF_PATH)"; \
	else \
		echo ">> $(IDF_PATH) уже существует — проверяю обновления подмодулей"; \
		cd "$(IDF_PATH)" && git fetch --tags && git checkout $(IDF_VERSION) && git submodule update --init --recursive; \
	fi
	@echo ">> install.sh для $(IDF_TARGETS_INSTALL)"
	cd "$(IDF_PATH)" && IDF_TOOLS_PATH="$(IDF_TOOLS_PATH)" ./install.sh "$(IDF_TARGETS_INSTALL)"

.PHONY: setup
setup: install-deps install-idf
	@echo ""
	@echo ">> ГОТОВО. Дальше:"
	@echo "   make set-target TARGET=$(TARGET)"
	@echo "   make build"
	@echo "   make PORT=$(PORT) flash monitor"

# udev-правила: чтобы /dev/ttyUSB* и /dev/ttyACM* открывались без sudo.
.PHONY: add-udev
add-udev:
	@echo ">> ставлю udev-правила Espressif (CP210x / CH34x / FTDI / ESP USB JTAG)"
	sudo install -m 0644 "$(IDF_PATH)/components/esptool_py/esptool/test/udev-rules/98-esp.rules" \
		/etc/udev/rules.d/98-esp.rules || true
	sudo udevadm control --reload-rules
	sudo udevadm trigger
	@echo ">> готово (если правил Espressif нет — поставь вручную с https://github.com/espressif/esptool/blob/master/test/udev-rules/98-esp.rules)"

# ---- IDF-команды ----
.PHONY: set-target
set-target:
	@$(ACTIVATE) && idf.py set-target $(TARGET)

# Удобные shortcut-цели, чтобы не писать каждый раз TARGET=...
# Использование: `make esp32c3` — то же что `make set-target TARGET=esp32c3`.
# Имя цели ($@) подставляется в idf.py set-target.
.PHONY: esp32 esp32s3 esp32c3 esp32c6 esp32h2 esp32s2
esp32 esp32s3 esp32c3 esp32c6 esp32h2 esp32s2:
	@echo ">> set-target $@ (rm sdkconfig + reconfigure under the hood)"
	rm -f sdkconfig sdkconfig.old
	rm -rf build
	@$(ACTIVATE) && idf.py set-target $@

.PHONY: reconfigure
reconfigure:
	@$(ACTIVATE) && idf.py reconfigure

# Полностью перечитать sdkconfig.defaults: старый sdkconfig затирает свежие defaults,
# поэтому удаляем его и просим reconfigure.
.PHONY: refresh-sdkconfig
refresh-sdkconfig:
	@echo ">> remove sdkconfig + reconfigure (defaults применяются заново)"
	rm -f sdkconfig sdkconfig.old
	rm -rf build
	@$(ACTIVATE) && idf.py set-target $(TARGET)

.PHONY: check-sdkconfig-fresh
check-sdkconfig-fresh:
	@if [ -f sdkconfig ] && [ sdkconfig.defaults -nt sdkconfig ]; then \
		echo ""; \
		echo "############################################################"; \
		echo "## ВНИМАНИЕ: sdkconfig.defaults свежее sdkconfig.         ##"; \
		echo "## Изменения в defaults НЕ попадут в текущий build!       ##"; \
		echo "## Запусти:  make refresh-sdkconfig && make build         ##"; \
		echo "## или одной командой: make rebuild                       ##"; \
		echo "############################################################"; \
		echo ""; \
	fi

.PHONY: build
build: check-sdkconfig-fresh
	@$(ACTIVATE) && idf.py build

# Полный цикл: defaults → sdkconfig → build → package. Ничего лишнего не чистим
# в исходниках, только sdkconfig + build/. Использовать когда правил sdkconfig.defaults.
.PHONY: rebuild
rebuild:
	@echo ">> rebuild: refresh-sdkconfig + build + package"
	$(MAKE) refresh-sdkconfig
	$(MAKE) build
	$(MAKE) package

.PHONY: flash
flash:
	@$(ACTIVATE) && idf.py -p $(PORT) -b $(BAUD) flash

.PHONY: monitor
monitor:
	@$(ACTIVATE) && idf.py -p $(PORT) monitor

.PHONY: flash-monitor
flash-monitor:
	@$(ACTIVATE) && idf.py -p $(PORT) -b $(BAUD) flash monitor

.PHONY: erase-flash
erase-flash:
	@$(ACTIVATE) && idf.py -p $(PORT) erase-flash

.PHONY: size
size:
	@$(ACTIVATE) && idf.py size

.PHONY: menuconfig
menuconfig:
	@$(ACTIVATE) && idf.py menuconfig

.PHONY: clean
clean:
	@$(ACTIVATE) && idf.py clean

.PHONY: fullclean
fullclean:
	@$(ACTIVATE) && idf.py fullclean

# ---- Упаковка прошивки для другого ПК ----
# Делаем zip-архив, в котором лежат:
#   * bootloader/bootloader.bin
#   * partition_table/partition-table.bin
#   * <project>.bin
#   * flash_args (esptool args-файл; читается через @flash_args)
#   * flasher_args.json (полные параметры от idf.py)
#   * flash.sh / flash.bat — обёртки над esptool (esptool ставится через pip)
#   * monitor.sh / monitor.bat — простой serial-монитор
#   * flash.env — CHIP=$(TARGET) и т.п.
#   * README.md — инструкция
# Ничего из IDF в архив не попадает — на целевой машине нужен только Python.
.PHONY: package
package:
	@echo ">> упаковка $(PKG_NAME)"
	@if [ ! -f build/flash_args ]; then \
		echo "ERROR: build/flash_args не найден — сначала собери: make build" >&2; \
		exit 1; \
	fi
	@if [ ! -f build/$(PROJECT_NAME).bin ]; then \
		echo "ERROR: build/$(PROJECT_NAME).bin не найден. Проверь PROJECT_NAME=$(PROJECT_NAME)" >&2; \
		exit 1; \
	fi
	@rm -rf "$(PKG_DIR)" "$(PKG_ARCHIVE)"
	@mkdir -p "$(PKG_DIR)/bootloader" "$(PKG_DIR)/partition_table"
	@cp build/flash_args                              "$(PKG_DIR)/flash_args"
	@cp build/flasher_args.json                       "$(PKG_DIR)/flasher_args.json" 2>/dev/null || true
	@cp build/$(PROJECT_NAME).bin                     "$(PKG_DIR)/$(PROJECT_NAME).bin"
	@cp build/bootloader/bootloader.bin               "$(PKG_DIR)/bootloader/bootloader.bin"
	@cp build/partition_table/partition-table.bin     "$(PKG_DIR)/partition_table/partition-table.bin"
	@cp scripts/flash.sh    "$(PKG_DIR)/flash.sh"
	@cp scripts/flash.bat   "$(PKG_DIR)/flash.bat"
	@cp scripts/monitor.sh  "$(PKG_DIR)/monitor.sh"
	@cp scripts/monitor.bat "$(PKG_DIR)/monitor.bat"
	@cp scripts/PACKAGE_README.md "$(PKG_DIR)/README.md"
	@chmod +x "$(PKG_DIR)/flash.sh" "$(PKG_DIR)/monitor.sh"
	@{ \
		echo "# bk300_monitor flash package"; \
		echo "# auto-generated by 'make package' on $$(date -u +%Y-%m-%dT%H:%M:%SZ)"; \
		echo "CHIP=$(TARGET)"; \
		echo "PROJECT_NAME=$(PROJECT_NAME)"; \
		echo "BAUD=$(BAUD)"; \
		echo "PKG_VERSION=$(PKG_VERSION)"; \
	} > "$(PKG_DIR)/flash.env"
	@if command -v zip >/dev/null 2>&1; then \
		( cd "$(DIST_DIR)" && zip -qr "$(PKG_NAME).zip" "$(PKG_NAME)" ); \
	else \
		echo ">> 'zip' не найден, делаю tar.gz вместо zip"; \
		tar -czf "$(DIST_DIR)/$(PKG_NAME).tar.gz" -C "$(DIST_DIR)" "$(PKG_NAME)"; \
	fi
	@echo ""
	@echo ">> ГОТОВО"
	@ls -lh "$(DIST_DIR)/$(PKG_NAME)".* 2>/dev/null || true
	@echo ""
	@echo ">> содержимое $(PKG_DIR):"
	@( cd "$(PKG_DIR)" && find . -type f | sort | sed 's|^\./|   |' )
	@echo ""
	@echo ">> на целевом ПК распакуй архив и запусти:"
	@echo "   Linux/macOS: ./flash.sh /dev/ttyUSB0"
	@echo "   Windows    : flash.bat COM5"

.PHONY: package-clean
package-clean:
	rm -rf "$(DIST_DIR)"

# ---- Диагностика ----
.PHONY: doctor
doctor:
	@echo "uname  : $$(uname -a)"
	@echo "shell  : $$SHELL"
	@echo "user   : $$USER (groups: $$(id -Gn $$USER))"
	@if [ -d "$(IDF_PATH)" ]; then \
		echo "IDF    : $(IDF_PATH)"; \
		( cd "$(IDF_PATH)" && git describe --tags --always 2>/dev/null ) || true; \
	else \
		echo "IDF    : НЕ установлен по пути $(IDF_PATH); запусти 'make install-idf'"; \
	fi
	@if [ -d "$(IDF_TOOLS_PATH)" ]; then \
		echo "tools  : $(IDF_TOOLS_PATH)"; \
	else \
		echo "tools  : НЕ установлены в $(IDF_TOOLS_PATH); запусти 'make install-idf'"; \
	fi
	@echo "ports  :"; ls -la /dev/ttyUSB* /dev/ttyACM* 2>/dev/null || echo "  (нет подключённых ESP-плат)"
