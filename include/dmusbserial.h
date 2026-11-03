#ifndef DMUSBSERIAL_H
#define DMUSBSERIAL_H

#include "dmusbserial_defs.h"
#include "dmusbserial_types.h"

/**
 * @brief USB serial driver configuration (the [dmusbserial] section, or a section named after the device)
 */
typedef struct
{
    uint32_t    rx_ring_size;       /**< Capacity of the RX ring buffer in bytes (0: no ring, the FIFO is read directly) */
    uint32_t    tx_timeout_ms;      /**< How long a write waits for the host to take data before it drops it */
} dmusbserial_config_t;

#endif /* DMUSBSERIAL_H */
