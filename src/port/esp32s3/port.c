#define DMOD_ENABLE_REGISTRATION    ON
#include "dmod.h"
#include "dmusbserial_port.h"
#include "dm_sw_ring.h"
#include <errno.h>
#include <stdbool.h>

/*
 * The USB Serial/JTAG controller of the ESP32-S3 (the USB of GPIO19 / GPIO20: the JTAG bridge and a CDC-ACM serial port, which is
 * /dev/ttyACM* on the host). The serial port is one pair of endpoints with a 64-byte FIFO in each direction:
 *
 * - TX: bytes written to EP1 are queued and sent to the host when the host asks (the packet is full, or WR_DONE was set).
 *   While the host does not read (nothing is connected), the FIFO fills and stays full: a write waits for the time given and
 *   gives up.
 * - RX: the host's bytes are in the EP1 FIFO; the interrupt "a packet was received" moves them into the ring of the driver.
 *
 * The controller is already running (the ROM uses it, and the debugger over JTAG): it is not reset or reconfigured here.
 */

#define USB_BASE                0x60038000UL
#define USB_EP1                 (USB_BASE + 0x00UL)
#define USB_EP1_CONF            (USB_BASE + 0x04UL)
#define USB_INT_RAW             (USB_BASE + 0x08UL)
#define USB_INT_ST              (USB_BASE + 0x0CUL)
#define USB_INT_ENA             (USB_BASE + 0x10UL)
#define USB_INT_CLR             (USB_BASE + 0x14UL)

#define EP1_CONF_WR_DONE        (1UL << 0)      /* the bytes written are complete: send them */
#define EP1_CONF_IN_DATA_FREE   (1UL << 1)      /* the TX FIFO has room */
#define EP1_CONF_OUT_DATA_AVAIL (1UL << 2)      /* the RX FIFO holds data */
#define USB_INT_OUT_RECV_PKT    (1UL << 2)      /* a packet from the host has arrived */

#define INTMTX_USB_SERIAL_JTAG_MAP_REG  (0x600C2000UL + 0x180UL)
#define CPU_INTERRUPT           13U             /* level 1: GPIO 2, UART0 3, UART1 9, UART2 12, this 13 */

#define FIFO_SIZE               64U
#define SPINS_PER_TRY           2000U

static bool         s_started = false;
static dm_sw_ring_t s_ring = NULL;

static inline volatile uint32_t *reg(uint32_t address)
{
    return (volatile uint32_t *)address;
}

static inline uint32_t intenable_read(void)
{
    uint32_t value;
    __asm__ __volatile__("rsr.intenable %0" : "=a"(value));
    return value;
}

static inline void intenable_write(uint32_t value)
{
    __asm__ __volatile__("wsr.intenable %0; rsync" :: "a"(value) : "memory");
}

static void rx_drain(void);

static inline bool tx_free(void)
{
    return (*reg(USB_EP1_CONF) & EP1_CONF_IN_DATA_FREE) != 0U;
}

static inline bool rx_available(void)
{
    return (*reg(USB_EP1_CONF) & EP1_CONF_OUT_DATA_AVAIL) != 0U;
}

/** Waits for room in the TX FIFO: spins a little, then sleeps. Returns false when the time ran out. */
static bool wait_tx_free(uint32_t timeout_ms)
{
    for (uint32_t i = 0; i < SPINS_PER_TRY; i++)
        if (tx_free()) return true;

    uint32_t sleeps = timeout_ms == 0U ? 1U : timeout_ms;
    while (sleeps-- != 0U)
    {
        Dmod_ThreadSleep(1);
        if (tx_free()) return true;
    }
    return tx_free();
}

dmod_dmusbserial_port_api_declaration(1.0, int, _init, ( void ))
{
    if (s_started) return -EBUSY;
    *reg(USB_INT_CLR) = USB_INT_OUT_RECV_PKT;
    s_started = true;
    return 0;
}

