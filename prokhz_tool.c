/*
 * prokhz-tool.c — 125 kHz RFID command line tool using libprokhz
 */

#include "prokhz.h"
#include "prokhz_device.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>
#include <time.h>

/* ── Aptus tag generation ─────────────────────────────────────────────── */

#define APTUS_MIN 100000000UL
#define APTUS_MAX 999999999UL

static void run_aptus(prokhz_dev_t *dev, prokhz_tag_type_t tag_type)
{
    uint8_t          id[PROKHZ_TAG_ID_LEN];
    prokhz_tag_id_t  readback;
    char             written[32], read_buf[32];
    prokhz_err_t     rc;
    unsigned long    val;

    srand((unsigned)time(NULL));
    fprintf(stdout, "Aptus generation running on driver '%s'. "
                    "Press Ctrl-C to stop.\n",
            prokhz_driver_name(dev));

    for (;;) {
        val = (unsigned long)(
            (double)rand() / ((double)RAND_MAX + 1.0) *
            (double)(APTUS_MAX - APTUS_MIN + 1)
        ) + APTUS_MIN;

        id[0] = 0x01;
        id[1] = (uint8_t)(val >> 24);
        id[2] = (uint8_t)(val >> 16);
        id[3] = (uint8_t)(val >>  8);
        id[4] = (uint8_t)(val);

        prokhz_format_id(id, PROKHZ_FMT_HEX, written, sizeof written);

        do {
            rc = prokhz_write(dev, id, tag_type);
            if (rc != PROKHZ_OK) {
                fprintf(stderr, "Write: %s\n", prokhz_strerror(rc));
                continue;
            }
            memset(&readback, 0, sizeof readback);
            rc = prokhz_read(dev, &readback);
            if (rc != PROKHZ_OK) continue;
            prokhz_format_id(readback.bytes, PROKHZ_FMT_HEX,
                             read_buf, sizeof read_buf);
        } while (rc != PROKHZ_OK || strcmp(written, read_buf) != 0);

        fprintf(stdout, "%lu %s %s\n", val, written, read_buf);
        prokhz_beep(dev, 1);
    }
}

/* ── probe ────────────────────────────────────────────────────────────── */

static void run_probe(const prokhz_opts_t *opts)
{
    int           i, n;
    prokhz_dev_t *dev = NULL;
    prokhz_err_t  rc;

    prokhz_device_register_all();
    n = prokhz_device_count();
    fprintf(stdout, "Probing %d registered driver(s)...\n\n", n);

    for (i = 0; i < n; i++) {
        const prokhz_device_t *drv = prokhz_device_get(i);
        fprintf(stdout, "  %-14s  %s\n", drv->name, drv->desc);

        rc = prokhz_open(drv->name, opts, &dev);
        if (rc == PROKHZ_OK) {
            fprintf(stdout, "               -> FOUND\n");
            prokhz_close(dev);
            dev = NULL;
        } else {
            fprintf(stdout, "               -> not found (%s)\n",
                    prokhz_strerror(rc));
        }
    }
    fprintf(stdout, "\n");
}

/* ── usage ────────────────────────────────────────────────────────────── */

static void print_usage(const char *prog)
{
    fprintf(stdout,
        "prokhz-tool — 125 kHz RFID utility (libprokhz)\n"
        "\n"
        "Usage: %s [OPTIONS]\n"
        "\n"
        "Device:\n"
        "  --driver <n>           Use a specific driver instead of probing.\n"
        "                           See --list-drivers for available names.\n"
        "  -d, --device <path>    TTY device path (serial drivers only,\n"
        "                           default: /dev/ttyUSB0)\n"
        "  --list-drivers         List all registered drivers and exit\n"
        "  -p, --probe            Try every registered driver and report\n"
        "                           which devices are present, then exit\n"
        "\n"
        "Operations (pick one):\n"
        "  -r, --read             Read tag ID\n"
        "  -w, --write <hex>      Write 10-char hex ID  e.g. 0104AABB11\n"
        "  -a, --aptus            Write random Aptus IDs in a loop\n"
        "  -B, --beep  <1-9>      Trigger buzzer\n"
        "\n"
        "Output format (-r only):\n"
        "  -f, --format <fmt>     hex         AABBCC1122  (default)\n"
        "                         hex-spaced  AA BB CC 11 22\n"
        "                         dec8h       10-digit decimal (bytes 1-4)\n"
        "                         dec6h       10-digit decimal (bytes 2-4)\n"
        "                         wiegand     NNN,NNNNN\n"
        "                         dec9        9-digit decimal\n"
        "                         dec8        8-digit decimal (Aptus)\n"
        "\n"
        "Write / Aptus options:\n"
        "  -t, --tag-type <type>  t5577 (default) | em4305 | em4100\n"
        "\n"
        "Misc:\n"
        "  -b, --no-beep          Suppress auto-beep on tag access\n"
        "  -v, --verbose          Debug output\n"
        "  -h, --help             Show this help\n"
        "\n",
        prog);
}

