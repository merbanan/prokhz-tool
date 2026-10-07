/*
 * dev_tcc.c — CYB NFC Tool USB driver
 *
 * USB identity: 0483:4343, product "TCC-TSY-TBY".
 *
 * The wire protocol was recovered from the Android CYB NFC Tool app.  The
 * device exposes a HID-class interrupt interface with endpoint 0x02 for
 * writes and 0x82 for reads.  Messages are framed as:
 *
 *   55  command_be16  payload_length_be16  payload  ~xor(payload)  xor(payload)
 *
 * The buzzer, LF read, and LF write commands are 0x0055, 0x0072, and 0x0073
 * respectively.
 *
 * Copyright (C) 2026 Benjamin Larsson
 * MIT License — see LICENSE for details.
 */

#include "prokhz.h"
#include "prokhz_device.h"

#include <libusb-1.0/libusb.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

#define TCC_VID 0x0483
#define TCC_PID 0x4343

#define TCC_EP_OUT 0x02
#define TCC_EP_IN  0x82
#define TCC_USB_PACKET_SIZE 64

#define TCC_FRAME_MARKER   0x55
#define TCC_FRAME_OVERHEAD 7
#define TCC_MAX_PAYLOAD    2041
#define TCC_RX_CAPACITY    4096

#define TCC_CMD_BEEP  0x0055
#define TCC_CMD_READ  0x0072
#define TCC_CMD_WRITE 0x0073

#define TCC_MODE_ID       0
#define TCC_FREQ_125_KHZ  1
#define TCC_CARD_T55X7    1
#define TCC_CARD_EM4X05   3
#define TCC_USE_DICTIONARY 1

#define TCC_READ_RESPONSE_LEN  24
#define TCC_WRITE_REQUEST_LEN  24
#define TCC_WRITE_RESPONSE_LEN 36
#define TCC_BEEP_REQUEST_LEN     4

typedef struct {
    uint8_t data[TCC_RX_CAPACITY];
    size_t  len;
} tcc_rx_t;

typedef struct {
    libusb_context       *ctx;
    libusb_device_handle *devh;
    int                   interface_number;
    int                   alternate_setting;
    int                   kernel_detached;
    unsigned int          out_interval_ms;
    tcc_rx_t              rx;
} tcc_priv_t;

/* Return this handle's driver-private state. */
static inline tcc_priv_t *priv(prokhz_dev_t *dev)
{
    return (tcc_priv_t *)dev->priv;
}

/* Decode 4 bytes as a little-endian uint32. */
static uint32_t load_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* Encode value as 4 little-endian bytes. */
static void store_le32(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
    p[2] = (uint8_t)(value >> 16);
    p[3] = (uint8_t)(value >> 24);
}

/* XOR-fold the payload bytes into a single checksum byte. */
static uint8_t payload_xor(const uint8_t *payload, size_t len)
{
    uint8_t result = 0;
    size_t i;
    for (i = 0; i < len; i++) result ^= payload[i];
    return result;
}

/*
 * Assemble one request frame:
 *   55 command_be16 payload_len_be16 payload ~xor(payload) xor(payload)
 * Returns the total frame length, or 0 if the arguments are invalid or
 * the frame would not fit in out.
 */
static size_t tcc_build_frame(uint8_t *out, size_t out_size,
                              uint16_t command, const uint8_t *payload,
                              size_t payload_len)
{
    uint8_t checksum;
    size_t frame_len = payload_len + TCC_FRAME_OVERHEAD;

    if (!out || payload_len > TCC_MAX_PAYLOAD || frame_len > out_size ||
        (payload_len != 0 && !payload))
        return 0;

    out[0] = TCC_FRAME_MARKER;
    out[1] = (uint8_t)(command >> 8);
    out[2] = (uint8_t)command;
    out[3] = (uint8_t)(payload_len >> 8);
    out[4] = (uint8_t)payload_len;
    if (payload_len != 0) memcpy(out + 5, payload, payload_len);

    checksum = payload_xor(payload, payload_len);
    out[5 + payload_len] = (uint8_t)~checksum;
    out[6 + payload_len] = checksum;
    return frame_len;
}

/* Discard the first count bytes of the receive buffer. */
static void tcc_rx_drop(tcc_rx_t *rx, size_t count)
{
    if (count >= rx->len) {
        rx->len = 0;
        return;
    }
    memmove(rx->data, rx->data + count, rx->len - count);
    rx->len -= count;
}

/* Returns 1 for a frame, 0 for incomplete/no matching frame, and -1/-2 on
 * checksum/size errors.  Complete frames for other commands are discarded. */
