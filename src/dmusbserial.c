#define DMOD_ENABLE_REGISTRATION    ON
#include "dmod.h"
#include "dmusbserial.h"
#include "dmusbserial_port.h"
#include "dmdrvi.h"
#include "dmhaman.h"
#include "dmtty_types.h"
#include "dmini.h"
#include "dm_sw_ring.h"
#include <errno.h>
#include <string.h>

#define DMUSBSERIAL_CONTEXT_MAGIC   0x44555342  /* 'DUSB' */
#define DEFAULT_SECTION             "dmusbserial"

/**
 * @brief DMDRVI context structure
 */
struct dmdrvi_context
{
    uint32_t                magic;          /**< Magic number for validation */
    dmusbserial_config_t    config;         /**< Configuration parameters */
    dm_sw_ring_t            rx_ring;        /**< Ring of the received bytes (written by the interrupt) */
    dm_sw_ring_flags_t      rx_wait_flags;  /**< RX ring read-wait behavior */
    char                   *tty_path;       /**< Absolute dmdevfs path announced to dmtty (NULL = not announced) */
    bool                    connected;      /**< The last write was taken by the host */
    uint32_t                dropped;        /**< Bytes dropped because the host did not read */
};

static int is_valid_context(dmdrvi_context_t context)
{
    return (context != NULL && context->magic == DMUSBSERIAL_CONTEXT_MAGIC);
}

/* The port has one USB serial port: one driver instance */
static dmdrvi_context_t s_instance = NULL;

static dm_sw_ring_flags_t string_to_rx_wait_flags(const char *s)
{
    if (s != NULL)
    {
        if (strcmp(s, "none")     == 0) return 0;
        if (strcmp(s, "all_data") == 0) return dm_sw_ring_flags_wait_for_all_data;
    }
    return dm_sw_ring_flags_wait_for_some_data;
}

/**
 * @brief The section that holds the configuration: [dmusbserial], or the first other section (not [main]) with one of our keys
 *        - the device is named after it. Returns the name (section_buf, or the literal DEFAULT_SECTION).
 */
static const char *detect_config_section(dmini_context_t ini, char *section_buf, size_t section_buf_sz)
{
    if (dmini_has_key(ini, DEFAULT_SECTION, "rx_ring_size") || dmini_has_key(ini, DEFAULT_SECTION, "tx_timeout_ms"))
        return DEFAULT_SECTION;

    int needed = dmini_generate_string(ini, NULL, 0);
    if (needed <= 1)
        return DEFAULT_SECTION;

    char *ini_str = (char *)Dmod_Malloc((size_t)needed + 1);
    if (ini_str == NULL)
        return DEFAULT_SECTION;
    if (dmini_generate_string(ini, ini_str, (size_t)needed + 1) <= 0)
    {
        Dmod_Free(ini_str);
        return DEFAULT_SECTION;
    }
    ini_str[needed] = '\0';

    const char *result = DEFAULT_SECTION;
    for (char *p = ini_str; *p != '\0'; p++)
    {
        if (*p != '[') continue;
        char *name_start = p + 1;
        char *name_end = name_start;
        while (*name_end != '\0' && *name_end != ']' && *name_end != '\n' && *name_end != '\r')
            name_end++;
        if (*name_end != ']') continue;

        size_t name_len = (size_t)(name_end - name_start);
        if (name_len == 0 || name_len >= section_buf_sz) continue;
        memcpy(section_buf, name_start, name_len);
        section_buf[name_len] = '\0';
        if (strcmp(section_buf, "main") != 0 &&
            (dmini_has_key(ini, section_buf, "rx_ring_size") || dmini_has_key(ini, section_buf, "tx_timeout_ms")))
        {
            result = section_buf;
            break;
        }
    }

    Dmod_Free(ini_str);
    return result;
}

static void read_config_parameters(dmdrvi_context_t context, dmini_context_t config)
{
    char section_buf[64];
    const char *section = detect_config_section(config, section_buf, sizeof(section_buf));
    context->config.rx_ring_size  = (uint32_t)dmini_get_int(config, section, "rx_ring_size", 256);
    context->config.tx_timeout_ms = (uint32_t)dmini_get_int(config, section, "tx_timeout_ms", 20);
    context->rx_wait_flags = string_to_rx_wait_flags(dmini_get_string(config, section, "rx_ring_wait_mode", "some_data"));
}

