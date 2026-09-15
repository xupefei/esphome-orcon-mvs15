# Project status

## Current state

- RX decodes RAMSES II traffic and filters state updates to this fan.
- TX follows the IndaloTech factory firmware path and sends one frame per command.
- Single-frame Low and High commands were confirmed through the fan's `31D9` replies.
- Firmware is running on `orcon-mvs15.local` (`10.20.30.243`).

## Hardware

- Board: IndaloTech `ramses_esp` (ESP32-S3 + E07-900MM10S/CC1101).
- SPI: CS 38, SCK 36, MOSI 35, MISO 37.
- GPIO40: UART RX from CC1101 GDO2.
- GPIO39: CC1101 GDO0 FIFO status.
- LEDs: RX 42, TX 41.
- Remote: `29:160126`; fan: `29:227117`; HGI: `18:152992`.

## Protocol

- `22F1 003`: Away `000004`, Low `000104`, Medium `000204`, High `000304`, Auto `000404`.
- `31D9 003 0000XX`: Away `00`, Low `01`, Medium `02`, High `03`, Auto `04`.
- `22F3 007`: timed boost commands are defined in `orcon-mvs15.yaml`.

## Important implementation details

- `RamsesMessage` must remain trivially copyable because FreeRTOS queues use `memcpy`.
- TX uses the factory 1 MHz SPI configuration, five-byte FIFO threshold, `0xC3`
  PA entry, GDO0-paced FIFO writes, FIFO-empty completion, and 50 ms frame guard.
- TX frames use the factory UART/Manchester encoder and the `I` verb.
- A full power cycle may be needed once after replacing non-ESPHome firmware.

## Commands

```bash
/tmp/esphome-venv/bin/esphome compile orcon-mvs15.yaml
/tmp/esphome-venv/bin/esphome upload orcon-mvs15.yaml --device orcon-mvs15.local
/tmp/esphome-venv/bin/esphome logs orcon-mvs15.yaml --device orcon-mvs15.local
```

See `components/ramses_esp/LOCAL_CHANGES.md` for the upstream delta.
