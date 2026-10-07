/*
 * prokhz_common.c — driver registry, probe/open dispatch, shared utilities
 */

#include "prokhz.h"
#include "prokhz_device.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ══════════════════════════════════════════════════════════════════════════
 * Driver registry
 * ══════════════════════════════════════════════════════════════════════════ */

static const prokhz_device_t *registry[PROKHZ_MAX_DRIVERS];
static int                    registry_count = 0;
static int                    registry_ready = 0;

void prokhz_device_register(const prokhz_device_t *dev)
{
    if (!dev || registry_count >= PROKHZ_MAX_DRIVERS) return;
    registry[registry_count++] = dev;
}

int prokhz_device_count(void)                  { return registry_count; }

const prokhz_device_t *prokhz_device_get(int index)
{
    if (index < 0 || index >= registry_count) return NULL;
    return registry[index];
}

const prokhz_device_t *prokhz_device_find(const char *name)
{
    int i;
    if (!name) return NULL;
    for (i = 0; i < registry_count; i++)
        if (strcmp(registry[i]->name, name) == 0)
            return registry[i];
    return NULL;
}

/* ── built-in driver descriptors ──────────────────────────────────────── */

extern const prokhz_device_t prokhz_device_ctx203;
extern const prokhz_device_t prokhz_device_idrw;
extern const prokhz_device_t prokhz_device_tcc;
extern const prokhz_device_t prokhz_device_rfid_app;
extern const prokhz_device_t prokhz_device_p1d;

void prokhz_device_register_all(void)
{
    prokhz_device_register(&prokhz_device_ctx203);
    prokhz_device_register(&prokhz_device_idrw);
    prokhz_device_register(&prokhz_device_tcc);
    prokhz_device_register(&prokhz_device_rfid_app);
    prokhz_device_register(&prokhz_device_p1d);
}

static void ensure_registered(void)
{
    if (!registry_ready) {
        prokhz_device_register_all();
        registry_ready = 1;
    }
}

/* ══════════════════════════════════════════════════════════════════════════
 * prokhz_probe / prokhz_open
 * ══════════════════════════════════════════════════════════════════════════ */

static prokhz_dev_t *alloc_dev(const prokhz_device_t *drv,
                                const prokhz_opts_t   *opts)
{
    prokhz_dev_t *dev = calloc(1, sizeof *dev);
    if (!dev) return NULL;
    dev->device     = drv;
    dev->beep       = opts->beep;
    dev->verbose    = opts->verbose;
    dev->timeout_ms = opts->timeout_ms > 0 ? opts->timeout_ms : 1000;
    return dev;
}

prokhz_err_t prokhz_probe(const prokhz_opts_t *opts, prokhz_dev_t **out)
{
    int i;
    prokhz_err_t rc = PROKHZ_ERR_NODEV;

    if (!opts || !out) return PROKHZ_ERR_PARAM;
    ensure_registered();

    for (i = 0; i < registry_count; i++) {
        const prokhz_device_t *drv = registry[i];
        prokhz_dev_t          *dev = alloc_dev(drv, opts);
        if (!dev) return PROKHZ_ERR_IO;

        if (opts->verbose)
            fprintf(stderr, "[prokhz] probing %s (%s)...\n",
                    drv->name, drv->desc);

        rc = drv->ops->open(dev, opts);
        if (rc == PROKHZ_OK) {
            if (opts->verbose)
                fprintf(stderr, "[prokhz] opened %s\n", drv->name);
            *out = dev;
            return PROKHZ_OK;
        }
        free(dev);

        if (opts->verbose)
            fprintf(stderr, "[prokhz] %s: %s\n",
                    drv->name, prokhz_strerror(rc));
    }

    fprintf(stderr, "[prokhz] no device found\n");
    return PROKHZ_ERR_NODEV;
}

