/*
 * prokhz_device.h — driver registration and internal device API
 *
 * This header is used by:
 *   - each dev_*.c file  (to declare its prokhz_device_t and ops)
 *   - prokhz_common.c    (to own the registry and implement probe/open)
 *
 * It is NOT installed as a public header alongside prokhz.h.
 *
 * Adding a new driver
 * ───────────────────
 * 1. Create dev_mydevice.c.  Include this header.
 * 2. Implement open/read/write/beep/close and define a prokhz_device_t.
 * 3. Add an extern declaration and a prokhz_device_register() call in
 *    prokhz_device_register_all() inside prokhz_common.c.
 * 4. Add dev_mydevice.c to the Makefile.
 */

#ifndef PROKHZ_DEVICE_H
#define PROKHZ_DEVICE_H

#include "prokhz.h"

/* ── concrete device handle layout ───────────────────────────────────── */
/*
 * prokhz_dev_t is opaque in prokhz.h.  The full definition lives here so
 * device files can access ->priv and ->opts fields via a direct cast.
 *
 * IMPORTANT: field order must never change — device files cast
 * prokhz_dev_t * directly to dev_base_t *.
 */

typedef struct prokhz_device prokhz_device_t;   /* forward */

struct prokhz_dev {
    const prokhz_device_t *device;   /* back-pointer to driver descriptor */
    void                  *priv;     /* driver-private heap state         */
    int                    beep;
    int                    verbose;
    int                    timeout_ms;
};

/* Convenience accessor used by every device file */
static inline struct prokhz_dev *dev_base(prokhz_dev_t *d)
{
    return (struct prokhz_dev *)d;
}


/* ── shared device constants ──────────────────────────────────────────── */

/* Default TTY path used by serial drivers when opts->device is NULL */
#define PROKHZ_DEFAULT_TTY "/dev/ttyUSB0"


/* ── driver operations vtable ─────────────────────────────────────────── */

typedef struct {
    /**
     * Try to open the physical device described by opts.
     * Sets dev->priv on success.  Returns PROKHZ_OK or PROKHZ_ERR_NODEV.
     * This function doubles as the probe function: prokhz_probe() calls
     * open() on each registered driver and uses the first that succeeds.
     */
    prokhz_err_t (*open) (prokhz_dev_t *dev, const prokhz_opts_t *opts);

    prokhz_err_t (*read) (prokhz_dev_t *dev, prokhz_tag_id_t *id);

    prokhz_err_t (*write)(prokhz_dev_t *dev,
                          const uint8_t id[PROKHZ_TAG_ID_LEN],
                          prokhz_tag_type_t tag_type);

    /** NULL if device has no buzzer; prokhz_beep() returns PROKHZ_ERR_NOTSUP. */
    prokhz_err_t (*beep) (prokhz_dev_t *dev, uint8_t duration);

    void         (*close)(prokhz_dev_t *dev);
} prokhz_ops_t;

/* ── driver descriptor ────────────────────────────────────────────────── */

struct prokhz_device {
    const char          *name;  /* short identifier, e.g. "ctx203"  */
    const char          *desc;  /* human-readable description        */
    const prokhz_ops_t  *ops;
};

/* ── registry API ─────────────────────────────────────────────────────── */

#define PROKHZ_MAX_DRIVERS 16

void                    prokhz_device_register(const prokhz_device_t *dev);
void                    prokhz_device_register_all(void);
int                     prokhz_device_count(void);
const prokhz_device_t  *prokhz_device_get(int index);
const prokhz_device_t  *prokhz_device_find(const char *name);

#endif /* PROKHZ_DEVICE_H */
