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

#ifndef INCLUDE_LIBPROKHZ_INTERNAL_H_
#define INCLUDE_LIBPROKHZ_INTERNAL_H_

#include "libprokhz.h"

#define PROKHZ_DEVICES \
		DECL(ctx_idrw_203)


typedef struct {
	char name[256];
	int (*probe)();
	int (*init)(prokhz_ctx* ctx);
	int (*em4100_read)(prokhz_ctx* ctx, uint8_t *hex_buf);
	int (*em4100_write)(prokhz_ctx* ctx, uint8_t *hex_buf);
} hw_device;


#define DECL(name) extern hw_device name;
PROKHZ_DEVICES
#undef DECL

#endif // INCLUDE_LIBPROKHZ_INTERNAL_H_
