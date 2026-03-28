/*
 * dev_idrw.c — idrw USB HID control transfer driver
 *
 * VID 0xFFFF / PID 0x0035
 * Based on idrw_linux.c by Benjamin Larsson (2015).
 */

#include "prokhz.h"
#include "prokhz_device.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <libusb-1.0/libusb.h>

#define VID 0xFFFF
#define PID 0x0035

#define HID_GET_REPORT          0x01
#define HID_SET_REPORT          0x09
#define HID_REPORT_TYPE_FEATURE 0x03

#define CTRL_IN  (LIBUSB_ENDPOINT_IN  | LIBUSB_REQUEST_TYPE_CLASS | \
                  LIBUSB_RECIPIENT_INTERFACE)
#define CTRL_OUT (LIBUSB_ENDPOINT_OUT | LIBUSB_REQUEST_TYPE_CLASS | \
                  LIBUSB_RECIPIENT_INTERFACE)

#define PACKET_LEN  0x100
#define STX_OUT     0xAA
#define ETX_OUT     0xBB
#define STX_IN      0x02
#define ETX_IN      0x03
#define STATION_ID  0x00

static const uint8_t PKT_HDR[8] = {0x01,0x00,0x00,0x00,0x00,0x00,0x08,0x00};

#define CMD_GET_SNR 0x25
#define CMD_WRITE   0x21
#define CMD_BUZZER  0x89
#define TT_T5577    0x00
#define TT_EM4305   0x02

typedef struct {
    libusb_context       *ctx;
    libusb_device_handle *devh;
    int                   kernel_detached;
} idrw_priv_t;

static inline idrw_priv_t *priv(prokhz_dev_t *d)
{
    return (idrw_priv_t *)d->priv;
}

static void build_packet(uint8_t *buf, uint8_t cmd,
                          const uint8_t *payload, int plen)
{
    memset(buf, 0, PACKET_LEN);
    memcpy(buf, PKT_HDR, 8);
    buf[8]=STX_OUT; buf[9]=STATION_ID;
    buf[10]=(uint8_t)(plen+1+1);
    buf[11]=cmd;
    if (payload && plen > 0) memcpy(&buf[12], payload, (size_t)plen);
    buf[12+plen] = prokhz_xor_checksum(&buf[9], (size_t)(1+1+1+plen));
    buf[13+plen] = ETX_OUT;
}

static prokhz_err_t parse_response(const uint8_t *buf, uint8_t *data,
                                    int *data_len_out, int verbose)
{
    uint8_t pl_len, status, bcc, bcc_calc;
    int data_len;
    if (buf[8] != STX_IN) {
        if (verbose) fprintf(stderr,"[idrw] bad STX: 0x%02X\n",buf[8]);
        return PROKHZ_ERR_IO;
    }
    pl_len   = buf[10];
    status   = buf[11];
    data_len = (int)pl_len-1-1;
    if (data_len < 0) data_len = 0;
    if (buf[10+pl_len+1] != ETX_IN) {
        if (verbose) fprintf(stderr,"[idrw] bad ETX: 0x%02X\n",buf[10+pl_len+1]);
        return PROKHZ_ERR_IO;
    }
    bcc      = buf[10+pl_len];
    bcc_calc = prokhz_xor_checksum(&buf[9], (size_t)(1+pl_len));
    if (bcc != bcc_calc) {
        if (verbose) fprintf(stderr,"[idrw] BCC 0x%02X != 0x%02X\n",bcc,bcc_calc);
        return PROKHZ_ERR_CHECKSUM;
    }
    if (status == 0x01) return PROKHZ_ERR_NOTAG;
    if (status != 0x00) {
        if (verbose) fprintf(stderr,"[idrw] status 0x%02X\n",status);
        return PROKHZ_ERR_IO;
    }
    if (data && data_len > 0) memcpy(data, &buf[12], (size_t)data_len);
    if (data_len_out) *data_len_out = data_len;
    return PROKHZ_OK;
}

static prokhz_err_t ctrl_xfer(prokhz_dev_t *dev,
                               const uint8_t *out_pkt, uint8_t *in_pkt)
{
    idrw_priv_t *p=priv(dev);
    int tms=dev->timeout_ms, r;
    r = libusb_control_transfer(p->devh, CTRL_OUT, HID_SET_REPORT,
                                (HID_REPORT_TYPE_FEATURE<<8)|0x00, 0,
                                (unsigned char *)out_pkt, PACKET_LEN, tms);
    if (r < 0) {
        if (dev->verbose) fprintf(stderr,"[idrw] SET_REPORT: %s\n",libusb_error_name(r));
        return r==LIBUSB_ERROR_TIMEOUT ? PROKHZ_ERR_TIMEOUT : PROKHZ_ERR_IO;
    }
    r = libusb_control_transfer(p->devh, CTRL_IN, HID_GET_REPORT,
                                (HID_REPORT_TYPE_FEATURE<<8)|0x00, 0,
                                in_pkt, PACKET_LEN, tms);
    if (r < 0) {
        if (dev->verbose) fprintf(stderr,"[idrw] GET_REPORT: %s\n",libusb_error_name(r));
        return r==LIBUSB_ERROR_TIMEOUT ? PROKHZ_ERR_TIMEOUT : PROKHZ_ERR_IO;
    }
    return PROKHZ_OK;
}

