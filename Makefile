CC       = gcc
CFLAGS   = -Wall -Wextra -Wpedantic -std=c99 -O2 -I. -D_DEFAULT_SOURCE
LDFLAGS  = -lusb-1.0

GTK_CFLAGS  = $(shell pkg-config --cflags gtk4)
GTK_LDFLAGS = $(shell pkg-config --libs gtk4)

LIB_SRC = prokhz_common.c \
          dev_rfid_app.c  \
          dev_p1d.c       \
          dev_ctx203.c    \
          dev_idrw.c

LIB_OBJ = $(LIB_SRC:.c=.o)
LIB     = libprokhz.a

ALL_HEADERS = prokhz.h prokhz_device.h

.PHONY: all clean install

all: prokhz_tool prokhz_gui

$(LIB): $(LIB_OBJ)
	ar rcs $@ $^

prokhz_tool: prokhz_tool.o $(LIB)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

prokhz_gui: prokhz_gui.o $(LIB)
	$(CC) $(CFLAGS) $(GTK_CFLAGS) -o $@ $^ $(LDFLAGS) $(GTK_LDFLAGS)

# Library and tool objects share the same flags
$(LIB_OBJ) prokhz_tool.o: %.o: %.c $(ALL_HEADERS)
	$(CC) $(CFLAGS) -c -o $@ $<

# GUI object: GTK CFLAGS, -Wpedantic suppressed (GTK4 headers trigger it)
prokhz_gui.o: prokhz_gui.c $(ALL_HEADERS)
	$(CC) $(CFLAGS) -Wno-pedantic $(GTK_CFLAGS) -c -o $@ $<

install: prokhz_tool prokhz_gui $(LIB)
	install -Dm755 prokhz_tool  $(DESTDIR)/usr/local/bin/prokhz-tool
	install -Dm755 prokhz_gui   $(DESTDIR)/usr/local/bin/prokhz-gui
	install -Dm644 $(LIB)       $(DESTDIR)/usr/local/lib/$(LIB)
	install -Dm644 prokhz.h     $(DESTDIR)/usr/local/include/prokhz.h

clean:
	rm -f $(LIB_OBJ) prokhz_tool.o prokhz_gui.o \
	      $(LIB) prokhz_tool prokhz_gui