static int tcc_rx_extract(tcc_rx_t *rx, uint16_t expected_command,
                          uint8_t *payload, size_t payload_capacity,
                          size_t *payload_len_out)
{
    size_t payload_len, frame_len;
    uint16_t command;
    uint8_t checksum;

    for (;;) {
        while (rx->len != 0 && rx->data[0] != TCC_FRAME_MARKER)
            tcc_rx_drop(rx, 1);
        if (rx->len < 5) return 0;

        payload_len = ((size_t)rx->data[3] << 8) | rx->data[4];
        if (payload_len > TCC_MAX_PAYLOAD) {
            tcc_rx_drop(rx, 1);
            continue;
        }
        frame_len = payload_len + TCC_FRAME_OVERHEAD;
        if (rx->len < frame_len) return 0;

        checksum = payload_xor(rx->data + 5, payload_len);
        if (rx->data[5 + payload_len] != (uint8_t)~checksum ||
            rx->data[6 + payload_len] != checksum) {
            tcc_rx_drop(rx, frame_len);
            return -1;
        }

        command = ((uint16_t)rx->data[1] << 8) | rx->data[2];
        if (command != expected_command) {
            tcc_rx_drop(rx, frame_len);
            continue;
        }
        if (payload_len > payload_capacity) {
            tcc_rx_drop(rx, frame_len);
            return -2;
        }

        if (payload_len != 0) memcpy(payload, rx->data + 5, payload_len);
        if (payload_len_out) *payload_len_out = payload_len;
        tcc_rx_drop(rx, frame_len);
        return 1;
    }
}

/*
 * Build the 24-byte LF write request payload: mode, frequency, card type,
 * dictionary flag, then the 40-bit tag ID as two little-endian uint32
 * fields.  card_type is TCC_CARD_T55X7 or TCC_CARD_EM4X05.
 */
static void tcc_build_write_payload(uint8_t out[TCC_WRITE_REQUEST_LEN],
                                    const uint8_t id[PROKHZ_TAG_ID_LEN],
                                    uint8_t card_type)
{
    uint64_t value;

    memset(out, 0, TCC_WRITE_REQUEST_LEN);
    out[0] = TCC_MODE_ID;
    out[1] = TCC_FREQ_125_KHZ;
    out[2] = card_type;
    out[3] = TCC_USE_DICTIONARY;

    value = ((uint64_t)id[0] << 32) | ((uint64_t)id[1] << 24) |
            ((uint64_t)id[2] << 16) | ((uint64_t)id[3] << 8) | id[4];

    /* The firmware ABI splits the 40-bit value into two native uint32_t
     * fields, high word first, inside the request payload. */
    store_le32(out + 12, (uint32_t)(value >> 32));
    store_le32(out + 16, (uint32_t)value);
}

/* Copy the fixed buzzer-on or buzzer-off payload. */
static void tcc_build_beep_payload(uint8_t out[TCC_BEEP_REQUEST_LEN],
                                   int enabled)
{
    static const uint8_t on[TCC_BEEP_REQUEST_LEN] = {0x00, 0x05, 0x01, 0x00};
    static const uint8_t off[TCC_BEEP_REQUEST_LEN] = {0x01, 0x01, 0x02, 0x00};

    memcpy(out, enabled ? on : off, TCC_BEEP_REQUEST_LEN);
}

/* 1 = ID decoded, 0 = progress update, -1 = scan completed without a tag,
 * -2 = malformed response. */
static int tcc_decode_read_payload(const uint8_t *payload, size_t payload_len,
                                   uint8_t id[PROKHZ_TAG_ID_LEN])
{
    uint64_t value;

    if (payload_len < 21) return -2;
    if (payload[2] == 0) return payload[20] != 0 ? -1 : 0;
    if (payload[0] != TCC_MODE_ID) return -2;

    value = ((uint64_t)load_le32(payload + 8) << 32) |
            load_le32(payload + 12);
    id[0] = (uint8_t)(value >> 32);
    id[1] = (uint8_t)(value >> 24);
    id[2] = (uint8_t)(value >> 16);
    id[3] = (uint8_t)(value >> 8);
    id[4] = (uint8_t)value;
    return 1;
}