static prokhz_err_t idrw_read(prokhz_dev_t *dev, prokhz_tag_id_t *id)
{
    uint8_t out_pkt[PACKET_LEN]={0}, in_pkt[PACKET_LEN]={0};
    uint8_t data[16]={0};
    int data_len=0;
    prokhz_err_t rc;
    build_packet(out_pkt, CMD_GET_SNR, NULL, 0);
    rc = ctrl_xfer(dev, out_pkt, in_pkt);
    if (rc != PROKHZ_OK) return rc;
    rc = parse_response(in_pkt, data, &data_len, dev->verbose);
    if (rc != PROKHZ_OK) return rc;
    if (data_len < PROKHZ_TAG_ID_LEN) return PROKHZ_ERR_NOTAG;
    memcpy(id->bytes, data, PROKHZ_TAG_ID_LEN);
    id->type = PROKHZ_TAG_EM4100;
    return PROKHZ_OK;
}

static prokhz_err_t idrw_write(prokhz_dev_t *dev,
                                const uint8_t id[PROKHZ_TAG_ID_LEN],
                                prokhz_tag_type_t tag_type)
{
    uint8_t out_pkt[PACKET_LEN]={0}, in_pkt[PACKET_LEN]={0}, pl[10]={0};
    uint8_t tt;
    prokhz_err_t rc;
    if      (tag_type == PROKHZ_TAG_T5577)  tt = TT_T5577;
    else if (tag_type == PROKHZ_TAG_EM4305) tt = TT_EM4305;
    else return PROKHZ_ERR_NOTSUP;
    pl[0]=0x00; pl[1]=0x01; pl[2]=0x01; pl[3]=tt;
    memcpy(&pl[4], id, PROKHZ_TAG_ID_LEN);
    pl[9]=0x80;
    build_packet(out_pkt, CMD_WRITE, pl, 10);
    out_pkt[6] = 0x1F;
    rc = ctrl_xfer(dev, out_pkt, in_pkt);
    if (rc != PROKHZ_OK) return rc;
    rc = parse_response(in_pkt, NULL, NULL, dev->verbose);
    if (rc == PROKHZ_ERR_NOTAG) return rc;
    sleep(1);
    if (tag_type == PROKHZ_TAG_T5577) {
        fprintf(stdout, "OK\n");
    } else {
        prokhz_tag_id_t verify={0};
        if (idrw_read(dev, &verify) == PROKHZ_OK) {
            char buf[32];
            prokhz_format_id(verify.bytes, PROKHZ_FMT_HEX, buf, sizeof buf);
            fprintf(stdout, "%s\n", buf);
        }
    }
    return PROKHZ_OK;
}

static prokhz_err_t idrw_beep(prokhz_dev_t *dev, uint8_t duration)
{
    uint8_t out_pkt[PACKET_LEN]={0}, in_pkt[PACKET_LEN]={0};
    uint8_t pl[2]={(duration<1)?1:(duration>9)?9:duration,0x01};
    build_packet(out_pkt, CMD_BUZZER, pl, 2);
    return ctrl_xfer(dev, out_pkt, in_pkt);
}

static void idrw_close(prokhz_dev_t *dev)
{
    idrw_priv_t *p = priv(dev);
    if (!p) return;
    if (p->devh) {
        libusb_release_interface(p->devh, 0);
        if (p->kernel_detached) libusb_attach_kernel_driver(p->devh, 0);
        libusb_close(p->devh);
    }
    if (p->ctx) libusb_exit(p->ctx);
    free(p); dev->priv = NULL;
}

static prokhz_err_t idrw_open(prokhz_dev_t *dev, const prokhz_opts_t *opts)
{
    idrw_priv_t *p = calloc(1, sizeof *p);
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
    if (libusb_set_configuration(p->devh, 1) < 0 ||
        libusb_claim_interface(p->devh, 0) < 0) {
        libusb_close(p->devh); libusb_exit(p->ctx); free(p);
        return PROKHZ_ERR_NODEV;
    }
    { uint8_t tmp[1];
      libusb_control_transfer(p->devh,
          LIBUSB_ENDPOINT_IN|LIBUSB_REQUEST_TYPE_STANDARD|
          LIBUSB_RECIPIENT_INTERFACE,
          LIBUSB_REQUEST_GET_DESCRIPTOR,
          (LIBUSB_DT_REPORT<<8), 0, tmp, sizeof tmp, dev->timeout_ms);
      libusb_reset_device(p->devh);
      sleep(2);
      libusb_claim_interface(p->devh, 0); }
    dev->priv = p;
    return PROKHZ_OK;
}

static const prokhz_ops_t idrw_ops = {
    .open  = idrw_open,
    .read  = idrw_read,
    .write = idrw_write,
    .beep  = idrw_beep,
    .close = idrw_close,
};

const prokhz_device_t prokhz_device_idrw = {
    .name = "idrw",
    .desc = "Generic USB Reader, USB HID control transfers (feature reports)",
    .ops  = &idrw_ops,
};
