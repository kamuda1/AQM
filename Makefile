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

# Which board to build/flash. One PlatformIO environment per physical board
# (see platformio.ini). Override on the command line, e.g.:
#   make flash ENV=mushroom
ENV ?= comfort

# Device tag to inspect with `make check` (matches DEVICE_NAME / the env).
DEVICE ?= esp32-mushroom

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
		$(UV) tool install --with pip platformio; \
		echo "PlatformIO installed. Ensure '$(UV) tool dir'/../bin is on your PATH (run: $(UV) tool update-shell)."; \
	fi

.PHONY: deps
deps: ## Download board platform and library dependencies from platformio.ini
	$(PIO) pkg install

.PHONY: build
build: ## Compile the firmware (ENV=comfort|mushroom)
	$(PIO) run -e $(ENV)

.PHONY: upload
upload: ## Compile and flash the firmware to a connected ESP32 (ENV=...)
	$(PIO) run -e $(ENV) --target upload

.PHONY: monitor
monitor: ## Open the serial monitor (115200 baud)
	$(PIO) device monitor -e $(ENV)

.PHONY: flash
flash: upload monitor ## Upload then open the serial monitor (ENV=...)

.PHONY: check
check: ## Show the latest InfluxDB reading per field for a board (DEVICE=...)
	@SEC=include/secrets.h; \
	get() { grep "^#define $$1 " $$SEC | sed -E 's/.*"([^"]*)".*/\1/'; }; \
	URL=$$(get INFLUXDB_URL); ORG=$$(get INFLUXDB_ORG); \
	BUCKET=$$(get INFLUXDB_BUCKET); TOKEN=$$(get INFLUXDB_TOKEN); \
	echo "Latest '$(DEVICE)' readings from $$URL (bucket $$BUCKET, last 10m):"; \
	curl -s -m 10 "$$URL/api/v2/query?org=$$ORG" \
		-H "Authorization: Token $$TOKEN" \
		-H "Content-Type: application/vnd.flux" \
		-H "Accept: application/csv" \
		--data "from(bucket:\"$$BUCKET\") |> range(start:-10m) |> filter(fn:(r)=>r.device==\"$(DEVICE)\") |> last() |> keep(columns:[\"_time\",\"_field\",\"_value\"])" \
		| tr -d '\r'; \
	echo "(no rows below the header = board isn't writing yet; check the serial monitor for 'InfluxDB write failed')"

.PHONY: clean
clean: ## Remove build artifacts (ENV=...)
	$(PIO) run -e $(ENV) --target clean

# --- Time-series stack (InfluxDB + Grafana) ---

.PHONY: stack-up
stack-up: ## Start the InfluxDB + Grafana containers
	docker compose up -d
	@echo "InfluxDB: http://localhost:8086   Grafana: http://localhost:3001"

.PHONY: stack-down
stack-down: ## Stop the InfluxDB + Grafana containers
	docker compose down

.PHONY: stack-logs
stack-logs: ## Tail logs from the stack
	docker compose logs -f
