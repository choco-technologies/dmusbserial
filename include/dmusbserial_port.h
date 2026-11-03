#ifndef DMUSBSERIAL_PORT_H
#define DMUSBSERIAL_PORT_H

#include "dmod_types.h"
#include "dmusbserial_port_defs.h"
#include "dmusbserial_types.h"
#include "dm_sw_ring.h"

/**
 * Starts the USB serial port. Does not touch what the chip uses the USB for otherwise (the JTAG bridge keeps working).
 * @return 0, -EBUSY when started already
 */
dmod_dmusbserial_port_api(1.0, int, _init, ( void ));
dmod_dmusbserial_port_api(1.0, int, _deinit, ( void ));

/**
 * Sends the bytes and makes them available to the host at once. Waits at most timeout_ms for room when the host does not read.
 * @return bytes taken (less than size when the host did not read in time)
 */
dmod_dmusbserial_port_api(1.0, size_t, _write, ( const uint8_t *data, size_t size, uint32_t timeout_ms ));

/**
 * Reads what has arrived: from the ring when one is set, else from the FIFO of the port (does not wait).
 * @return bytes read
 */
dmod_dmusbserial_port_api(1.0, size_t, _read, ( uint8_t *data, size_t size ));

/** The bytes that arrive from the host go into the ring from the interrupt (NULL: back to reading the FIFO). */
dmod_dmusbserial_port_api(1.0, int, _set_rx_ring, ( dm_sw_ring_t ring ));

/** Waits (up to timeout_ms) until the host has taken everything written. */
dmod_dmusbserial_port_api(1.0, int, _flush, ( uint32_t timeout_ms ));

#endif /* DMUSBSERIAL_PORT_H */
