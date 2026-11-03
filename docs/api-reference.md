# dmusbserial API Reference

## Device

A stream device (`read` / `write` ignore the offset, `stat` reports size 0). The driver announces it to dmtty
(`dmtty_device_available`), which attaches a TTY.

## ioctl (`dmusbserial_types.h`, from `DMUSBSERIAL_IOCTL_BASE`)

See [dmusbserial.md](dmusbserial.md).

## Port API (`dmusbserial_port.h`)

| Function | |
|----------|---|
| `int dmusbserial_port_init(void)` / `_deinit` | Starts / stops the port (does not reset the controller) |
| `size_t dmusbserial_port_write(data, size, timeout_ms)` | Sends and flushes; returns the bytes taken |
| `size_t dmusbserial_port_read(data, size)` | From the ring, else from the FIFO; does not wait |
| `int dmusbserial_port_set_rx_ring(ring)` | The interrupt feeds the ring |
| `int dmusbserial_port_flush(timeout_ms)` | Waits for the host to take what was written |
