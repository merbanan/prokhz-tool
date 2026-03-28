/*
 * prokhz.h — public API for libprokhz
 *
 * Unified library for 125 kHz RFID reader/writers.
 * Driver selection is automatic: prokhz_probe() tries every registered
 * driver and returns a handle for the first device that responds.
 *
 * Supported devices (registered in prokhz_device.h):
 *   dev_rfid_app.c  — "ID card reader & writer6", serial binary, 38400 baud
 *   dev_p1d.c       — P1D reader/writer, serial ASCII, 9600 baud
 *   dev_ctx203.c    — CTX 203-ID-RW, USB HID interrupt transfers
 *   dev_idrw.c      — generic USB Reader, USB HID control transfers
 *
 * Copyright (C) 2014-2017 Benjamin Larsson
 * Library refactor: MIT License — see LICENSE for details.
 */

#ifndef PROKHZ_H
#define PROKHZ_H

#include <stdint.h>
#include <stddef.h>

/* ── version ──────────────────────────────────────────────────────────── */
#define PROKHZ_VERSION_MAJOR 1
#define PROKHZ_VERSION_MINOR 0

/* ── tag types ────────────────────────────────────────────────────────── */
typedef enum {
    PROKHZ_TAG_EM4100 = 0,  /* read-only EM4100 / compatible clone */
    PROKHZ_TAG_T5577  = 1,  /* writable T5577                      */
    PROKHZ_TAG_EM4305 = 2,  /* writable EM4305                     */
} prokhz_tag_type_t;

/* ── output formats ───────────────────────────────────────────────────── */
typedef enum {
    PROKHZ_FMT_HEX        = 0,  /* "0104AABB11"    no spaces            */
    PROKHZ_FMT_HEX_SPACED = 1,  /* "01 04 AA BB 11"                     */
    PROKHZ_FMT_DEC_8H     = 2,  /* "0012345678"    bytes 1-4 as uint32  */
    PROKHZ_FMT_DEC_6H     = 3,  /* "0000012345"    bytes 2-4 as uint24  */
    PROKHZ_FMT_WIEGAND    = 4,  /* "003,12345"     facility,card        */
    PROKHZ_FMT_DEC_9      = 5,  /* "012345678"     9-digit decimal      */
    PROKHZ_FMT_DEC_8      = 6,  /* "12345678"      8-digit (Aptus)      */
} prokhz_format_t;

/* ── error codes ──────────────────────────────────────────────────────── */
typedef enum {
    PROKHZ_OK            =  0,
    PROKHZ_ERR_NOTAG     = -1,  /* no tag in field                       */
    PROKHZ_ERR_IO        = -2,  /* transport failure                     */
    PROKHZ_ERR_CHECKSUM  = -3,  /* packet checksum mismatch              */
    PROKHZ_ERR_NODEV     = -4,  /* device not found or failed to open    */
    PROKHZ_ERR_PARAM     = -5,  /* invalid argument                      */
    PROKHZ_ERR_NOTSUP    = -6,  /* operation not supported by driver     */
    PROKHZ_ERR_TIMEOUT   = -7,  /* operation timed out                   */
} prokhz_err_t;

/* ── tag ID ───────────────────────────────────────────────────────────── */
#define PROKHZ_TAG_ID_LEN 5

typedef struct {
    uint8_t           bytes[PROKHZ_TAG_ID_LEN];
    prokhz_tag_type_t type;
} prokhz_tag_id_t;

/* ── opaque device handle ─────────────────────────────────────────────── */
typedef struct prokhz_dev prokhz_dev_t;

/* ── open / probe options ─────────────────────────────────────────────── */
typedef struct {
    const char *device;      /* TTY path hint, or NULL (USB drivers ignore) */
    int         beep;        /* non-zero: beep on tag access                */
    int         verbose;     /* non-zero: debug output to stderr            */
    int         timeout_ms;  /* I/O timeout; 0 = driver default (1000 ms)  */
} prokhz_opts_t;

/* ── device lifecycle ─────────────────────────────────────────────────── */

/**
 * Probe all registered drivers and open the first that responds.
 * On success *out receives the device handle.
 * Returns PROKHZ_OK or PROKHZ_ERR_NODEV if nothing was found.
 */
prokhz_err_t prokhz_probe(const prokhz_opts_t *opts, prokhz_dev_t **out);

/**
 * Open a specific driver by name (as returned by prokhz_driver_name()).
 * Useful when multiple devices are connected simultaneously.
 */
prokhz_err_t prokhz_open(const char *driver_name,
                          const prokhz_opts_t *opts, prokhz_dev_t **out);

/** Return the name of the driver backing this handle (never NULL). */
const char *prokhz_driver_name(const prokhz_dev_t *dev);

/** Close the device and free the handle. */
void prokhz_close(prokhz_dev_t *dev);

/* ── tag operations ───────────────────────────────────────────────────── */

prokhz_err_t prokhz_read(prokhz_dev_t *dev, prokhz_tag_id_t *id);
prokhz_err_t prokhz_write(prokhz_dev_t *dev,
                           const uint8_t id[PROKHZ_TAG_ID_LEN],
                           prokhz_tag_type_t tag_type);
prokhz_err_t prokhz_beep(prokhz_dev_t *dev, uint8_t duration);

/* ── utilities ────────────────────────────────────────────────────────── */

prokhz_err_t prokhz_format_id(const uint8_t id[PROKHZ_TAG_ID_LEN],
                               prokhz_format_t fmt, char *buf, size_t bufsz);
prokhz_err_t prokhz_parse_hex_id(const char *hex,
                                  uint8_t out[PROKHZ_TAG_ID_LEN]);
void         prokhz_em4100_encode(const uint8_t id[PROKHZ_TAG_ID_LEN],
                                   uint8_t out[8]);
uint8_t      prokhz_xor_checksum(const uint8_t *buf, size_t len);
const char  *prokhz_strerror(prokhz_err_t err);

#endif /* PROKHZ_H */