prokhz_err_t prokhz_open(const char *driver_name,
                          const prokhz_opts_t *opts, prokhz_dev_t **out)
{
    const prokhz_device_t *drv;
    prokhz_dev_t          *dev;
    prokhz_err_t           rc;

    if (!driver_name || !opts || !out) return PROKHZ_ERR_PARAM;
    ensure_registered();

    drv = prokhz_device_find(driver_name);
    if (!drv) {
        fprintf(stderr, "[prokhz] unknown driver '%s'\n", driver_name);
        return PROKHZ_ERR_PARAM;
    }

    dev = alloc_dev(drv, opts);
    if (!dev) return PROKHZ_ERR_IO;

    rc = drv->ops->open(dev, opts);
    if (rc != PROKHZ_OK) { free(dev); return rc; }

    *out = dev;
    return PROKHZ_OK;
}

/* ── remaining public API ─────────────────────────────────────────────── */

const char *prokhz_driver_name(const prokhz_dev_t *dev)
{
    if (!dev || !dev->device) return "(none)";
    return dev->device->name;
}

prokhz_err_t prokhz_read(prokhz_dev_t *dev, prokhz_tag_id_t *id)
{
    if (!dev || !id)              return PROKHZ_ERR_PARAM;
    if (!dev->device->ops->read)  return PROKHZ_ERR_NOTSUP;
    memset(id, 0, sizeof *id);
    return dev->device->ops->read(dev, id);
}

prokhz_err_t prokhz_write(prokhz_dev_t *dev,
                           const uint8_t id[PROKHZ_TAG_ID_LEN],
                           prokhz_tag_type_t tag_type)
{
    if (!dev || !id)              return PROKHZ_ERR_PARAM;
    if (!dev->device->ops->write) return PROKHZ_ERR_NOTSUP;
    return dev->device->ops->write(dev, id, tag_type);
}

prokhz_err_t prokhz_beep(prokhz_dev_t *dev, uint8_t duration)
{
    if (!dev)                     return PROKHZ_ERR_PARAM;
    if (!dev->device->ops->beep)  return PROKHZ_ERR_NOTSUP;
    return dev->device->ops->beep(dev, duration);
}

void prokhz_close(prokhz_dev_t *dev)
{
    if (!dev) return;
    if (dev->device->ops->close)
        dev->device->ops->close(dev);
    free(dev);
}

/* ══════════════════════════════════════════════════════════════════════════
 * Shared utilities
 * ══════════════════════════════════════════════════════════════════════════ */

const char *prokhz_strerror(prokhz_err_t err)
{
    switch (err) {
    case PROKHZ_OK:           return "success";
    case PROKHZ_ERR_NOTAG:    return "no tag in field";
    case PROKHZ_ERR_IO:       return "I/O error";
    case PROKHZ_ERR_CHECKSUM: return "checksum mismatch";
    case PROKHZ_ERR_NODEV:    return "device not found";
    case PROKHZ_ERR_PARAM:    return "invalid parameter";
    case PROKHZ_ERR_NOTSUP:   return "not supported";
    case PROKHZ_ERR_TIMEOUT:  return "timeout";
    default:                  return "unknown error";
    }
}

prokhz_err_t prokhz_parse_hex_id(const char *hex, uint8_t out[PROKHZ_TAG_ID_LEN])
{
    int i;
    if (!hex || !out || strlen(hex) != 10) return PROKHZ_ERR_PARAM;
    for (i = 0; i < PROKHZ_TAG_ID_LEN; i++) {
        unsigned v = 0;
        char tmp[3] = { hex[i*2], hex[i*2+1], '\0' };
        if (sscanf(tmp, "%02x", &v) != 1) return PROKHZ_ERR_PARAM;
        out[i] = (uint8_t)v;
    }
    return PROKHZ_OK;
}

