# Local changes to `ramses_esp`

Vendored from [`wlcrs/esphome-ramses`](https://github.com/wlcrs/esphome-ramses)
under the included MIT license.

## Fixes

1. **Queue safety:** removed the `std::string` member from `RamsesMessage` and added
   a trivially-copyable assertion. FreeRTOS queues copy messages with `memcpy`.
2. **TX encoding:** added the IndaloTech UART framing, Manchester encoding,
   prime/break prefix, trailer, and training bytes.
3. **Message verb:** write the parsed `I`/`RQ`/`W`/`RP` verb into the encoded fields.
4. **TX transport:** use 1 MHz SPI, one `0xC3` PA entry, an initial FIFO preload,
   GDO0 interrupt-driven refills and completion, direct RX recovery, and a 50 ms
   frame guard. No `TXBYTES` reads are used.

## Board notes

- `gdo0_pin` is UART RX from CC1101 GDO2 (GPIO40 on the IndaloTech board).
- `gdo2_pin` is CC1101 GDO0 FIFO status (GPIO39).
- Power-cycle once if the CC1101 remains latched after replacing other firmware.
