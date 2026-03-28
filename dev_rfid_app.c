/*
 * dev_rfid_app.c — "ID card reader & writer6" serial binary driver
 *
 * 38400 baud, /dev/ttyUSB0 default.
 * Based on rfid_app.c by Benjamin Larsson (2014).
 */

#include "prokhz.h"
#include "prokhz_device.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <termios.h>
#include <errno.h>

#define DEFAULT_TTY PROKHZ_DEFAULT_TTY
#define BUFSZ       100

static const uint8_t CMD_READ[7] = {0xAA,0xDD,0x00,0x03, 0x01,0x0C,0x0D};
static const uint8_t CMD_BEEP[8] = {0xAA,0xDD,0x00,0x04, 0x01,0x03,0x0A,0x08};

typedef struct { int fd; } rfid_app_priv_t;

static inline rfid_app_priv_t *priv(prokhz_dev_t *d)
{
    return (rfid_app_priv_t *)d->priv;
}

static prokhz_err_t tty_open(const char *path, int *fd_out)
{
    struct termios tio;
    int fd = open(path, O_RDWR | O_NONBLOCK);
    if (fd < 0) {
        fprintf(stderr, "[rfid_app] cannot open %s: %s\n", path, strerror(errno));
        return PROKHZ_ERR_NODEV;
    }
    memset(&tio, 0, sizeof tio);
    tio.c_cflag = CS8 | CREAD | CLOCAL;
    tio.c_cc[VMIN] = 1; tio.c_cc[VTIME] = 5;
    cfsetospeed(&tio, B38400); cfsetispeed(&tio, B38400);
    tcsetattr(fd, TCSANOW, &tio);
    *fd_out = fd;
    return PROKHZ_OK;
}

static int tty_xfer(int fd, const uint8_t *cmd, int clen,
                    uint8_t *resp, int rmax)
{
    if (write(fd, cmd, clen) != clen) return -1;
    sleep(1);
    return (int)read(fd, resp, rmax);
}

static void destuff(const uint8_t *raw, int cnt, uint8_t *dst)
{
    int si = 7, di = 7;
    while (si < cnt) {
        dst[di] = raw[si];
        if (raw[si] == 0xAA && si+1 < cnt && raw[si+1] == 0x00) si++;
        si++; di++;
    }
}

static int stuff(const uint8_t *src, int len, uint8_t *dst)
{
    int si, di = 0;
    for (si = 0; si < len; si++) {
        if (src[si] == 0xAA) dst[di++] = 0xAA;
        dst[di++] = src[si];
    }
    return di;
}

static prokhz_err_t rfid_app_beep(prokhz_dev_t *dev, uint8_t duration)
{
    (void)duration;
    uint8_t resp[BUFSZ];
    tty_xfer(priv(dev)->fd, CMD_BEEP, sizeof CMD_BEEP, resp, BUFSZ);
    return PROKHZ_OK;
}

static prokhz_err_t rfid_app_read(prokhz_dev_t *dev, prokhz_tag_id_t *id)
{
    uint8_t raw[BUFSZ]={0}, pkt[BUFSZ]={0};
    int cnt, i;

    if (dev->beep) rfid_app_beep(dev, 1);

    cnt = tty_xfer(priv(dev)->fd, CMD_READ, sizeof CMD_READ, raw, BUFSZ);
    if (cnt < 13)       return PROKHZ_ERR_IO;
    if (raw[6] == 0x01) return PROKHZ_ERR_NOTAG;

    uint8_t csum = prokhz_xor_checksum(&raw[4], (size_t)(cnt - 5));
    if (csum != raw[cnt-1]) {
        if (dev->verbose)
            fprintf(stderr, "[rfid_app] checksum: got 0x%02X expected 0x%02X\n",
                    csum, raw[cnt-1]);
        return PROKHZ_ERR_CHECKSUM;
    }
    destuff(raw, cnt, pkt);
    for (i = 0; i < PROKHZ_TAG_ID_LEN; i++) id->bytes[i] = pkt[7+i];
    id->type = PROKHZ_TAG_EM4100;
    return PROKHZ_OK;
}

