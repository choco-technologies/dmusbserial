# dmusbserial

## The ESP32-S3 port

The USB Serial/JTAG controller has one pair of endpoints for the serial port (EP1) with a 64-byte FIFO in each direction:

- **Write**: bytes written to the EP1 register are queued; the controller sends the packet when it is full (64 bytes) or when
  `WR_DONE` is set - the port sets it after each write, so short writes reach the host at once. While the host does not read,
  `IN_DATA_FREE` stays 0: the write waits `tx_timeout_ms` (a spin, then sleeps of 1 ms) and gives up with what it has written.
- **Read**: the host's bytes are in the same register while `OUT_DATA_AVAIL` is set. The interrupt "packet received" (the source
  of the USB Serial/JTAG goes to the CPU interrupt 13, level 1) clears itself **before** it reads the FIFO - a packet that arrives
  while it reads raises it again - and moves the bytes into the ring.

The CPU interrupts in use: GPIO 2 (dmgpio), UART0 3, UART1 9, UART2 12 (dmuart), USB Serial/JTAG 13.

## When no host reads

A console that blocks on a port nobody reads is a hung system. After a write that timed out the driver is in the "no host" state:
writes return at once (the bytes are counted as dropped) until a write is taken completely again. The write system call always
returns the full size.

## ioctl

| Command | arg | |
|---------|-----|---|
| `dmusbserial_ioctl_cmd_get_connected` | `int *` | 1 when the last write was taken by the host |
| `dmusbserial_ioctl_cmd_get_dropped` | `uint32_t *` | bytes dropped because the host did not read |

## Limits

- One instance (the chip has one USB Serial/JTAG).
- The line settings of the host (baud rate, parity) mean nothing to the controller: there is no UART behind it.
- The port is the one the debugger uses for the JTAG: nothing here touches it, but a host that opens the serial port resets the
  board (see the README).
