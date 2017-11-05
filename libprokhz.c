/*
Copyright (C) 2017 Benjamin Larsson <benjamin.larsson@inteno.se>

Permission is hereby granted, free of charge, to any person obtaining a copy of this software and
associated documentation files (the "Software"), to deal in the Software without restriction,
including without limitation the rights to use, copy, modify, merge, publish, distribute,
sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all copies or
substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT
NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT
OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
*/


#include <errno.h>
#include <signal.h> 
#include <string.h> 
#include <stdio.h> 
#include <stdlib.h> 
#include <getopt.h>
#include <unistd.h>
#include <malloc.h>
#include <inttypes.h>

#include "libprokhz.h"
#include "libprokhz_internal.h"

prokhz_ctx* libprokhz_init(char *serial_tty_name) {
	int i, probe;
	prokhz_ctx* ctx;
	hw_device* hw_dev;
	hw_device devices[] = {
#define DECL(name) name,
	PROKHZ_DEVICES
#undef DECL
	};
	int devices_num = sizeof(devices)/sizeof(*devices);


	ctx = malloc(sizeof(prokhz_ctx));
	memset(ctx, 0, sizeof(prokhz_ctx));
        fprintf(stdout, "sizeof(prokhz_ctx)=%ld\n", sizeof(prokhz_ctx));

	// probe for devices, break on first find
	for (i=0 ; i<devices_num ; i++) {
		hw_dev = &devices[i];
	            fprintf(stdout, "probe()\n");
		probe = hw_dev->probe();
		if (probe) {
	            fprintf(stdout, "device %s found\n", hw_dev->name);

			ctx->name = hw_dev->name;
			ctx->em4100_read = hw_dev->em4100_read;
	            fprintf(stdout, "device %s found\n", ctx->name);
			break;
		}
	}

	if (!(ctx->name)) {
		free(ctx);
		return NULL;
	}
            fprintf(stdout, "device %s found\n", ctx->name);

	return ctx;
};


