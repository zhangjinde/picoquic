/*
* Author: Christian Huitema
* Copyright (c) 2017, Private Octopus, Inc.
* All rights reserved.
*
* Permission to use, copy, modify, and distribute this software for any
* purpose with or without fee is hereby granted, provided that the above
* copyright notice and this permission notice appear in all copies.
*
* THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
* ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
* WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
* DISCLAIMED. IN NO EVENT SHALL Private Octopus, Inc. BE LIABLE FOR ANY
* DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
* (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
* LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
* ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
* (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
* SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

/* Simple set of utilities */

#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include "picoquic_internal.h"

char * picoquic_string_create(const char * original, size_t len)
{
	char * str = (char *)malloc(len + 1);

	if (str != NULL)
	{
		if (original == NULL || len == 0)
		{
			str[0] = 0;
		}
		else
		{
			memcpy(str, original, len);
			str[len] = 0;
		}
	}

	return str;
}

char * picoquic_string_duplicate(const char * original)
{
	char * str = NULL;

	if (original != NULL)
	{
		size_t len = strlen(original);

		str = picoquic_string_create(original, len);
	}

	return str;
}

static FILE *debug_out;
static int debug_suspended = 0;

void debug_printf(const char *fmt, ...)
{
    if (debug_suspended == 0)
    {
        va_list args;
        va_start(args, fmt);
        vfprintf(debug_out ? debug_out : stderr, fmt, args);
        va_end(args);
    }
}

void debug_printf_push_stream(FILE *f)
{
	if (debug_out)
	{
		fprintf(stderr, "Nested err out not supported\n");
		exit(1);
	}
	debug_out = f;
}

void debug_printf_pop_stream(void)
{
	if (debug_out == NULL)
	{
		fprintf(stderr, "No current err out\n");
		exit(1);
	}
	debug_out = NULL;
}

void debug_printf_suspend(void)
{
    debug_suspended = 1;
}

void debug_printf_resume(void)
{
    debug_suspended = 0;
}