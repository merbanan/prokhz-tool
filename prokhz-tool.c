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

/* Command line client for libprokhz */

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

void print_usage() {
    printf("prokhz-tool version 0.1, Copyright (c) 2017 Benjamin Larsson <benjamin.larsson@inteno.se>\n");     
    printf("Usage: prokhz-tool [OPTION]...\n");
    printf("\t -p Probe for supported hardware\n");
    printf("\t -r Read rfid tag/fob\n");
}


int main(int argc,char** argv)
{ 
    int option = 0;
    int read_device = 0;
    int verbose = 0;
    int probe = 0;
    char* write_string = NULL;
    char* serial_device = NULL;
    prokhz_ctx* ctx = NULL;

    while ((option = getopt(argc, argv,"pw:vr")) != -1) {
        switch (option) {
            case 'v' : verbose = 1;
                break;
            case 'r' : read_device = 1;
                break;
            case 'p' : probe = 1;
                break;
            case 'w' : write_string = optarg;
                break;
            default: ; print_usage(); 
                 exit(EXIT_FAILURE);
        }
    }

    if (argc < 2) {
        print_usage();
        goto exit;
    }

    if (probe) {
        ctx = libprokhz_init(serial_device);
        if (ctx)
            fprintf(stdout, "device %s found\n", ctx->name);
	ctx->em4100_read(ctx, (uint8_t*)"123678");

    }
exit:
    if (ctx)
        free (ctx);
    return 0; 
};