/* ---- DMOD lifecycle ---- */

int dmod_init(const Dmod_Config_t *Config)
{
    DMOD_LOG_INFO("DMUSBSERIAL interface module initialized\n");
    return 0;
}

int dmod_deinit(void)
{
    DMOD_LOG_INFO("DMUSBSERIAL interface module deinitialized\n");
    return 0;
}

/* ---- DMDRVI interface ---- */

dmod_dmdrvi_dif_api_declaration(2.0, dmusbserial, dmdrvi_context_t, _create, ( dmini_context_t config, dmdrvi_dev_num_t* dev_num ))
{
    if (config == NULL || dev_num == NULL)
    {
        DMOD_LOG_ERROR("Invalid parameters to dmusbserial_dmdrvi_create\n");
        return NULL;
    }
    if (s_instance != NULL)
    {
        DMOD_LOG_ERROR("The USB serial port is used already\n");
        return NULL;
    }

    dmdrvi_context_t context = Dmod_Malloc(sizeof(struct dmdrvi_context));
    if (context == NULL)
        return NULL;
    memset(context, 0, sizeof(*context));
    context->magic = DMUSBSERIAL_CONTEXT_MAGIC;
    context->connected = true;
    read_config_parameters(context, config);

    if (dmusbserial_port_init() != 0)
    {
        DMOD_LOG_ERROR("Failed to start the USB serial port\n");
        Dmod_Free(context);
        return NULL;
    }

    if (context->config.rx_ring_size > 0)
    {
        /* No mutex in the ring: it is written from the interrupt (see dmuart) */
        context->rx_ring = dm_sw_ring_create(context->config.rx_ring_size,
                                             dm_sw_ring_flags_drop_old_data | context->rx_wait_flags);
        if (context->rx_ring == NULL)
        {
            DMOD_LOG_ERROR("Failed to create RX ring buffer (size=%u)\n", (unsigned)context->config.rx_ring_size);
        }
        else
        {
            dmusbserial_port_set_rx_ring(context->rx_ring);
        }
    }

    dev_num->flags = DMDRVI_NUM_MINOR;
    dev_num->major = 0;
    dev_num->minor = 0;

    /* A device named after the section ([console_usb]), else numeric */
    char section_buf[DMDRVI_ALT_NAME_MAX_LEN + 1];
    const char *section = detect_config_section(config, section_buf, sizeof(section_buf));
    if (strcmp(section, DEFAULT_SECTION) != 0)
    {
        size_t name_len = strlen(section);
        if (name_len <= DMDRVI_ALT_NAME_MAX_LEN)
        {
            dev_num->flags |= DMDRVI_NUM_ALT_NAME;
            memcpy(dev_num->alt_name, section, name_len + 1);
        }
    }

    s_instance = context;
    DMOD_LOG_INFO("USB serial port started (rx ring %u bytes, tx timeout %u ms)\n",
        (unsigned)context->config.rx_ring_size, (unsigned)context->config.tx_timeout_ms);
    return context;
}

dmod_dmdrvi_dif_api_declaration(2.0, dmusbserial, void, _path_ready, ( dmdrvi_context_t context, const dmdrvi_dev_num_t* dev_num, const char* path ))
{
    if (!is_valid_context(context) || path == NULL)
        return;

    /* Announce the device to dmtty via dmhaman (no direct link to dmtty): a TTY node is attached on top of it */
    context->tty_path = Dmod_StrDup(path);
    if (context->tty_path != NULL)
    {
        dmtty_device_available_params_t tty_params = {
            .path  = context->tty_path,
            .name  = NULL,
            .flags = 0,
        };
        dmhaman_call_handler(DMTTY_HANDLER_NAME_DEVICE_AVAILABLE, &tty_params);
    }
}

dmod_dmdrvi_dif_api_declaration(2.0, dmusbserial, void, _free, ( dmdrvi_context_t context ))
{
    if (!is_valid_context(context))
        return;

    if (context->tty_path != NULL)
    {
        dmtty_device_unavailable_params_t tty_params = { .path = context->tty_path };
        dmhaman_call_handler(DMTTY_HANDLER_NAME_DEVICE_UNAVAILABLE, &tty_params);
        Dmod_Free(context->tty_path);
        context->tty_path = NULL;
    }
    if (context->rx_ring != NULL)
    {
        dmusbserial_port_set_rx_ring(NULL);
        dm_sw_ring_destroy(context->rx_ring);
        context->rx_ring = NULL;
    }
    dmusbserial_port_deinit();
    if (s_instance == context)
        s_instance = NULL;
    context->magic = 0;
    Dmod_Free(context);
}

