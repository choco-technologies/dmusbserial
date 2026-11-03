# dmusbserial

[![License](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)

The USB serial port driver for [DMOD](https://github.com/choco-technologies/dmod): a DMDRVI driver for `dmdevfs` that serves the
CDC-ACM serial port of the chip's own USB (the ESP32-S3 USB Serial/JTAG controller: `/dev/ttyACM*` on the host) and announces it
to [dmtty](https://github.com/choco-technologies/dmtty), so a console (the shell) runs on the USB connector alone - no UART
adapter.

## Features

- **ESP32-S3 USB Serial/JTAG**: the controller is the one the ROM and the debugger use; it is not reset or reconfigured, so the
  JTAG bridge keeps working next to the serial port.
- **Interrupt driven receiver**: the bytes from the host go into a ring (`dm_sw_ring`) from the interrupt.
- **No host, no hang**: while the host does not read, a write waits `tx_timeout_ms` and then drops the bytes (and drops at once
  until the host reads again), so a console or a log never blocks the system. `ioctl`s tell whether the host is there and how
  much was dropped.
- **dmtty**: `tty0` (or the next) is attached to the device.

## Configuration

```ini
[main]
driver_order=-3

[usb_serial]            ; the device is named after the section
rx_ring_size=256
rx_ring_wait_mode=some_data   ; none | some_data | all_data
tx_timeout_ms=20
```

`configs/board/esp32s3-lcd28.ini` is the one for the ESP32-S3 2.8" display board. Install it with a line in `flash.dmd`:

```
dmusbserial driver=board/esp32s3-lcd28.ini
```

## Using it from the host

The host side is a plain serial port: `/dev/ttyACM*`, any baud rate. **Opening the port from the host resets the board** (the
reset of the USB Serial/JTAG follows DTR / RTS; with `pyserial` it happened also with `dtr = False; rts = False` set before
`open()`): open the port first and let the board boot with it open, and send nothing before the prompt.

## Building

```bash
cmake -DDMOD_CPU_FAMILY=esp32s3 -B build      # needs the xtensa-esp32s3-elf toolchain
cmake --build build
```

## Documentation

See [docs/](docs/README.md), or `dmf-man dmusbserial`.
