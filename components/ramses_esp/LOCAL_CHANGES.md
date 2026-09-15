# Local changes to `ramses_esp`

Vendored from [`wlcrs/esphome-ramses`](https://github.com/wlcrs/esphome-ramses)
(MIT, see `LICENSE`) with one fix applied:

**Heap-corruption fix.** `RamsesMessage` is passed by value through FreeRTOS
queues (`xQueueSend`/`xQueueReceive`), which copy items with a raw `memcpy`.
The struct had a `std::string timestamp` member, so `memcpy` aliased the
string's internal pointer across objects — causing a `free()` of a non-heap
pointer on TX and a double-free / use-after-free on RX.

The `timestamp` field was write-only (never read; `to_hgi80()` doesn't use it),
so it was removed. `RamsesMessage` is now trivially copyable, and a
`static_assert` guards the invariant.