dmod_dmusbserial_port_api_declaration(1.0, int, _deinit, ( void ))
{
    if (!s_started) return 0;
    Dmod_EnterCritical();
    *reg(USB_INT_ENA) &= ~USB_INT_OUT_RECV_PKT;
    intenable_write(intenable_read() & ~(1UL << CPU_INTERRUPT));
    Dmod_ExitCritical();
    s_ring = NULL;
    s_started = false;
    return 0;
}

dmod_dmusbserial_port_api_declaration(1.0, size_t, _write, ( const uint8_t *data, size_t size, uint32_t timeout_ms ))
{
    if (!s_started || data == NULL) return 0;

    size_t done = 0;
    while (done < size)
    {
        if (!wait_tx_free(timeout_ms)) break;
        /* the FIFO takes bytes while it says it has room (it drops that flag when full) */
        while (done < size && tx_free())
            *reg(USB_EP1) = data[done++];
    }
    if (done != 0U)
        *reg(USB_EP1_CONF) = EP1_CONF_WR_DONE;      /* send what is in the FIFO, a short packet too */
    return done;
}

dmod_dmusbserial_port_api_declaration(1.0, size_t, _read, ( uint8_t *data, size_t size ))
{
    if (!s_started || data == NULL || size == 0U) return 0;

    if (s_ring != NULL)
        return (size_t)dm_sw_ring_read(s_ring, data, (dm_sw_ring_capacity_t)size);

    size_t n = 0;
    while (n < size && rx_available())
        data[n++] = (uint8_t)(*reg(USB_EP1) & 0xFFU);
    return n;
}

dmod_dmusbserial_port_api_declaration(1.0, int, _set_rx_ring, ( dm_sw_ring_t ring ))
{
    if (!s_started) return -EINVAL;

    Dmod_EnterCritical();
    s_ring = ring;
    if (ring != NULL)
    {
        *reg(INTMTX_USB_SERIAL_JTAG_MAP_REG) = CPU_INTERRUPT;
        *reg(USB_INT_ENA) |= USB_INT_OUT_RECV_PKT;
        intenable_write(intenable_read() | (1UL << CPU_INTERRUPT));
        rx_drain();     /* what came before the ring was there */
    }
    else
    {
        *reg(USB_INT_ENA) &= ~USB_INT_OUT_RECV_PKT;
        intenable_write(intenable_read() & ~(1UL << CPU_INTERRUPT));
    }
    Dmod_ExitCritical();
    return 0;
}

dmod_dmusbserial_port_api_declaration(1.0, int, _flush, ( uint32_t timeout_ms ))
{
    if (!s_started) return -EINVAL;
    /* the TX FIFO has room again when the host has read the packet (it is free, empty or not, until something is written) */
    return wait_tx_free(timeout_ms) ? 0 : -ETIMEDOUT;
}

/** The bytes of the RX FIFO into the ring. The interrupt is cleared BEFORE the FIFO is read: a packet that arrives meanwhile
    raises it again (cleared after, it would be lost and the data would wait in the FIFO with no interrupt to fetch it). */
static void rx_drain(void)
{
    uint8_t buf[FIFO_SIZE];
    for (;;)
    {
        *reg(USB_INT_CLR) = USB_INT_OUT_RECV_PKT;
        uint32_t n = 0;
        while (n < sizeof(buf) && rx_available())
            buf[n++] = (uint8_t)(*reg(USB_EP1) & 0xFFU);
        if (n == 0U) return;
        if (s_ring != NULL)
            dm_sw_ring_write(s_ring, buf, (dm_sw_ring_capacity_t)n);
    }
}

static void usb_irq_handler(void)
{
    if (*reg(USB_INT_ST) & USB_INT_OUT_RECV_PKT)
        rx_drain();
}

DMOD_IRQ_HANDLER(13)    /* CPU_INTERRUPT */
{
    usb_irq_handler();
}

int dmod_init(const Dmod_Config_t *Config)
{
    DMOD_LOG_INFO("DMUSBSERIAL port module initialized (ESP32-S3)\n");
    return 0;
}

int dmod_deinit(void)
{
    dmusbserial_port_deinit();
    DMOD_LOG_INFO("DMUSBSERIAL port module deinitialized (ESP32-S3)\n");
    return 0;
}
