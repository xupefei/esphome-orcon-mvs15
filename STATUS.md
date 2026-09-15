# Project status & handoff

_Snapshot of where the Orcon MVS-15 / ESPHome build stands, so it can be resumed
without re-deriving everything._

## TL;DR

- **RX: fully working and reliable.** The ESP decodes RAMSES II traffic, the HA
  `Speed` entity tracks the fan, neighbour units are filtered out.
- **TX: works but NOT 100% reliable.** Commands reach the fan and change its speed,
  but only land ~most of the time; sometimes a command is missed. Current mitigation
  is sending each command **6× spaced ~200 ms** (`send_ramses` script). Even so, in
  testing it "worked twice" out of several tries — **this is the open problem.**
- Everything is flashed to the board and committed to this repo.

## Hardware / addresses (reference)

- Board: **IndaloTech `ramses_esp`** = ESP32-S3-WROOM-1 + E07-900MM10S (CC1101 @ 868.3 MHz, ~+10 dBm).
- Device on WiFi at `orcon-mvs15.local` (was 10.20.30.243).
- CC1101 pins: CS 38, SCK 36, MOSI 35, MISO 37; **`gdo0_pin`=GPIO40 (UART RX ← CC1101 GDO2)**,
  **`gdo2_pin`=GPIO39 (CC1101 GDO0, TX data / FIFO flag)**. LEDs: RX blue 42, TX green 41.
- Addresses: remote (we transmit AS) `29:160126`; fan `29:227117`; HGI id `18:152992`.
- Payloads — `22F1 003`: Away `000004`, Low `000104`, Med `000204`, High `000304`, Auto `000404`.
  `31D9 003 0000XX` state: 00 Away / 01 Low / 02 Med / 03 High / 04 Auto.
- Env is a **very busy 868 MHz band** — dozens of devices (`32:`, `37:`, `07:`, many `29:`).

## Bugs found & fixed (all in `components/ramses_esp/`, see LOCAL_CHANGES.md)

1. **Heap corruption** — `RamsesMessage` (with a `std::string`) `memcpy`'d through a
   FreeRTOS queue. Removed the dead `timestamp` field; `static_assert` guards it.
2. **Swapped GDO pins** — UART RX must be on the CC1101 **GDO2** line (board GPIO40),
   not GDO0. Verified against native `uart_set_pin(tx=GDO0, rx=GDO2)`.
3. **Cold-boot latch** — after flashing ESPHome over the native firmware the CC1101
   stayed latched dead until a **full power cycle**. Soft/OTA resets don't clear it;
   later OTAs are fine.
4. **TX verb RQ→I** — `from_hgi80` set `type` but not the verb bits in `fields`, so
   every command went out as **RQ** (a read request); the fan replied but never acted.
5. **TX FIFO transport (the reliability lever)** — upstream polled the `TXBYTES`
   register (unreliable, erratum) and filled-then-STX. Now matches native: **STX into
   an empty FIFO, then stream bytes gated by the hardware GDO0 flag** (`IOCFG0=0x02`,
   GDO0 read as a GPIO input). The TX bit-encoder was also ported 1:1 (byte-verified),
   plus the `0x00`+break prime and the RQ→I fix. This took TX from "never" to
   "mostly works".

## The open problem: TX reliability

TX is correct and works, but misses sometimes. Believed cause: the **congested 868
band** — our single ~12 ms frame collides with other devices. Contributing factors,
and what was tried:

- **Repeats help.** 6× spaced ~200 ms is the current setting and the most reliable so
  far (still not perfect). 1× is unreliable.
- **Listen-before-talk (native's `FRM_MIN_TX_DELAY`) was tried and REVERTED** — it made
  things worse in this band (likely starved TX: the channel rarely stays quiet 50 ms,
  so the gate blocked/delayed sends). If retried, do it smarter (RSSI/carrier-sense
  with backoff, or a shorter/tunable quiet window) and confirm it doesn't starve.
- **Crystal frequency offset (~+66 kHz, `FREQEST≈+42` on every RX frame) is a RED
  HERRING** — the native firmware transmits reliably on this same board with the same
  offset, so it is NOT the blocker. Frequency-correction attempts (`FSCTRL0`, `FREQ`
  regs) were tried and reverted; they didn't help and didn't behave predictably blind.
- **Marginal link:** the fan is heard at only ~−102 dBm (near CC1101 sensitivity). The
  handheld remote works because it's used near the fan. Our fixed ESP has a weak link.

### Best next steps (in priority order)

1. **Improve the RF link (most likely to actually fix it):** move the ESP closer to
   the fan, use a proper 868 MHz antenna in the clear (away from the metal MVHR
   housing), and/or swap to a **+20 dBm module (E07-900M20S)** — ~10 dB more margin.
2. **Get an RTL-SDR (~€30)** and capture both our TX and the real remote's TX. This is
   the only way to *see* whether ours differs in frequency/power/shape — turns blind
   guessing into measurement. Compare on-air bytes/timing directly.
3. **Smarter listen-before-talk:** gate on the CC1101 carrier-sense (RSSI/CS in
   `PKTSTATUS`) with a short window + retry/backoff, instead of the frame-state gate
   that starved. Confirm it fires (add temporary logging of how often it defers).
4. Consider the pragmatic fallback: **native IndaloTech firmware + Home Assistant over
   MQTT/`ramses_cc`** — TX is 100% reliable there; not pure ESPHome.

## How to build / flash / watch logs

Toolchain is a Python 3.13 venv (PyPI reached via the Databricks proxy):

```bash
# compile + OTA (device already on ESPHome; USB only needed the very first time)
cd ~/esphome-orcon-mvs15
PIP_INDEX_URL=https://pypi-proxy.cloud.databricks.com/simple \
  /tmp/esphome-venv/bin/esphome compile orcon-mvs15.yaml
/tmp/esphome-venv/bin/esphome upload orcon-mvs15.yaml --device orcon-mvs15.local

# live logs (RX: frames are at INFO right now)
/tmp/esphome-venv/bin/esphome logs orcon-mvs15.yaml --device orcon-mvs15.local
```

- Reduce log noise later by lowering the `RX:` log in `ramses_frame.cpp` back to
  `ESP_LOGD` (it's at `ESP_LOGI` now for debugging).
- Repeat count / spacing is the `send_ramses` script in `orcon-mvs15.yaml`.
- `secrets.yaml` (WiFi) is git-ignored; it exists locally on this machine.

## Notes / loose ends

- The old fork `github.com/xupefei/esphome-ramses` only has the heap fix; the real
  work lives in this repo's vendored `components/ramses_esp/`. The fork can be deleted
  (needs `delete_repo` scope) or used to open an upstream PR (the TX-framing, verb, and
  GDO0-transport fixes would help everyone).
- Web UI at `http://orcon-mvs15.local/`; weekly uptime-based reboot is configured.