/* Current CLOCK_MONOTONIC time in milliseconds; 0 on failure. */
static int64_t monotonic_ms(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/*
 * Frame command+payload and write it to the OUT interrupt endpoint.
 * Sleeps for the endpoint's bInterval afterwards so back-to-back
 * transfers are not dropped by the firmware.
 */
static prokhz_err_t tcc_send(prokhz_dev_t *dev, uint16_t command,
                             const uint8_t *payload, size_t payload_len)
{
    tcc_priv_t *p = priv(dev);
    uint8_t frame[TCC_MAX_PAYLOAD + TCC_FRAME_OVERHEAD];
    size_t frame_len;
    int transferred = 0;
    int rc;

    frame_len = tcc_build_frame(frame, sizeof frame, command,
                                payload, payload_len);
    if (frame_len == 0) return PROKHZ_ERR_PARAM;

    rc = libusb_interrupt_transfer(p->devh, TCC_EP_OUT, frame,
                                   (int)frame_len, &transferred,
                                   (unsigned int)dev->timeout_ms);
    if (rc < 0) {
        if (dev->verbose)
            fprintf(stderr, "[tcc] OUT: %s\n", libusb_error_name(rc));
        return rc == LIBUSB_ERROR_TIMEOUT ? PROKHZ_ERR_TIMEOUT : PROKHZ_ERR_IO;
    }
    if (transferred != (int)frame_len) return PROKHZ_ERR_IO;
    if (p->out_interval_ms != 0)
        usleep(p->out_interval_ms * 1000U);
    return PROKHZ_OK;
}

/*
 * Read interrupt packets into the rx buffer until a complete frame for
 * command is extracted or timeout_ms elapses.  On success the frame's
 * payload is returned via payload / *payload_len_out.
 */
static prokhz_err_t tcc_receive(prokhz_dev_t *dev, uint16_t command,
                                uint8_t *payload, size_t payload_capacity,
                                size_t *payload_len_out, int timeout_ms)
{
    tcc_priv_t *p = priv(dev);
    uint8_t packet[TCC_USB_PACKET_SIZE];
    int64_t deadline = monotonic_ms() + timeout_ms;

    for (;;) {
        int extracted = tcc_rx_extract(&p->rx, command, payload,
                                       payload_capacity, payload_len_out);
        int64_t remaining;
        int transferred = 0;
        int rc;

        if (extracted == 1) return PROKHZ_OK;
        if (extracted == -1) return PROKHZ_ERR_CHECKSUM;
        if (extracted == -2) return PROKHZ_ERR_IO;

        remaining = deadline - monotonic_ms();
        if (remaining <= 0) return PROKHZ_ERR_TIMEOUT;
        rc = libusb_interrupt_transfer(p->devh, TCC_EP_IN, packet,
                                       sizeof packet, &transferred,
                                       (unsigned int)remaining);
        if (transferred > 0) {
            if (p->rx.len + (size_t)transferred > sizeof p->rx.data)
                p->rx.len = 0;
            memcpy(p->rx.data + p->rx.len, packet, (size_t)transferred);
            p->rx.len += (size_t)transferred;
        }
        if (rc < 0 && rc != LIBUSB_ERROR_TIMEOUT) {
            if (dev->verbose)
                fprintf(stderr, "[tcc] IN: %s\n", libusb_error_name(rc));
            return PROKHZ_ERR_IO;
        }
    }
}

/* Flush stale device output: clear the rx buffer, then read the IN
 * endpoint until it goes quiet (1 ms per read). */
static void tcc_drain_input(tcc_priv_t *p)
{
    uint8_t packet[TCC_USB_PACKET_SIZE];
    int transferred;
    int rc;

    p->rx.len = 0;
    do {
        transferred = 0;
        rc = libusb_interrupt_transfer(p->devh, TCC_EP_IN, packet,
                                       sizeof packet, &transferred, 1);
    } while (rc == 0 && transferred > 0);
}

/*
 * EM4100 read: send the ID-mode 125 kHz scan request, then consume
 * response frames until the scan reports a tag (PROKHZ_OK, id->bytes),
 * a completed scan without a tag (PROKHZ_ERR_NOTAG), or the attempt
 * budget is exhausted (PROKHZ_ERR_TIMEOUT).
 */
static prokhz_err_t tcc_read(prokhz_dev_t *dev, prokhz_tag_id_t *id)
{
    uint8_t request[2] = {TCC_MODE_ID, TCC_FREQ_125_KHZ};
    uint8_t response[TCC_READ_RESPONSE_LEN];
    int response_timeout = dev->timeout_ms < 1500 ? 1500 : dev->timeout_ms;
    int attempts;
    prokhz_err_t rc;

    tcc_drain_input(priv(dev));
    rc = tcc_send(dev, TCC_CMD_READ, request, sizeof request);
    if (rc != PROKHZ_OK) return rc;

    for (attempts = 0; attempts < 16; attempts++) {
        size_t response_len = 0;
        int decoded;

        rc = tcc_receive(dev, TCC_CMD_READ, response, sizeof response,
                         &response_len, response_timeout);
        if (rc != PROKHZ_OK) return rc;
        decoded = tcc_decode_read_payload(response, response_len, id->bytes);
        if (decoded == 1) {
            id->type = PROKHZ_TAG_EM4100;
            return PROKHZ_OK;
        }
        if (decoded == -1) return PROKHZ_ERR_NOTAG;
        if (decoded == -2) return PROKHZ_ERR_IO;
    }
    return PROKHZ_ERR_TIMEOUT;
}

/*
 * Write the 40-bit ID to a T5577 or EM4305 tag; any other tag type is
 * PROKHZ_ERR_NOTSUP.  The device confirms acceptance with a 36-byte
 * response whose byte 31 is non-zero; a "Done" text response means the
 * write failed.  Scans that report no writable tag yield NOTAG errors
 * via the response status.
 */
static prokhz_err_t tcc_write(prokhz_dev_t *dev,
                              const uint8_t id[PROKHZ_TAG_ID_LEN],
                              prokhz_tag_type_t tag_type)
{
    uint8_t request[TCC_WRITE_REQUEST_LEN];
    uint8_t response[TCC_WRITE_RESPONSE_LEN];
    uint8_t card_type;
    int response_timeout = dev->timeout_ms < 20000 ? 20000 : dev->timeout_ms;
    int attempts;
    prokhz_err_t rc;

    if (tag_type == PROKHZ_TAG_T5577)
        card_type = TCC_CARD_T55X7;
    else if (tag_type == PROKHZ_TAG_EM4305)
        card_type = TCC_CARD_EM4X05;
    else
        return PROKHZ_ERR_NOTSUP;

    tcc_build_write_payload(request, id, card_type);
    tcc_drain_input(priv(dev));
    rc = tcc_send(dev, TCC_CMD_WRITE, request, sizeof request);
    if (rc != PROKHZ_OK) return rc;

    for (attempts = 0; attempts < 32; attempts++) {
        size_t response_len = 0;

        rc = tcc_receive(dev, TCC_CMD_WRITE, response, sizeof response,
                         &response_len, response_timeout);
        if (rc != PROKHZ_OK) return rc;
        if (response_len < TCC_WRITE_RESPONSE_LEN) return PROKHZ_ERR_IO;
        if (response[31] != 0) return PROKHZ_OK;
        if (strncasecmp((const char *)response, "Done", 4) == 0)
            return PROKHZ_ERR_IO;
    }
    return PROKHZ_ERR_TIMEOUT;
}

/* Pulse the buzzer for duration*100 ms using paired on/off commands. */
static prokhz_err_t tcc_beep(prokhz_dev_t *dev, uint8_t duration)
{
    uint8_t request[TCC_BEEP_REQUEST_LEN];
    prokhz_err_t rc;

    if (duration < 1) duration = 1;
    if (duration > 9) duration = 9;

    tcc_drain_input(priv(dev));
    tcc_build_beep_payload(request, 1);
    rc = tcc_send(dev, TCC_CMD_BEEP, request, sizeof request);
    if (rc != PROKHZ_OK) return rc;

    /* prokhz-tool durations are 1..9; use 100 ms steps for this device's
     * separate buzzer-on and buzzer-off commands. */
    usleep((useconds_t)duration * 100000U);

    tcc_build_beep_payload(request, 0);
    return tcc_send(dev, TCC_CMD_BEEP, request, sizeof request);
}

/* Release the interface, reattach the kernel driver if detached,
 * tear down the libusb handle/context, and free the private state. */
static void tcc_close(prokhz_dev_t *dev)
{
    tcc_priv_t *p = priv(dev);
    if (!p) return;
    if (p->devh) {
        libusb_release_interface(p->devh, p->interface_number);
        if (p->kernel_detached)
            libusb_attach_kernel_driver(p->devh, p->interface_number);
        libusb_close(p->devh);
    }
    if (p->ctx) libusb_exit(p->ctx);
    free(p);
    dev->priv = NULL;
}

/*
 * Search the device's configuration descriptors for a HID-class altsetting
 * carrying both interrupt endpoints 0x82 (IN) and 0x02 (OUT).  Returns 1
 * and stores the interface number, alternate setting, and OUT bInterval
 * (used as the inter-send delay) when found; 0 otherwise.
 */
static int tcc_find_interface(libusb_device *device, int *interface_number,
                              int *alternate_setting,
                              unsigned int *out_interval_ms)
{
    struct libusb_config_descriptor *config = NULL;
    int rc, i, j, k;

    rc = libusb_get_active_config_descriptor(device, &config);
    if (rc < 0) rc = libusb_get_config_descriptor(device, 0, &config);
    if (rc < 0 || !config) return 0;

    for (i = 0; i < config->bNumInterfaces; i++) {
        const struct libusb_interface *interface = &config->interface[i];
        for (j = 0; j < interface->num_altsetting; j++) {
            const struct libusb_interface_descriptor *alt = &interface->altsetting[j];
            int have_in = 0, have_out = 0;
            unsigned int interval = 0;

            if (alt->bInterfaceClass != LIBUSB_CLASS_HID) continue;
            for (k = 0; k < alt->bNumEndpoints; k++) {
                const struct libusb_endpoint_descriptor *ep = &alt->endpoint[k];
                if ((ep->bmAttributes & LIBUSB_TRANSFER_TYPE_MASK) !=
                    LIBUSB_TRANSFER_TYPE_INTERRUPT)
                    continue;
                if (ep->bEndpointAddress == TCC_EP_IN) have_in = 1;
                if (ep->bEndpointAddress == TCC_EP_OUT) {
                    have_out = 1;
                    interval = ep->bInterval;
                }
            }
            if (have_in && have_out) {
                *interface_number = alt->bInterfaceNumber;
                *alternate_setting = alt->bAlternateSetting;
                *out_interval_ms = interval;
                libusb_free_config_descriptor(config);
                return 1;
            }
        }
    }
    libusb_free_config_descriptor(config);
    return 0;
}

/*
 * Open the device: init libusb, locate VID:PID 0483:4343, find its
 * interrupt interface, detach the kernel driver when active, claim the
 * interface, and drain any pending output.  Doubles as the probe
 * function — returns PROKHZ_ERR_NODEV when the hardware is absent.
 */
static prokhz_err_t tcc_open(prokhz_dev_t *dev, const prokhz_opts_t *opts)
{
    tcc_priv_t *p = calloc(1, sizeof *p);
    libusb_device *device;
    int rc;

    if (!p) return PROKHZ_ERR_IO;
    p->interface_number = -1;
    rc = libusb_init(&p->ctx);
    if (rc < 0) {
        free(p);
        return PROKHZ_ERR_NODEV;
    }
    if (opts->verbose)
        libusb_set_option(p->ctx, LIBUSB_OPTION_LOG_LEVEL,
                          LIBUSB_LOG_LEVEL_WARNING);

    p->devh = libusb_open_device_with_vid_pid(p->ctx, TCC_VID, TCC_PID);
    if (!p->devh) {
        libusb_exit(p->ctx);
        free(p);
        return PROKHZ_ERR_NODEV;
    }

    device = libusb_get_device(p->devh);
    if (!tcc_find_interface(device, &p->interface_number,
                            &p->alternate_setting, &p->out_interval_ms)) {
        libusb_close(p->devh);
        libusb_exit(p->ctx);
        free(p);
        return PROKHZ_ERR_NODEV;
    }

    rc = libusb_kernel_driver_active(p->devh, p->interface_number);
    if (rc == 1) {
        rc = libusb_detach_kernel_driver(p->devh, p->interface_number);
        if (rc < 0) {
            libusb_close(p->devh);
            libusb_exit(p->ctx);
            free(p);
            return PROKHZ_ERR_NODEV;
        }
        p->kernel_detached = 1;
    }

    rc = libusb_claim_interface(p->devh, p->interface_number);
    if (rc < 0) {
        if (p->kernel_detached)
            libusb_attach_kernel_driver(p->devh, p->interface_number);
        libusb_close(p->devh);
        libusb_exit(p->ctx);
        free(p);
        return PROKHZ_ERR_NODEV;
    }
    if (p->alternate_setting != 0) {
        rc = libusb_set_interface_alt_setting(p->devh, p->interface_number,
                                              p->alternate_setting);
        if (rc < 0) {
            libusb_release_interface(p->devh, p->interface_number);
            if (p->kernel_detached)
                libusb_attach_kernel_driver(p->devh, p->interface_number);
            libusb_close(p->devh);
            libusb_exit(p->ctx);
            free(p);
            return PROKHZ_ERR_NODEV;
        }
    }

    dev->priv = p;
    tcc_drain_input(p);
    return PROKHZ_OK;
}

static const prokhz_ops_t tcc_ops = {
    .open  = tcc_open,
    .read  = tcc_read,
    .write = tcc_write,
    .beep  = tcc_beep,
    .close = tcc_close,
};

const prokhz_device_t prokhz_device_tcc = {
    .name = "tcc",
    .desc = "CYB NFC Tool TCC-TSY-TBY, USB HID (0483:4343)",
    .ops  = &tcc_ops,
};