static int build_write_frame(uint8_t *out, uint8_t mode,
                              const uint8_t id[PROKHZ_TAG_ID_LEN])
{
    uint8_t pl[8], stuffed[20];
    int slen, idx;
    pl[0]=mode; pl[1]=0x0C; pl[2]=0x00;
    memcpy(pl+3, id, PROKHZ_TAG_ID_LEN);
    uint8_t csum = prokhz_xor_checksum(pl, 8);
    slen = stuff(pl, 8, stuffed);
    out[0]=0xAA; out[1]=0xDD; out[2]=0x00; out[3]=0x09;
    memcpy(out+4, stuffed, slen);
    idx = 4+slen;
    out[idx++] = csum;
    if (csum == 0xAA) out[idx++] = 0xAA;
    return idx;
}

static prokhz_err_t rfid_app_write(prokhz_dev_t *dev,
                                    const uint8_t id[PROKHZ_TAG_ID_LEN],
                                    prokhz_tag_type_t tag_type)
{
    (void)tag_type;
    uint8_t frame[BUFSZ], resp[BUFSZ];
    int flen;

    if (dev->beep) rfid_app_beep(dev, 1);
    flen = build_write_frame(frame, 0x03, id);
    tty_xfer(priv(dev)->fd, frame, flen, resp, BUFSZ);
    flen = build_write_frame(frame, 0x02, id);
    tty_xfer(priv(dev)->fd, frame, flen, resp, BUFSZ);

    prokhz_tag_id_t verify = {0};
    prokhz_err_t rc = rfid_app_read(dev, &verify);
    if (rc == PROKHZ_OK && dev->verbose) {
        char buf[32];
        prokhz_format_id(verify.bytes, PROKHZ_FMT_HEX, buf, sizeof buf);
        fprintf(stdout, "[rfid_app] readback: %s\n", buf);
    }
    return rc;
}

static void rfid_app_close(prokhz_dev_t *dev)
{
    rfid_app_priv_t *p = priv(dev);
    if (p) { if (p->fd >= 0) close(p->fd); free(p); dev->priv = NULL; }
}

/* Expected device identity string from the firmware version response */
#define RFID_APP_DEVICE_ID "ID card reader & writer6"

static prokhz_err_t rfid_app_open(prokhz_dev_t *dev, const prokhz_opts_t *opts)
{
    static const uint8_t id_cmd[7] = {0xAA,0xDD,0x00,0x03, 0x01,0x02,0x03};
    const char      *path = opts->device ? opts->device : DEFAULT_TTY;
    rfid_app_priv_t *p    = calloc(1, sizeof *p);
    prokhz_err_t     rc;
    uint8_t          resp[BUFSZ] = {0};
    int              cnt;

    if (!p) return PROKHZ_ERR_IO;

    rc = tty_open(path, &p->fd);
    if (rc != PROKHZ_OK) { free(p); return rc; }

    /*
     * Send the firmware version query and check the response for the
     * expected device identity string starting at byte offset 7.
     * If it does not match, this is not the right device.
     */
    cnt = tty_xfer(p->fd, id_cmd, sizeof id_cmd, resp, BUFSZ);
    if (cnt < 8) {
        if (opts->verbose)
            fprintf(stderr, "[rfid_app] no response to id query on %s\n", path);
        close(p->fd); free(p);
        return PROKHZ_ERR_NODEV;
    }

    if (strncmp((char *)&resp[7], RFID_APP_DEVICE_ID,
                strlen(RFID_APP_DEVICE_ID)) != 0) {
        if (opts->verbose)
            fprintf(stderr, "[rfid_app] unexpected device id: '%s'\n",
                    (char *)&resp[7]);
        close(p->fd); free(p);
        return PROKHZ_ERR_NODEV;
    }

    if (opts->verbose)
        fprintf(stdout, "[rfid_app] found: %s\n", RFID_APP_DEVICE_ID);

    dev->priv = p;
    return PROKHZ_OK;
}

static const prokhz_ops_t rfid_app_ops = {
    .open  = rfid_app_open,
    .read  = rfid_app_read,
    .write = rfid_app_write,
    .beep  = rfid_app_beep,
    .close = rfid_app_close,
};

const prokhz_device_t prokhz_device_rfid_app = {
    .name = "rfid-app",
    .desc = "ID card reader & writer6, serial binary, 38400 baud",
    .ops  = &rfid_app_ops,
};