prokhz_err_t prokhz_format_id(const uint8_t id[PROKHZ_TAG_ID_LEN],
                               prokhz_format_t fmt, char *buf, size_t bufsz)
{
    if (!id || !buf || bufsz < 16) return PROKHZ_ERR_PARAM;
    switch (fmt) {
    case PROKHZ_FMT_HEX:
        snprintf(buf, bufsz, "%02X%02X%02X%02X%02X",
                 id[0], id[1], id[2], id[3], id[4]);
        break;
    case PROKHZ_FMT_HEX_SPACED:
        snprintf(buf, bufsz, "%02X %02X %02X %02X %02X",
                 id[0], id[1], id[2], id[3], id[4]);
        break;
    case PROKHZ_FMT_DEC_8H: {
        uint32_t v = ((uint32_t)id[1] << 24) | ((uint32_t)id[2] << 16)
                   | ((uint32_t)id[3] <<  8) |  (uint32_t)id[4];
        snprintf(buf, bufsz, "%010u", v);
        break;
    }
    case PROKHZ_FMT_DEC_6H: {
        uint32_t v = ((uint32_t)id[2] << 16) | ((uint32_t)id[3] << 8)
                   |  (uint32_t)id[4];
        snprintf(buf, bufsz, "%010u", v);
        break;
    }
    case PROKHZ_FMT_WIEGAND:
        snprintf(buf, bufsz, "%03u,%05u", (unsigned)id[2],
                 (unsigned)((id[3] << 8) | id[4]));
        break;
    case PROKHZ_FMT_DEC_9: {
        uint64_t v = ((uint64_t)id[0] << 32) | ((uint64_t)id[1] << 24)
                   | ((uint64_t)id[2] << 16) | ((uint64_t)id[3] <<  8)
                   |  (uint64_t)id[4];
        snprintf(buf, bufsz, "%09llu", (unsigned long long)v);
        break;
    }
    case PROKHZ_FMT_DEC_8: {
        char tmp[16];
        uint64_t v = ((uint64_t)id[0] << 32) | ((uint64_t)id[1] << 24)
                   | ((uint64_t)id[2] << 16) | ((uint64_t)id[3] <<  8)
                   |  (uint64_t)id[4];
        snprintf(tmp, sizeof tmp, "%09llu", (unsigned long long)v);
        snprintf(buf, bufsz, "%s", tmp + 1);
        break;
    }
    default:
        return PROKHZ_ERR_PARAM;
    }
    return PROKHZ_OK;
}

uint8_t prokhz_xor_checksum(const uint8_t *buf, size_t len)
{
    uint8_t c = 0;
    size_t  i;
    for (i = 0; i < len; i++) c ^= buf[i];
    return c;
}

static int em4100_col_parity(const uint8_t *id, int shift)
{
    int i, p = 0;
    for (i = 0; i < 5; i++) {
        p += (id[i] >> (shift + 4)) & 1;
        p += (id[i] >>  shift)      & 1;
    }
    return p & 1;
}

void prokhz_em4100_encode(const uint8_t id[PROKHZ_TAG_ID_LEN], uint8_t out[8])
{
    static const uint8_t ep[16] = {0,1,1,0, 1,0,0,1, 1,0,0,1, 0,1,1,0};
    uint8_t p0=ep[id[0]>>4], p1=ep[id[0]&0xf];
    uint8_t p2=ep[id[1]>>4], p3=ep[id[1]&0xf];
    uint8_t p4=ep[id[2]>>4], p5=ep[id[2]&0xf];
    uint8_t p6=ep[id[3]>>4], p7=ep[id[3]&0xf];
    uint8_t p8=ep[id[4]>>4], p9=ep[id[4]&0xf];
    uint8_t pc0=(uint8_t)em4100_col_parity(id,3);
    uint8_t pc1=(uint8_t)em4100_col_parity(id,2);
    uint8_t pc2=(uint8_t)em4100_col_parity(id,1);
    uint8_t pc3=(uint8_t)em4100_col_parity(id,0);
    out[0] = 0xFF;
    out[1] = 0x80|((id[0]>>1)&0x78)|(p0<<2)|((id[0]>>2)&0x03);
    out[2] = (id[0]<<6)|(p1<<5)|((id[1]>>3)&0x1e)|p2;
    out[3] = (id[1]<<4)|(p3<<3)|(id[2]>>5);
    out[4] = ((id[2]<<3)&0x80)|(p4<<6)|((id[2]<<2)&0x3c)|(p5<<1)|(id[3]>>7);
    out[5] = ((id[3]<<1)&0xe0)|(p6<<4)|(id[3]&0x0f);
    out[6] = (p7<<7)|((id[4]>>1)&0x78)|(p8<<2)|((id[4]>>2)&0x03);
    out[7] = (id[4]<<6)|(p9<<5)|(pc0<<4)|(pc1<<3)|(pc2<<2)|(pc3<<1);
}
