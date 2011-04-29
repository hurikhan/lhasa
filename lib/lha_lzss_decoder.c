/*

Copyright (c) 2011, Simon Howard

Permission to use, copy, modify, and/or distribute this software
for any purpose with or without fee is hereby granted, provided
that the above copyright notice and this permission notice appear
in all copies.

THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL
WARRANTIES WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE
AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT, INDIRECT, OR
CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM
LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT,
NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.

 */

#include <stdlib.h>
#include <string.h>
#include <inttypes.h>

#include "lha_lzss_decoder.h"

// Parameters for ring buffer, used for storing history.  This acts
// as the dictionary for copy operations.

#define RING_BUFFER_SIZE 4096
#define START_OFFSET 18

// Threshold offset.  In the copy operation, the copy length is a 4-bit
// value, giving a range 0..15.  The threshold offsets this so that it
// is interpreted as 3..18 - a more useful range.

#define THRESHOLD 3

// Size of output buffer.  Must be large enough to hold the results of
// a complete "run" (see below).

#define OUTPUT_BUFFER_SIZE (15 + THRESHOLD) * 8

// LZSS decoder, for the -lz5- compression method used by LArc.
//
// This processes "runs" of eight commands, each of which is either
// "output a character" or "copy block".  The result of that run
// is written into outbuf.  Successive calls to lha_lzss_read empty
// outbuf until another run is processed.

typedef struct {
	uint8_t ringbuf[RING_BUFFER_SIZE];
	unsigned int ringbuf_pos;
	uint8_t outbuf[OUTPUT_BUFFER_SIZE];
	unsigned int outbuf_pos, outbuf_len;
} LHALZSSDecoder;

static int lha_lzss_init(void *data)
{
	LHALZSSDecoder *decoder = data;

	memset(decoder->ringbuf, ' ', RING_BUFFER_SIZE);
	decoder->ringbuf_pos = RING_BUFFER_SIZE - START_OFFSET;

	decoder->outbuf_pos = 0;
	decoder->outbuf_len = 0;

	return 1;
}

// Add a single byte to the output buffer.

static void output_byte(LHALZSSDecoder *decoder, uint8_t b)
{
	decoder->outbuf[decoder->outbuf_len] = b;
	++decoder->outbuf_len;

	decoder->ringbuf[decoder->ringbuf_pos] = b;
	decoder->ringbuf_pos = (decoder->ringbuf_pos + 1) % RING_BUFFER_SIZE;
}

// Output a "block" of data from the specified range in the ring buffer.

static void output_block(LHALZSSDecoder *decoder, unsigned int start,
                         unsigned int len)
{
	unsigned int i;

	for (i = 0; i < len; ++i) {
		output_byte(decoder,
		            decoder->ringbuf[(start + i) % RING_BUFFER_SIZE]);
	}
}

// Process a "run" of LZSS-compressed data (a control byte followed by
// eight "commands").

static int process_run(LHALZSSDecoder *decoder,
                       LHADecoderCallback callback,
                       void *callback_data)
{
	uint8_t bitmap;
	unsigned int bit;

	// Initialize outbuf.

	decoder->outbuf_len = 0;

	// Read the bitmap byte first.

	if (!callback(&bitmap, 1, callback_data)) {
		return 0;
	}

	// Each bit in the bitmap is a command.
	// If the bit is set, it is an "output byte" command.
	// If it is not set, it is a "copy block" command.

	for (bit = 0; bit < 8; ++bit) {
		if ((bitmap & (1 << bit)) != 0) {
			uint8_t b;

			if (!callback(&b, 1, callback_data)) {
				return 0;
			}

			output_byte(decoder, b);
		} else {
			uint8_t cmd[2];
			unsigned int seqstart, seqlen;

			if (!callback(cmd, 2, callback_data)) {
				return 0;
			}

			seqstart = ((cmd[1] & 0xf0) << 4) | cmd[0];
			seqlen = (cmd[1] & 0x0f) + THRESHOLD;

			output_block(decoder, seqstart, seqlen);
		}
	}

	return 1;
}

static size_t lha_lzss_read(void *data, uint8_t *buf, size_t buf_len,
                            LHADecoderCallback callback, void *callback_data)
{
	LHALZSSDecoder *decoder = data;
	size_t filled, bytes;

	filled = 0;

	while (filled < buf_len) {

		// Try to empty out some of the output buffer first.

		bytes = decoder->outbuf_len - decoder->outbuf_pos;

		if (buf_len - filled < bytes) {
			bytes = buf_len - filled;
		}

		memcpy(buf + filled, decoder->outbuf + decoder->outbuf_pos,
		       bytes);
		decoder->outbuf_pos += bytes;
		filled += bytes;

		// If outbuf is now empty, we can process another run to
		// re-fill it.

		if (decoder->outbuf_pos >= decoder->outbuf_len) {
			process_run(decoder, callback, callback_data);
			decoder->outbuf_pos = 0;
		}

		// No more data to be read?

		if (decoder->outbuf_len == 0) {
			break;
		}
	}

	return filled;
}

LHADecoderType lha_lzss_decoder = {
	lha_lzss_init,
	NULL,
	lha_lzss_read,
	sizeof(LHALZSSDecoder)
};

