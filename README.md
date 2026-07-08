# AQM — ESP32 Air Quality Monitor

Firmware for an ESP32 dev board that reads three I2C sensors and pushes the
readings to InfluxDB over WiFi, with Grafana for dashboards:

- **Sensirion SCD-41** — CO2, temperature, humidity
- **BH1750** — ambient light (lux)
- **Bosch BME680** — temperature, humidity, pressure, gas resistance

Readings are also printed to the serial console (115200 baud) for local
debugging. All sensors share a single I2C bus, and the main loop polls them
every 5 seconds (bounded by the SCD-41's own sample rate) without blocking on
network I/O.

Data flows: ESP32 → InfluxDB (LAN) → Grafana. Tailscale is only used to view
the Grafana/InfluxDB dashboards remotely — the ESP32 itself just needs local
network access to the InfluxDB host.

## Hardware

Wire all three sensors to the ESP32's I2C bus (SDA/SCL) and power. The BME680
address is hardcoded to `0x77` — adjust `BME680_I2C_ADDR` in `src/main.cpp` if
your breakout uses `0x76` instead.

## Prerequisites

- [uv](https://docs.astral.sh/uv/) (used to install PlatformIO Core in an
  isolated environment)
- [Docker](https://docs.docker.com/get-docker/) + Docker Compose (for the
  InfluxDB/Grafana stack)
- An ESP32 dev board connected over USB for flashing

## Setup

1. **Install PlatformIO and project dependencies:**

   ```
   make setup
   ```

2. **Configure stack secrets:**

   ```
   cp .env.example .env
   ```

   Edit `.env` and fill in an InfluxDB admin password/token of your choosing,
   plus your Tailscale tailnet hostname (only needed if you want remote
   access — see "Remote access via Tailscale" below). `docker-compose.yml`
   reads this file automatically.

3. **Start the InfluxDB + Grafana stack** (can run on any always-on machine
   on your LAN, not necessarily where you flash the firmware):

   ```
   make stack-up
   ```

   This brings up:
   - InfluxDB UI at `http://localhost:8086` (login `admin` / your
     `INFLUXDB_ADMIN_PASSWORD` from `.env`)
   - Grafana UI at `http://localhost:3001` (login `admin` / `admin`, InfluxDB
     datasource is auto-provisioned). Host port 3001 is used instead of
     Grafana's default 3000 to avoid clashing with other services on this
     host.

   Change the InfluxDB admin password/token in `.env` before any non-LAN
   deployment.

4. **Configure device secrets:**

   ```
   cp include/secrets.h.example include/secrets.h
   ```

   Edit `include/secrets.h` with your WiFi credentials and the InfluxDB URL.
   Use the **LAN IP** of the machine running `docker compose` (e.g.
   `http://192.168.1.50:8086`), not `localhost` — the ESP32 is a separate
   device on the network. `INFLUXDB_TOKEN` must match `INFLUXDB_ADMIN_TOKEN`
   in `.env`.

5. **Flash the firmware:**

   ```
   make flash
   ```

   This compiles, uploads to the connected ESP32, and opens the serial
   monitor so you can watch it connect and start reporting readings.

## Other commands

| Command             | Description                              |
|----------------------|-------------------------------------------|
| `make build`         | Compile the firmware only                 |
| `make upload`        | Compile and flash, no serial monitor      |
| `make monitor`       | Open the serial monitor only              |
| `make clean`         | Remove build artifacts                    |
| `make stack-down`    | Stop the InfluxDB + Grafana containers    |
| `make stack-logs`    | Tail logs from the stack                  |
| `make help`          | List all available targets                |

## Remote access via Tailscale

Grafana can be exposed on your tailnet at `https://$TAILSCALE_HOSTNAME/aqm`
via [Tailscale Serve](https://tailscale.com/kb/1242/tailscale-serve), where
`$TAILSCALE_HOSTNAME` is whatever you set in `.env` (find yours with
`tailscale status --self`, or in the
[admin console](https://login.tailscale.com/admin/machines)). This is
tailnet-only (not Funnel) — only devices logged into the same tailnet can
reach it.

Set up the mapping with (no hostname needed here — Tailscale infers it from
the machine itself):

```
tailscale serve --bg --set-path=/aqm http://127.0.0.1:3001/aqm
```

Two things make this work together, both required:

- Tailscale Serve's `--set-path` **strips** the `/aqm` prefix before
  forwarding to the backend, so the backend target URL must include `/aqm`
  itself (`http://127.0.0.1:3001/aqm`, not just `http://127.0.0.1:3001`) to
  get it back.
- Grafana needs to know it's mounted under `/aqm` so its own links/assets
  resolve correctly. That's set via `GF_SERVER_ROOT_URL` (built from
  `TAILSCALE_HOSTNAME` in `.env`) and `GF_SERVER_SERVE_FROM_SUB_PATH` in
  `docker-compose.yml`. Without these, Grafana redirects bare `/login` to the
  external `/aqm/login` URL, which Tailscale strips back down to `/login` on
  the next hit — an infinite redirect loop.

If Grafana's port (`3001`) or `TAILSCALE_HOSTNAME` in `.env` ever change, the
`tailscale serve` command above needs to be re-run and the Grafana container
recreated (`docker compose up -d grafana`) to pick up the new root URL, or
this breaks again.

Check current Serve routes with `tailscale serve status`, and remove this one
with `tailscale serve --https=443 --set-path=/aqm off`.

## Data model

Each reading cycle writes a single point to the `air_quality` measurement,
tagged with `device` (from `DEVICE_NAME` in `secrets.h`). Fields are only
included when their sensor read succeeds that cycle, so a failing sensor
just drops out of the time series instead of writing bogus data:

- `co2`, `scd4x_temperature`, `scd4x_humidity`
- `light`
- `bme_temperature`, `bme_humidity`, `pressure`, `gas_resistance`
