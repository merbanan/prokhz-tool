# libprokhz

A C library, command line tool, and GTK4 GUI for reading and writing
125 kHz RFID tags using USB and serial reader/writer hardware.

Originally based on reverse-engineered device drivers by Benjamin Larsson
(2014–2017). The library consolidates four separate tools into a unified
driver-registration API with automatic device detection.

---

## Supported hardware

| Driver | Device | Interface | VID:PID |
|--------|--------|-----------|---------|
| `ctx203` | CTX 203-ID-RW | USB HID interrupt transfers | `6688:6850` |
| `idrw` | Generic "USB Reader" | USB HID control transfers (feature reports) | `ffff:0035` |
| `rfid-app` | "ID card reader & writer6" | Serial binary, 38400 baud | — |
| `p1d` | P1D reader/writer | Serial ASCII, 9600 baud | — |

## Supported tag types

| Tag | Read | Write |
|-----|------|-------|
| EM4100 | All drivers | — |
| T5577 | — | `ctx203`, `idrw`, `p1d` |
| EM4305 | — | `ctx203`, `idrw` |

---

## File overview

| File | Description |
|------|-------------|
| `prokhz.h` | Public API — types, enums, function declarations |
| `prokhz_device.h` | Driver registration API (internal, not installed) |
| `prokhz_common.c` | Driver registry, probe/open dispatch, shared utilities |
| `dev_rfid_app.c` | Serial binary driver |
| `dev_p1d.c` | Serial ASCII driver |
| `dev_ctx203.c` | USB HID interrupt transfer driver |
| `dev_idrw.c` | USB HID control transfer driver |
| `prokhz_tool.c` | Command line tool |
| `prokhz_gui.c` | GTK4 graphical frontend |

---

## Building

### Dependencies

