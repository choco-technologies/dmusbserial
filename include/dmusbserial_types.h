#ifndef DMUSBSERIAL_TYPES_H
#define DMUSBSERIAL_TYPES_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/**
 * @brief Base of the dmusbserial private ioctl range (== DMDRVI_IOCTL_CUSTOM_BASE)
 *
 * Numbers below it belong to dmdrvi's standard commands (network, block, monitor), which this driver does not implement and
 * answers with -ENOTTY.
 */
#define DMUSBSERIAL_IOCTL_BASE   0x1000

/**
 * @brief IOCTL commands for the DMUSBSERIAL device
 */
typedef enum
{
    dmusbserial_ioctl_cmd_get_connected = DMUSBSERIAL_IOCTL_BASE, /**< arg = int* - 1 when the host takes what is written (the last write did not time out) */
    dmusbserial_ioctl_cmd_get_dropped,                           /**< arg = uint32_t* - bytes dropped because the host did not read */

    dmusbserial_ioctl_cmd_max
} dmusbserial_ioctl_cmd_t;

/**
 * @brief Opaque driver context type (forward declaration)
 */
struct dmdrvi_context;
typedef struct dmdrvi_context *dmdrvi_context_t;

#endif /* DMUSBSERIAL_TYPES_H */
