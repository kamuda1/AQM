# AQM — ESP32 Air Quality Monitor
#
# Firmware for an ESP32 dev board that reads three I2C sensors and prints
# readings over the serial monitor (115200 baud):
#   - Sensirion SCD-41   : CO2, temperature, humidity
#   - BH1750             : ambient light (lux)
#   - Bosch BME680       : temperature, humidity, pressure, gas resistance
#
# The project is built with PlatformIO (Arduino framework). PlatformIO Core
# is a Python tool, so we install it with uv (`uv tool install`), which keeps
# it in an isolated, uv-managed environment. This Makefile bootstraps that
# toolchain and wraps the common tasks.

# Prefer a `pio` already on PATH; otherwise use the one uv installs into its
# tool bin directory.
UV  ?= uv
PIO ?= $(shell command -v pio 2>/dev/null || echo $(shell $(UV) tool dir 2>/dev/null)/platformio/bin/pio)

.DEFAULT_GOAL := help

.PHONY: help
help: ## Show this help
	@grep -E '^[a-zA-Z_-]+:.*?## .*$$' $(MAKEFILE_LIST) \
		| awk 'BEGIN {FS = ":.*?## "}; {printf "  \033[36m%-12s\033[0m %s\n", $$1, $$2}'

.PHONY: setup
setup: install deps ## Install PlatformIO and fetch project dependencies

.PHONY: install
install: ## Install PlatformIO Core via uv if it is not already present
	@if ! command -v $(UV) >/dev/null 2>&1; then \
		echo "uv is required but not installed. See https://docs.astral.sh/uv/"; \
		exit 1; \
	fi
	@if [ -x "$(PIO)" ] || command -v pio >/dev/null 2>&1; then \
		echo "PlatformIO already installed: $$($(PIO) --version)"; \
	else \
		echo "Installing PlatformIO Core with uv..."; \
		$(UV) tool install platformio; \
		echo "PlatformIO installed. Ensure '$(UV) tool dir'/../bin is on your PATH (run: $(UV) tool update-shell)."; \
	fi

.PHONY: deps
deps: ## Download board platform and library dependencies from platformio.ini
	$(PIO) pkg install

.PHONY: build
build: ## Compile the firmware
	$(PIO) run

.PHONY: upload
upload: ## Compile and flash the firmware to a connected ESP32
	$(PIO) run --target upload

.PHONY: monitor
monitor: ## Open the serial monitor (115200 baud)
	$(PIO) device monitor

.PHONY: flash
flash: upload monitor ## Upload then open the serial monitor

.PHONY: clean
clean: ## Remove build artifacts
	$(PIO) run --target clean
