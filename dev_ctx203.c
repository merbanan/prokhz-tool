/*
 * dev_ctx203.c — CTX 203-ID-RW USB HID interrupt transfer driver
 *
 * VID 0x6688 / PID 0x6850
 * Based on ctx203_rfid.c by Benjamin Larsson (2017).
 */

#include "prokhz.h"
#include "prokhz_device.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <libusb-1.0/libusb.h>

#define VID 0x6688
#define PID 0x6850

#define EP_OUT      0x03
#define EP_IN       0x85
#define PKT_OUT_SZ  24
#define PKT_IN_SZ   48
#define MSG_START   0x01
#define MSG_END     0x04
#define MSG_OVERHEAD 5

#define CMD_EM4100_READ 0x10
#define CMD_T5577_WRITE 0x12
#define CMD_EM4305_CMD  0x13
#define CMD_BUZZER      0x03

static const uint8_t CFG_T5577[4]  = {0x00,0x14,0x80,0x41};
static const uint8_t CFG_EM4305[4] = {0xFA,0x01,0x80,0x00};

typedef struct {
    libusb_context       *ctx;
    libusb_device_handle *devh;
    int                   kernel_detached;
} ctx203_priv_t;

static inline ctx203_priv_t *priv(prokhz_dev_t *d)
{
    return (ctx203_priv_t *)d->priv;
}

static void build_packet(uint8_t *buf, uint8_t cmd,
                          const uint8_t *payload, int plen)
{
    memset(buf, 0, PKT_OUT_SZ);
    buf[0]=EP_OUT; buf[1]=MSG_START;
    buf[2]=(uint8_t)(MSG_OVERHEAD+plen);
    buf[3]=cmd;
    if (payload && plen > 0) memcpy(&buf[4], payload, (size_t)plen);
    buf[4+plen]   = prokhz_xor_checksum(&buf[1], (size_t)(3+plen));
    buf[4+plen+1] = MSG_END;
}

static prokhz_err_t usb_xfer(prokhz_dev_t *dev,
                               const uint8_t *out, uint8_t *in)
{
    ctx203_priv_t *p = priv(dev);
    int tms=dev->timeout_ms, r, bt=0;
    libusb_interrupt_transfer(p->devh, EP_IN, in, PKT_IN_SZ, &bt, 50);
    r = libusb_interrupt_transfer(p->devh, EP_OUT,
                                  (unsigned char *)out, PKT_OUT_SZ, &bt, tms);
    if (r < 0) {
        if (dev->verbose) fprintf(stderr,"[ctx203] OUT: %s\n",libusb_error_name(r));
        return r==LIBUSB_ERROR_TIMEOUT ? PROKHZ_ERR_TIMEOUT : PROKHZ_ERR_IO;
    }
    usleep(60*1000);
    r = libusb_interrupt_transfer(p->devh, EP_IN, in, PKT_IN_SZ, &bt, tms);
    if (r < 0) {
        if (dev->verbose) fprintf(stderr,"[ctx203] IN: %s\n",libusb_error_name(r));
        return r==LIBUSB_ERROR_TIMEOUT ? PROKHZ_ERR_TIMEOUT : PROKHZ_ERR_IO;
    }
    return PROKHZ_OK;
}

static prokhz_err_t t5577_block_write(prokhz_dev_t *dev, int block,
                                       const uint8_t *data4)
{
    uint8_t pkt[PKT_OUT_SZ], resp[PKT_IN_SZ]={0};
    uint8_t pl[7]={0x04,0x00,data4[0],data4[1],data4[2],data4[3],(uint8_t)block};
    build_packet(pkt, CMD_T5577_WRITE, pl, 7);
    return usb_xfer(dev, pkt, resp);
}

static prokhz_err_t t5577_reset(prokhz_dev_t *dev)
{
    uint8_t pkt[PKT_OUT_SZ], resp[PKT_IN_SZ]={0}, pl[5]={0};
    build_packet(pkt, CMD_T5577_WRITE, pl, 5);
    return usb_xfer(dev, pkt, resp);
}

static prokhz_err_t em4305_login(prokhz_dev_t *dev)
{
    uint8_t pkt[PKT_OUT_SZ], resp[PKT_IN_SZ]={0};
    uint8_t pl[7]={0x03,0,0,0,0,0,0};
    build_packet(pkt, CMD_EM4305_CMD, pl, 7);
    return usb_xfer(dev, pkt, resp);
}

static prokhz_err_t em4305_write_word(prokhz_dev_t *dev, int word,
                                       const uint8_t *data4)
{
    uint8_t pkt[PKT_OUT_SZ], resp[PKT_IN_SZ]={0};
    uint8_t pl[7]={0x01,(uint8_t)word,data4[0],data4[1],data4[2],data4[3],0x00};
    build_packet(pkt, CMD_EM4305_CMD, pl, 7);
    return usb_xfer(dev, pkt, resp);
}