dmod_dmdrvi_dif_api_declaration(2.0, dmusbserial, void*, _open, ( dmdrvi_context_t context, int flags, const dmdrvi_dev_num_t* dev_num ))
{
    (void)flags; (void)dev_num;
    return is_valid_context(context) ? (void *)context : NULL;
}

dmod_dmdrvi_dif_api_declaration(2.0, dmusbserial, void, _close, ( dmdrvi_context_t context, void* handle ))
{
    (void)context; (void)handle;
}

dmod_dmdrvi_dif_api_declaration(2.0, dmusbserial, dmdrvi_ssize_t, _read, ( dmdrvi_context_t context, void* handle, void* buffer, size_t size, dmdrvi_offset_t offset ))
{
    (void)handle;
    if (offset < 0) return -EINVAL;
    if (size > (size_t)INT64_MAX) return -EOVERFLOW;
    /* a stream, not seekable: the offset is not used */
    if (!is_valid_context(context) || buffer == NULL || size == 0)
        return 0;
    return (dmdrvi_ssize_t)dmusbserial_port_read((uint8_t *)buffer, size);
}

dmod_dmdrvi_dif_api_declaration(2.0, dmusbserial, dmdrvi_ssize_t, _write, ( dmdrvi_context_t context, void* handle, const void* buffer, size_t size, dmdrvi_offset_t offset ))
{
    (void)handle;
    if (offset < 0) return -EINVAL;
    if (size > (size_t)INT64_MAX) return -EOVERFLOW;
    if (!is_valid_context(context) || buffer == NULL || size == 0)
        return 0;

    /* Nobody reads on the host: after a write that timed out, the bytes are dropped at once (no wait per call) until the
       host takes data again. The caller is told everything was written, so a console is not stuck without a host. */
    uint32_t timeout = context->connected ? context->config.tx_timeout_ms : 0U;
    size_t done = dmusbserial_port_write((const uint8_t *)buffer, size, timeout);
    if (done < size)
    {
        context->connected = false;
        context->dropped += (uint32_t)(size - done);
    }
    else
    {
        context->connected = true;
    }
    return (dmdrvi_ssize_t)size;
}

dmod_dmdrvi_dif_api_declaration(2.0, dmusbserial, int, _ioctl, ( dmdrvi_context_t context, void* handle, int command, void* arg ))
{
    (void)handle;
    if (!is_valid_context(context))
    {
        DMOD_LOG_ERROR("Invalid DMDRVI context in dmusbserial_dmdrvi_ioctl\n");
        return -EINVAL;
    }

    /* Not ours (dmdevfs probes every node with the standard commands): dmdrvi's "not implemented" answer */
    if (command < DMUSBSERIAL_IOCTL_BASE || command >= dmusbserial_ioctl_cmd_max)
        return -ENOTTY;
    if (arg == NULL)
        return -EINVAL;

    switch (command)
    {
        case dmusbserial_ioctl_cmd_get_connected:
            *(int *)arg = context->connected ? 1 : 0;
            return 0;
        case dmusbserial_ioctl_cmd_get_dropped:
            *(uint32_t *)arg = context->dropped;
            return 0;
        default:
            return -ENOTTY;
    }
}

dmod_dmdrvi_dif_api_declaration(2.0, dmusbserial, int, _flush, ( dmdrvi_context_t context, void* handle ))
{
    (void)handle;
    if (!is_valid_context(context))
        return -EINVAL;
    return dmusbserial_port_flush(context->connected ? context->config.tx_timeout_ms : 0U);
}

dmod_dmdrvi_dif_api_declaration(2.0, dmusbserial, int, _stat, ( dmdrvi_context_t context, const char* path, dmdrvi_stat_t* stat ))
{
    (void)path;
    if (!is_valid_context(context) || stat == NULL)
        return -EINVAL;
    stat->size = (dmdrvi_size_t)0;      /* a stream */
    stat->mode = 0666;
    return 0;
}