static void list_drivers(void)
{
    int i, n;
    prokhz_device_register_all();
    n = prokhz_device_count();
    fprintf(stdout, "Registered drivers (%d), in probe order:\n\n", n);
    for (i = 0; i < n; i++) {
        const prokhz_device_t *d = prokhz_device_get(i);
        fprintf(stdout, "  %-14s  %s\n", d->name, d->desc);
    }
    fprintf(stdout, "\n");
}

/* ── option helpers ───────────────────────────────────────────────────── */

static const struct option long_opts[] = {
    {"driver",       required_argument, NULL, 'D'},
    {"device",       required_argument, NULL, 'd'},
    {"list-drivers", no_argument,       NULL, 'L'},
    {"probe",        no_argument,       NULL, 'p'},
    {"read",         no_argument,       NULL, 'r'},
    {"write",        required_argument, NULL, 'w'},
    {"aptus",        no_argument,       NULL, 'a'},
    {"beep",         required_argument, NULL, 'B'},
    {"format",       required_argument, NULL, 'f'},
    {"tag-type",     required_argument, NULL, 't'},
    {"no-beep",      no_argument,       NULL, 'b'},
    {"verbose",      no_argument,       NULL, 'v'},
    {"help",         no_argument,       NULL, 'h'},
    {NULL, 0, NULL, 0}
};

static prokhz_format_t parse_format(const char *s)
{
    if (!strcmp(s,"hex"))         return PROKHZ_FMT_HEX;
    if (!strcmp(s,"hex-spaced"))  return PROKHZ_FMT_HEX_SPACED;
    if (!strcmp(s,"dec8h"))       return PROKHZ_FMT_DEC_8H;
    if (!strcmp(s,"dec6h"))       return PROKHZ_FMT_DEC_6H;
    if (!strcmp(s,"wiegand"))     return PROKHZ_FMT_WIEGAND;
    if (!strcmp(s,"dec9"))        return PROKHZ_FMT_DEC_9;
    if (!strcmp(s,"dec8"))        return PROKHZ_FMT_DEC_8;
    fprintf(stderr, "Unknown format '%s'.\n", s);
    exit(EXIT_FAILURE);
}

static prokhz_tag_type_t parse_tag_type(const char *s)
{
    if (!strcmp(s,"em4100")) return PROKHZ_TAG_EM4100;
    if (!strcmp(s,"t5577"))  return PROKHZ_TAG_T5577;
    if (!strcmp(s,"em4305")) return PROKHZ_TAG_EM4305;
    fprintf(stderr, "Unknown tag type '%s'. Use: em4100 | t5577 | em4305\n", s);
    exit(EXIT_FAILURE);
}

/* ── main ─────────────────────────────────────────────────────────────── */

