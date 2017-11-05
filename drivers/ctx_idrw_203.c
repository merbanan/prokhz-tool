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

#include "../libprokhz_internal.h"
#include "../libprokhz.h"


int ctx_idrw_203_probe() {
           fprintf(stdout, "probing()\n");
	
	return 1;
};

int dummy_read(uint8_t *hex_buf) {
        fprintf(stdout, "dummy_read() %s\n", hex_buf);
	
	return 1;
};



hw_device ctx_idrw_203 = {
//	.name = "ctx-idrw-203",
	.name = "1234567890",
	.probe = ctx_idrw_203_probe,
	.em4100_read = dummy_read,
};

hw_device p1d = {
//	.name = "ctx-idrw-203",
	.name = "0987654321",
	.probe = ctx_idrw_203_probe,
	.em4100_read = dummy_read,
};