static prokhz_err_t ctx203_read(prokhz_dev_t *dev, prokhz_tag_id_t *id)
{
    uint8_t pkt[PKT_OUT_SZ], resp[PKT_IN_SZ];
    prokhz_err_t rc;
    int retry=10, data_len;
    build_packet(pkt, CMD_EM4100_READ, NULL, 0);
    do {
        memset(resp, 0, PKT_IN_SZ);
        rc = usb_xfer(dev, pkt, resp);
        if (rc != PROKHZ_OK) return rc;
        data_len = resp[2] - MSG_OVERHEAD - 1;
    } while (data_len < PROKHZ_TAG_ID_LEN && --retry > 0);
    if (data_len < PROKHZ_TAG_ID_LEN) return PROKHZ_ERR_NOTAG;
    memcpy(id->bytes, &resp[5], PROKHZ_TAG_ID_LEN);
    id->type = PROKHZ_TAG_EM4100;
    return PROKHZ_OK;
}

static prokhz_err_t ctx203_write(prokhz_dev_t *dev,
                                  const uint8_t id[PROKHZ_TAG_ID_LEN],
                                  prokhz_tag_type_t tag_type)
{
    uint8_t bs[8];
    prokhz_err_t rc;
    prokhz_em4100_encode(id, bs);
    if (tag_type == PROKHZ_TAG_T5577) {
        if ((rc=t5577_block_write(dev,1,bs)))       return rc;
        if ((rc=t5577_block_write(dev,2,bs+4)))     return rc;
        if ((rc=t5577_block_write(dev,0,CFG_T5577)))return rc;
        if ((rc=t5577_reset(dev)))                  return rc;
        fprintf(stdout, "OK\n");
    } else if (tag_type == PROKHZ_TAG_EM4305) {
        if ((rc=em4305_login(dev)))                   return rc;
        if ((rc=em4305_write_word(dev,5,bs)))         return rc;
        if ((rc=em4305_write_word(dev,6,bs+4)))       return rc;
        if ((rc=em4305_write_word(dev,4,CFG_EM4305))) return rc;
        usleep(200*1000);
        prokhz_tag_id_t verify={0};
        if (ctx203_read(dev, &verify) == PROKHZ_OK) {
            char buf[32];
            prokhz_format_id(verify.bytes, PROKHZ_FMT_HEX, buf, sizeof buf);
            fprintf(stdout, "%s\n", buf);
        }
    } else {
        return PROKHZ_ERR_NOTSUP;
    }
    return PROKHZ_OK;
}

static prokhz_err_t ctx203_beep(prokhz_dev_t *dev, uint8_t duration)
{
    uint8_t pkt[PKT_OUT_SZ], resp[PKT_IN_SZ]={0};
    uint8_t pl[1]={(duration<1)?1:(duration>9)?9:duration};
    build_packet(pkt, CMD_BUZZER, pl, 1);
    return usb_xfer(dev, pkt, resp);
}

static void ctx203_close(prokhz_dev_t *dev)
{
    ctx203_priv_t *p = priv(dev);
    if (!p) return;
    if (p->devh) {
        libusb_release_interface(p->devh, 0);
        if (p->kernel_detached) libusb_attach_kernel_driver(p->devh, 0);
        libusb_close(p->devh);
    }
    if (p->ctx) libusb_exit(p->ctx);
    free(p); dev->priv = NULL;
}

static prokhz_err_t ctx203_open(prokhz_dev_t *dev, const prokhz_opts_t *opts)
{
    ctx203_priv_t *p = calloc(1, sizeof *p);
    int r;
    if (!p) return PROKHZ_ERR_IO;
    r = libusb_init(&p->ctx);
    if (r < 0) { free(p); return PROKHZ_ERR_NODEV; }
    if (opts->verbose)
        libusb_set_option(p->ctx, LIBUSB_OPTION_LOG_LEVEL,
                          LIBUSB_LOG_LEVEL_WARNING);
    p->devh = libusb_open_device_with_vid_pid(p->ctx, VID, PID);
    if (!p->devh) { libusb_exit(p->ctx); free(p); return PROKHZ_ERR_NODEV; }
    r = libusb_detach_kernel_driver(p->devh, 0);
    if (r == 0) p->kernel_detached = 1;
    else if (r != LIBUSB_ERROR_NOT_FOUND && r != LIBUSB_ERROR_NOT_SUPPORTED) {
        libusb_close(p->devh); libusb_exit(p->ctx); free(p);
        return PROKHZ_ERR_NODEV;
    }
    if (libusb_claim_interface(p->devh, 0) < 0) {
        libusb_close(p->devh); libusb_exit(p->ctx); free(p);
        return PROKHZ_ERR_NODEV;
    }
    { uint8_t tmp[PKT_IN_SZ]={0}; int bt=0;
      libusb_interrupt_transfer(p->devh, EP_IN, tmp, PKT_IN_SZ, &bt, 200); }
    usleep(300*1000);
    dev->priv = p;
    return PROKHZ_OK;
}

static const prokhz_ops_t ctx203_ops = {
    .open  = ctx203_open,
    .read  = ctx203_read,
    .write = ctx203_write,
    .beep  = ctx203_beep,
    .close = ctx203_close,
};

const prokhz_device_t prokhz_device_ctx203 = {
    .name = "ctx203",
    .desc = "CTX 203-ID-RW, USB HID interrupt transfers (EP 0x03/0x85)",
    .ops  = &ctx203_ops,
};