int main(int argc, char **argv)
{
    const char        *driver_name = NULL;
    const char        *device      = NULL;
    prokhz_format_t    format      = PROKHZ_FMT_HEX;
    prokhz_tag_type_t  tag_type    = PROKHZ_TAG_T5577;
    int                do_probe    = 0;
    int                do_read     = 0;
    int                do_aptus    = 0;
    int                do_beep     = 0;
    int                no_beep     = 0;
    int                verbose     = 0;
    uint8_t            beep_dur    = 3;
    const char        *write_hex   = NULL;

    int opt;
    while ((opt = getopt_long(argc, argv, "D:d:Lprw:aB:f:t:bvh",
                              long_opts, NULL)) != -1) {
        switch (opt) {
        case 'D': driver_name = optarg;                break;
        case 'd': device      = optarg;                break;
        case 'L': list_drivers(); return EXIT_SUCCESS;
        case 'p': do_probe    = 1;                     break;
        case 'r': do_read     = 1;                     break;
        case 'w': write_hex   = optarg;                break;
        case 'a': do_aptus    = 1;                     break;
        case 'B': do_beep=1; beep_dur=(uint8_t)atoi(optarg); break;
        case 'f': format      = parse_format(optarg);  break;
        case 't': tag_type    = parse_tag_type(optarg);break;
        case 'b': no_beep     = 1;                     break;
        case 'v': verbose     = 1;                     break;
        case 'h': print_usage(argv[0]); return EXIT_SUCCESS;
        default:  print_usage(argv[0]); return EXIT_FAILURE;
        }
    }

    if (!do_probe && !do_read && !write_hex && !do_aptus && !do_beep) {
        print_usage(argv[0]);
        return EXIT_FAILURE;
    }

    if (do_aptus && tag_type == PROKHZ_TAG_EM4100) {
        fprintf(stderr, "Aptus requires a writable tag type: t5577 or em4305\n");
        return EXIT_FAILURE;
    }

    prokhz_opts_t opts = {
        .device     = device,
        .beep       = !no_beep,
        .verbose    = verbose,
        .timeout_ms = 2000,
    };

    /* probe runs before opening a device and exits independently */
    if (do_probe) {
        run_probe(&opts);
        return EXIT_SUCCESS;
    }

    prokhz_dev_t *dev = NULL;
    prokhz_err_t  rc;

    if (driver_name)
        rc = prokhz_open(driver_name, &opts, &dev);
    else
        rc = prokhz_probe(&opts, &dev);

    if (rc != PROKHZ_OK) {
        fprintf(stderr, "Device open failed: %s\n", prokhz_strerror(rc));
        return EXIT_FAILURE;
    }

    if (verbose)
        fprintf(stderr, "Using driver: %s\n", prokhz_driver_name(dev));

    if (do_beep) {
        rc = prokhz_beep(dev, beep_dur);
        if (rc == PROKHZ_ERR_NOTSUP)
            fprintf(stderr, "Note: driver '%s' has no buzzer.\n",
                    prokhz_driver_name(dev));
        else if (rc != PROKHZ_OK)
            fprintf(stderr, "Beep: %s\n", prokhz_strerror(rc));
    }

    if (write_hex) {
        uint8_t id[PROKHZ_TAG_ID_LEN];
        rc = prokhz_parse_hex_id(write_hex, id);
        if (rc != PROKHZ_OK) {
            fprintf(stderr, "Bad hex ID '%s': need exactly 10 hex chars.\n",
                    write_hex);
            prokhz_close(dev);
            return EXIT_FAILURE;
        }
        rc = prokhz_write(dev, id, tag_type);
        if (rc != PROKHZ_OK) {
            fprintf(stderr, "Write: %s\n", prokhz_strerror(rc));
            prokhz_close(dev);
            return EXIT_FAILURE;
        }
    }

    if (do_read) {
        prokhz_tag_id_t tag = {0};
        rc = prokhz_read(dev, &tag);
        if (rc == PROKHZ_ERR_NOTAG) {
            fprintf(stdout, "NOTAG\n");
        } else if (rc != PROKHZ_OK) {
            fprintf(stderr, "Read: %s\n", prokhz_strerror(rc));
            prokhz_close(dev);
            return EXIT_FAILURE;
        } else {
            char buf[32] = {0};
            prokhz_format_id(tag.bytes, format, buf, sizeof buf);
            fprintf(stdout, "%s\n", buf);
        }
    }

    if (do_aptus)
        run_aptus(dev, tag_type);

    prokhz_close(dev);
    return EXIT_SUCCESS;
}
