/*
 * dev_p1d.c — P1D reader/writer serial ASCII driver
 *
 * 9600 baud, /dev/ttyUSB0 default.
 * Based on p1d_rfid.c by Benjamin Larsson (2014).
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

static const uint8_t CMD_ST0[4] = {'S','T','0','\r'};
static const uint8_t CMD_ST1[4] = {'S','T','1','\r'};
static const uint8_t CMD_RSD[4] = {'R','S','D','\r'};
static const uint8_t CMD_SCB[4] = {'S','C','B','\r'};
static const uint8_t CMD_SRD[4] = {'S','R','D','\r'};
static const uint8_t CMD_SRA[4] = {'S','R','A','\r'};
static const uint8_t CMD_LTG[4] = {'L','T','G','\r'};

typedef struct { int fd; } p1d_priv_t;

static inline p1d_priv_t *priv(prokhz_dev_t *d)
{
    return (p1d_priv_t *)d->priv;
}

static prokhz_err_t tty_open(const char *path, int *fd_out)
{
    struct termios tio;
    int fd = open(path, O_RDWR | O_NONBLOCK);
    if (fd < 0) {
        fprintf(stderr, "[p1d] cannot open %s: %s\n", path, strerror(errno));
        return PROKHZ_ERR_NODEV;
    }
    memset(&tio, 0, sizeof tio);
    tio.c_cflag = CS8 | CREAD | CLOCAL;
    tio.c_cc[VMIN] = 1; tio.c_cc[VTIME] = 5;
    cfsetospeed(&tio, B9600); cfsetispeed(&tio, B9600);
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

static prokhz_err_t p1d_read(prokhz_dev_t *dev, prokhz_tag_id_t *id)
{
    uint8_t resp[BUFSZ] = {0};
    int cnt, retry = 1, i;

retry:
    tty_xfer(priv(dev)->fd, CMD_ST0, sizeof CMD_ST0, resp, BUFSZ);
    memset(resp, 0, BUFSZ);
    cnt = tty_xfer(priv(dev)->fd, CMD_RSD, sizeof CMD_RSD, resp, BUFSZ);
    if (cnt < 0) return PROKHZ_ERR_IO;
    if (resp[0] == '?') {
        if (retry--) goto retry;
        return PROKHZ_ERR_NOTAG;
    }
    for (i = 0; i < PROKHZ_TAG_ID_LEN; i++) {
        unsigned v = 0;
        char tmp[3] = { (char)resp[i*2], (char)resp[i*2+1], '\0' };
        if (sscanf(tmp, "%02x", &v) != 1) return PROKHZ_ERR_IO;
        id->bytes[i] = (uint8_t)v;
    }
    id->type = PROKHZ_TAG_EM4100;
    return PROKHZ_OK;
}

static prokhz_err_t p1d_write(prokhz_dev_t *dev,
                               const uint8_t id[PROKHZ_TAG_ID_LEN],
                               prokhz_tag_type_t tag_type)
{
    uint8_t cmd[20]={0}, resp[BUFSZ]={0};
    int i;

    if (tag_type == PROKHZ_TAG_T5577)
        tty_xfer(priv(dev)->fd, CMD_ST1, sizeof CMD_ST1, resp, BUFSZ);
    else
        tty_xfer(priv(dev)->fd, CMD_ST0, sizeof CMD_ST0, resp, BUFSZ);

    memset(resp, 0, BUFSZ);
    tty_xfer(priv(dev)->fd, CMD_SCB, sizeof CMD_SCB, resp, BUFSZ);
    if (resp[0] == '?') return PROKHZ_ERR_NOTAG;

    cmd[0]='W'; cmd[1]='E'; cmd[2]='P';
    for (i = 0; i < PROKHZ_TAG_ID_LEN; i++)
        snprintf((char *)&cmd[3+i*2], 3, "%02X", id[i]);
    cmd[13] = '\r';
    tty_xfer(priv(dev)->fd, cmd, 14, resp, BUFSZ);
    tty_xfer(priv(dev)->fd, CMD_SRD, sizeof CMD_SRD, resp, BUFSZ);
    tty_xfer(priv(dev)->fd, CMD_SRA, sizeof CMD_SRA, resp, BUFSZ);

    if (dev->verbose) {
        prokhz_tag_id_t verify = {0};
        if (p1d_read(dev, &verify) == PROKHZ_OK) {
            char buf[32];
            prokhz_format_id(verify.bytes, PROKHZ_FMT_HEX, buf, sizeof buf);
            fprintf(stdout, "[p1d] readback: %s\n", buf);
        }
    }
    return PROKHZ_OK;
}

static void p1d_close(prokhz_dev_t *dev)
{
    p1d_priv_t *p = priv(dev);
    if (p) { if (p->fd >= 0) close(p->fd); free(p); dev->priv = NULL; }
}

static prokhz_err_t p1d_open(prokhz_dev_t *dev, const prokhz_opts_t *opts)
{
    const char *path = opts->device ? opts->device : DEFAULT_TTY;
    p1d_priv_t *p    = calloc(1, sizeof *p);
    prokhz_err_t rc;
    uint8_t resp[BUFSZ] = {0};
    int cnt;

    if (!p) return PROKHZ_ERR_IO;

    rc = tty_open(path, &p->fd);
    if (rc != PROKHZ_OK) { free(p); return rc; }

    /*
     * Use the Locate Transponder command (LTG) as a probe.
     * A P1D device responds with either "OK" (tag present) or "?1"
     * (no tag). Any other response means this is not a P1D device.
     */
    cnt = tty_xfer(p->fd, CMD_LTG, sizeof CMD_LTG, resp, BUFSZ);
    if (cnt < 2) {
        if (opts->verbose)
            fprintf(stderr, "[p1d] no response to LTG on %s\n", path);
        close(p->fd); free(p);
        return PROKHZ_ERR_NODEV;
    }

    if (strncmp((char *)resp, "OK", 2) != 0 &&
        strncmp((char *)resp, "?1", 2) != 0) {
        if (opts->verbose)
            fprintf(stderr, "[p1d] unexpected LTG response: '%.*s'\n",
                    cnt, resp);
        close(p->fd); free(p);
        return PROKHZ_ERR_NODEV;
    }

    if (opts->verbose)
        fprintf(stdout, "[p1d] found P1D device on %s (LTG: %s)\n",
                path, strncmp((char *)resp, "OK", 2) == 0 ?
                "tag present" : "no tag");

    dev->priv = p;
    return PROKHZ_OK;
}

static const prokhz_ops_t p1d_ops = {
    .open  = p1d_open,
    .read  = p1d_read,
    .write = p1d_write,
    .beep  = NULL,
    .close = p1d_close,
};

const prokhz_device_t prokhz_device_p1d = {
    .name = "p1d",
    .desc = "P1D reader/writer, serial ASCII, 9600 baud",
    .ops  = &p1d_ops,
};
