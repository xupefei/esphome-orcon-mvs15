# Local changes to `ramses_esp`

Vendored from [`wlcrs/esphome-ramses`](https://github.com/wlcrs/esphome-ramses)
(MIT, see `LICENSE`) with fixes applied while getting reliable RX **and** TX
working against an Orcon MVS-15 on an IndaloTech ESP32-S3 + CC1101 board.

## Fixes

**1. Heap corruption (`ramses_message.h` / `.cpp`, `ramses_esp.cpp`).**
`RamsesMessage` is passed by value through FreeRTOS queues (`xQueueSend`/
`xQueueReceive`), which copy items with a raw `memcpy`. The struct had a
`std::string timestamp` member, so `memcpy` aliased the string's internal
pointer — `free()` of a stack pointer on TX, double-free / use-after-free on RX.
The field was write-only, so it's removed; a `static_assert` now enforces that
`RamsesMessage` stays trivially copyable.

**2. RX had no UART framing awareness — fine; TX had no UART framing at all
(`ramses_message.cpp::to_raw_frame`).** RAMSES sends each byte as UART async
serial (start/stop bits, bit-reversed, packed into FIFO octets). The upstream
TX path only Manchester-encoded the nibbles and never added the async framing,
so transmitted frames were undecodable. `to_raw_frame()` now ports the native
firmware's exact bit encoder (verified byte-for-byte identical) plus the
prime + break prefix.

**3. Message verb defaulted to RQ (`ramses_message.cpp::from_hgi80`).** The verb
(`I`/`RQ`/`W`/`RP`) was stored in `type` but never written into `fields`, and
`to_raw_frame`/checksum read the verb from `fields`. Every transmitted command
went out as **RQ** (a read request) — the fan replied but never acted. Fixed by
folding the verb bits into `fields`.

**4. TX FIFO transport (`ramses_esp.cpp::process_tx_queue`).** The upstream code
started TX then filled the FIFO, and polled the `TXBYTES` register to pace the
feed. `TXBYTES` reads are unreliable on the CC1101 (a known erratum), which made
transmit intermittent. Now matches the native firmware 1:1: **STX into an empty
FIFO, then stream bytes gated by the hardware GDO0 TX-FIFO-threshold flag**
(`IOCFG0=0x02`, GDO0 read as a GPIO). The GDO0/UART-TX pin is repurposed as an
input in `setup()` since we transmit via the FIFO, not UART TX. **This was the
fix that made TX reliable.**

## Board / setup notes (not code, but essential)

- **Pin mapping:** `gdo0_pin` is the UART **RX** pin (CC1101 GDO2, demodulated
  data out); `gdo2_pin` is the CC1101 **GDO0** line (TX data / FIFO flag). On the
  IndaloTech board that's GPIO40 and GPIO39 respectively — see `orcon-mvs15.yaml`.
- **Cold boot required once:** after flashing ESPHome over the native firmware,
  the CC1101 stays powered through the (soft) reset and can latch in a dead RX
  state. A full **power cycle** clears it; subsequent OTA updates are fine.
- **Crystal offset is harmless:** this board reads ~+66 kHz (`FREQEST≈+42`) on
  every frame. RX handles it via AFC; TX works anyway (the native firmware uses
  the identical `FREQ` value). No frequency correction is applied.