- GCC or compatible C99 compiler
- [libusb-1.0](https://libusb.info/) (for USB drivers)
- [GTK 4](https://gtk.org/) (for the GUI, optional)

On Debian/Ubuntu:

```sh
sudo apt install build-essential libusb-1.0-0-dev libgtk-4-dev
```

### Compile

```sh
make
```

Produces `libprokhz.a`, the `prokhz_tool` binary, and the `prokhz_gui` binary.

### Install

```sh
sudo make install
```

Installs `prokhz-tool` and `prokhz-gui` to `/usr/local/bin`, `libprokhz.a`
to `/usr/local/lib`, and `prokhz.h` to `/usr/local/include`.
A custom prefix can be set with `DESTDIR`:

```sh
sudo make install DESTDIR=/usr
```

---

## prokhz-tool

### Synopsis

```
prokhz-tool [OPTIONS]
```

### Device selection

By default `prokhz-tool` calls `prokhz_probe()`, which tries every
registered driver in order and uses the first device that responds.
Pass `--driver` to target a specific one.

```sh
prokhz-tool --list-drivers
```

```
Registered drivers (4), in probe order:

  ctx203          CTX 203-ID-RW, USB HID interrupt transfers (EP 0x03/0x85)
  idrw            Generic USB Reader, USB HID control transfers (feature reports)
  rfid-app        ID card reader & writer6, serial binary, 38400 baud
  p1d             P1D reader/writer, serial ASCII, 9600 baud
```

### Probe attached devices

Tries every registered driver and reports which hardware is present:

```sh
prokhz-tool --probe
```

```
Probing 4 driver(s)...

  ctx203          FOUND
  idrw            not found (device not found)
  rfid-app        not found (device not found)
  p1d             not found (device not found)
```

### Read a tag

```sh
# Auto-detect device, print raw hex
prokhz-tool -r

# Wiegand format (facility,card)
prokhz-tool -r -f wiegand

# 8-digit Aptus decimal
prokhz-tool -r -f dec8

# Force a specific driver and TTY
prokhz-tool --driver p1d -d /dev/ttyUSB1 -r
```

### Output formats

| Flag | Format | Example |
|------|--------|---------|
| `hex` | Raw hex, no spaces (default) | `0104AABB11` |
| `hex-spaced` | Spaced hex pairs | `01 04 AA BB 11` |
| `dec8h` | 10-digit decimal, bytes 1–4 | `0000045235` |
| `dec6h` | 10-digit decimal, bytes 2–4 | `0000011025` |
| `wiegand` | Facility and card number | `004,43793` |
| `dec9` | 9-digit decimal | `017563409` |
| `dec8` | 8-digit decimal (Aptus) | `17563409` |

### Write a tag

```sh
# Write to a T5577 tag (default)
prokhz-tool -w 0104AABB11

# Write to an EM4305 tag
prokhz-tool -w 0104AABB11 -t em4305

# Write via a specific driver, suppress beep
prokhz-tool --driver ctx203 -w 0104AABB11 -t t5577 -b
```

### Aptus tag generation

Generates random EM4100-compatible IDs in the Aptus range
(100 000 000 – 999 999 999), writes each to a writable tag, verifies
the readback, and loops until interrupted with Ctrl-C. Each successful
write is confirmed with a beep.

The ID is encoded as `01` followed by the value as a big-endian 32-bit
integer, matching the Aptus access control system's credential format.

```sh
prokhz-tool -a -t t5577
prokhz-tool -a -t em4305 --driver idrw
```

Output per successful write:
```
174392810 01A6627AA 01A6627AA
```
`<decimal value> <written hex> <readback hex>`

### Trigger buzzer

```sh
prokhz-tool -B 5
```

Not all drivers support the buzzer. `p1d` has no software buzzer
command; `prokhz-tool` prints a note and continues.

### All options

```
  --driver <n>           Use a named driver instead of probing
  -d, --device <path>    TTY path for serial drivers (default: /dev/ttyUSB0)
  --list-drivers         List registered drivers and exit
  -p, --probe            Try every driver and report which are present
  -r, --read             Read tag ID
  -w, --write <hex>      Write 10-char hex ID to tag
  -a, --aptus            Write random Aptus IDs in a loop
  -B, --beep <1-9>       Trigger buzzer
  -f, --format <fmt>     Output format for -r (see table above)
  -t, --tag-type <type>  t5577 | em4305 | em4100  (default: t5577)
  -b, --no-beep          Suppress auto-beep on tag access
  -v, --verbose          Debug output
  -h, --help             Show help
```

---

## prokhz-gui

A GTK4 graphical frontend providing all the functionality of
`prokhz-tool` in a point-and-click interface.

```sh
prokhz_gui
```

### Device frame

- **Driver** dropdown — lists all registered drivers plus `(auto)` for
  automatic probing.
- **Serial port** dropdown — populated by scanning `/dev/ttyUSB*` and
  `/dev/ttyACM*` at startup. The first port found is selected by
  default. The `↺` button rescans at any time.
- **Probe** — tries every registered driver and automatically selects
  the matched driver in the dropdown.
- **Connect / Disconnect** — opens or closes the selected device.
- **Beep** — sends a beep command to the connected device. Disabled for
  drivers without buzzer support.
- **Verbose** checkbox — enables debug output in the log.

### Read frame

Reads the tag currently in the RF field and displays the result in all
seven output formats simultaneously, each with its own Copy button.

### Write frame

Writes a 10-character hex tag ID to a writable tag. Leave the field
blank to write back the hex result from the last read.

### Generate Aptus tags frame

Generates random Aptus-range EM4100 IDs, writes them to a writable tag
in a loop, and verifies each write before moving on. A running count
and the last written ID are displayed. Stop ends the loop cleanly after
the current write completes.

### Log

All operations are logged with their result. The Clear button empties
the log.

---

## Library API

Include `prokhz.h` and link with `-lprokhz -lusb-1.0`.

### Open a device

```c
prokhz_opts_t opts = {
    .device     = NULL,   /* NULL = USB or /dev/ttyUSB0 */
    .beep       = 1,
    .verbose    = 0,
    .timeout_ms = 2000,
};

prokhz_dev_t *dev = NULL;

/* Auto-detect: tries every registered driver in order */
prokhz_err_t rc = prokhz_probe(&opts, &dev);

/* Or open a specific driver by name */
prokhz_err_t rc = prokhz_open("ctx203", &opts, &dev);

if (rc != PROKHZ_OK) {
    fprintf(stderr, "%s\n", prokhz_strerror(rc));
    return 1;
}
```

### Read a tag

```c
prokhz_tag_id_t tag;
rc = prokhz_read(dev, &tag);
if (rc == PROKHZ_OK) {
    char buf[32];
    prokhz_format_id(tag.bytes, PROKHZ_FMT_HEX, buf, sizeof buf);
    printf("%s\n", buf);
} else if (rc == PROKHZ_ERR_NOTAG) {
    puts("no tag");
}
```

### Write a tag

```c
uint8_t id[PROKHZ_TAG_ID_LEN];
prokhz_parse_hex_id("0104AABB11", id);
rc = prokhz_write(dev, id, PROKHZ_TAG_T5577);
```

### Close

```c
prokhz_close(dev);
```

### Error codes

| Code | Meaning |
|------|---------|
| `PROKHZ_OK` | Success |
| `PROKHZ_ERR_NOTAG` | No tag in RF field |
| `PROKHZ_ERR_IO` | Transport read/write failure |
| `PROKHZ_ERR_CHECKSUM` | Packet checksum mismatch |
| `PROKHZ_ERR_NODEV` | Device not found or failed to open |
| `PROKHZ_ERR_PARAM` | Invalid argument |
| `PROKHZ_ERR_NOTSUP` | Operation not supported by this driver |
| `PROKHZ_ERR_TIMEOUT` | Operation timed out |

---

## Adding a driver

1. Create `dev_mydevice.c`. Include `prokhz.h` and `prokhz_device.h`.
2. Implement `open`, `read`, `write`, `close`, and optionally `beep`,
   matching the signatures in `prokhz_ops_t`. The `open` function also
   serves as the probe function — return `PROKHZ_ERR_NODEV` if the
   hardware is not present.
3. Define a `prokhz_device_t` descriptor and expose it:
   ```c
   const prokhz_device_t prokhz_device_mydevice = {
       .name = "mydevice",
       .desc = "My RFID device",
       .ops  = &mydevice_ops,
   };
   ```
4. In `prokhz_common.c`, add an `extern` declaration and a
   `prokhz_device_register()` call inside `prokhz_device_register_all()`.
5. Add `dev_mydevice.c` to `LIB_SRC` in the Makefile.

---

## USB permissions

On Linux, libusb requires either root or a udev rule to access USB HID
devices without `sudo`. Create `/etc/udev/rules.d/70-prokhz.rules`:

```
# CTX 203-ID-RW
SUBSYSTEM=="usb", ATTRS{idVendor}=="6688", ATTRS{idProduct}=="6850", MODE="0666"
# Generic USB Reader
SUBSYSTEM=="usb", ATTRS{idVendor}=="ffff", ATTRS{idProduct}=="0035", MODE="0666"
```

Reload rules:

```sh
sudo udevadm control --reload-rules && sudo udevadm trigger
```

---

## Serial port permissions

Add your user to the `dialout` group to access `/dev/ttyUSB*` without
`sudo`:

```sh
sudo usermod -aG dialout $USER
```

Log out and back in for the change to take effect.

---

## License

MIT License. Copyright (C) 2014–2017 Benjamin Larsson.
See source files for full license text.
