# ESPHome — Orcon MVS-15 (RAMSES II / 868 MHz)

An [ESPHome](https://esphome.io/) config that controls an **Orcon MVS-15** MVHR
ventilation unit over its native **RAMSES II** RF protocol at **868.3 MHz**, by
impersonating the 15RF remote. No wiring into the unit, no pairing — the ESP just
transmits frames from an address the fan already trusts.

It exposes the fan in Home Assistant (native ESPHome API) **and** as a
self-hosted web page, with a speed selector, timed-boost buttons, live state
tracking from the fan's own broadcasts, and RX/TX status LEDs.

## Hardware

Built for the **[IndaloTech `ramses_esp`](https://github.com/IndaloTech/ramses_esp)
board**: an **ESP32-S3-WROOM-1** + **E07-900MM10S (CC1101 @ 868 MHz)** with an SMA
antenna. The CC1101 pin map and LED pins in the config are taken straight from that
board's firmware defaults, so **no wiring is required** — just reflash it.

Any ESP32/ESP32-S3 + CC1101 (868 MHz) will work; adjust the pins in
`orcon-mvs15.yaml` to match your wiring.

| Signal | GPIO (IndaloTech board) |
|--------|-------------------------|
| CS     | 38 |
| SCK    | 36 |
| MOSI   | 35 |
| MISO   | 37 |
| GDO0   | 39 |
| GDO2   | 40 |
| RX LED (blue)  | 42 |
| TX LED (green) | 41 |

## What it does

- **Speed** (`select`): Away / Low / Medium / High / Auto → sends verified `22F1` frames.
- **Boost 15/30/60 min** (`button`): sends `22F3` timed-boost frames at High.
- **State tracking**: parses the fan's unsolicited `31D9` broadcasts so the entity
  stays correct even when you use the physical remote.
- **LEDs**: blue blinks on any received frame, green blinks on transmit.
- **Web UI**: `http://orcon-mvs15.local/` — works with no internet (assets embedded).
- **Home Assistant**: native ESPHome API, auto-discovered. No MQTT broker needed.

## RAMSES II frames used

Device addresses are set via `substitutions` at the top of the YAML. Replace them
with **your own** sniffed values.

- `remote_id` — the address this gateway transmits *as* (your real remote).
- `fan_id` — your MVS-15 unit.

Speed (`22F1 003`), middle payload byte = speed:

| Speed  | Payload  |
|--------|----------|
| Away   | `000004` |
| Low    | `000104` |
| Medium | `000204` |
| High   | `000304` |
| Auto   | `000404` |

Boost (`22F3 007`), 3rd byte = duration in minutes (hex): `0F`=15, `1E`=30, `3C`=60.

State readback (`31D9 003 0000XX`): `00` Away / `01` Low / `02` Medium / `03` High / `04` Auto.

> **Finding your own IDs:** flash with `logger: level: DEBUG`, press buttons on your
> physical remote, and watch the log for `22F1` frames — the source is your remote's
> address, the destination is the fan. Plug both into the `substitutions` block.

## Usage

1. `pip3 install esphome` (or `brew install esphome`).
2. `cp secrets.yaml.example secrets.yaml` and fill in your Wi-Fi.
3. Edit the `substitutions` in `orcon-mvs15.yaml` with your device IDs (and pins, if
   not using the IndaloTech board).
4. First flash over USB (ESPHome can't OTA onto other firmware):
   ```bash
   esphome run orcon-mvs15.yaml --device /dev/cu.usbmodemXXXX
   ```
   Force the S3 bootloader if needed: hold **FUNCTION** (BOOT), tap **RESET**, release.
   An `esptool.py --chip esp32s3 erase_flash` first is recommended when coming from
   other firmware.
5. After that, updates are OTA over Wi-Fi.

## Credits

- [`wlcrs/esphome-ramses`](https://github.com/wlcrs/esphome-ramses) — the ESPHome
  CC1101 / RAMSES II transport component. A patched copy is vendored here under
  [`components/ramses_esp/`](components/ramses_esp/) (MIT; see its `LICENSE` and
  `LOCAL_CHANGES.md`).
- [`IndaloTech/ramses_esp`](https://github.com/IndaloTech/ramses_esp) — the reference
  firmware and hardware (Peter Price).
- [`bitboxx/orcon-mvs15-rf`](https://github.com/bitboxx/orcon-mvs15-rf) — documented
  RAMSES II command payloads for the Orcon MVS-15.
- [`ramses-rf`](https://github.com/ramses-rf) — the RAMSES II protocol project.

## Notes

- The RAMSES component is **vendored locally** (`components/ramses_esp/`) and loaded
  via `external_components: - source: components`, so builds are self-contained and
  reproducible with no network fetch. It carries several fixes over upstream (heap
  corruption, TX UART-framing, the RQ→I verb, and the GDO0-based TX transport that
  makes transmit reliable) — see `components/ramses_esp/LOCAL_CHANGES.md`.
- This impersonates a remote your fan already knows; it does not pair. The physical
  remote keeps working.

## Troubleshooting

- **After flashing ESPHome over other firmware, power-cycle the board once.** A soft
  reset (OTA/USB re-flash) doesn't reset the CC1101, which can latch in a dead RX
  state. Unplug ~15 s. Later ESPHome OTA updates don't need this.
- **RX works but the fan ignores commands?** TX requires UART framing, the `I` verb,
  and GDO0-paced FIFO writes. Do not poll `TXBYTES` (CC1101 erratum).
- **`gdo0_pin` = UART RX (CC1101 GDO2); `gdo2_pin` = CC1101 GDO0 (TX data + FIFO
  flag).** Confusingly named, but that's the mapping. Get it wrong and RX is silent.
- TX uses 1 MHz SPI, an initial FIFO preload, GDO0 interrupt-driven refills and
  completion, direct RX recovery, and a 50 ms inter-frame guard.
- A steady `FREQEST` offset on received frames (~+42 / 66 kHz on this board) is just
  crystal tolerance and is **harmless** — RX compensates via AFC, TX works anyway.
