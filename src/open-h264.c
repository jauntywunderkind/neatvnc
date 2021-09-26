/*
 * Copyright (c) 2021 Andri Yngvason
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES WITH
 * REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY
 * AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT,
 * INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM
 * LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE
 * OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
 * PERFORMANCE OF THIS SOFTWARE.
 */

#include "open-h264.h"

#include "h264-encoder.h"
#include "rfb-proto.h"
#include "enc-util.h"
#include "vec.h"
#include "fb.h"

enum open_h264_flags {
	OPEN_H264_FLAG_RESET_CONTEXT = 0,
	OPEN_H264_FLAG_RESET_ALL_CONTEXTS = 1,
};

struct open_h264_header {
	uint32_t length;
	uint32_t flags;
};

static int open_h264_encode_header(struct open_h264* self, struct vec* dst,
		size_t payload_size, uint32_t flags)
{
	int rc;

	rc = encode_rect_count(dst, 1);
	if (rc < 0)
		return -1;

	rc = encode_rect_head(dst, RFB_ENCODING_OPEN_H264, 0, 0, self->width,
			self->height);
	if (rc < 0)
		return -1;

	struct open_h264_header* header = vec_append_zero(dst,
			sizeof(struct open_h264_header));
	if (!header)
		return -1;

	header->length = htonl(payload_size);
	header->flags = htonl(flags);

	return 0;
}

static void open_h264_handle_packet(const void* data, size_t size,
		void* userdata)
{
	struct open_h264* self = userdata;

	vec_append(&self->pending, data, size);
	self->on_ready(self->userdata);
}

int open_h264_init(struct open_h264* self)
{
	if (vec_init(&self->pending, 4090) < 0)
		return -1;

	if (self->width && self->height && self->format) {
		if (h264_encoder_create(self->width, self->height, self->format)) {
			vec_destroy(&self->pending);
			return -1;
		}

		h264_encoder_set_userdata(self->encoder, self);
		h264_encoder_set_packet_handler_fn(self->encoder,
				open_h264_handle_packet);
	}

	return 0;
}

void open_h264_destroy(struct open_h264* self)
{
	if (self->encoder)
		h264_encoder_destroy(self->encoder);
	vec_destroy(&self->pending);
}

static int open_h264_resize(struct open_h264* self, struct nvnc_fb* fb)
{
	struct h264_encoder* encoder = h264_encoder_create(fb->width,
			fb->height, fb->fourcc_format);
	if (!encoder)
		return -1;

	if (self->encoder)
		h264_encoder_destroy(self->encoder);

	h264_encoder_set_userdata(encoder, self);
	h264_encoder_set_packet_handler_fn(encoder, open_h264_handle_packet);

	self->encoder = encoder;

	self->width = fb->width;
	self->height = fb->height;
	self->format = fb->fourcc_format;
	self->needs_reset = true;

	return 0;
}

int open_h264_feed_frame(struct open_h264* self, struct nvnc_fb* fb)
{
	if (fb->width != self->width || fb->height != self->height ||
			fb->fourcc_format != self->format) {
		if (open_h264_resize(self, fb) < 0)
			return -1;
	}

	assert(self->width && self->height && self->on_ready);

	// TODO: encoder_feed should return an error code
	h264_encoder_feed(self->encoder, fb);
	return -1;
}

int open_h264_read(struct open_h264* self, struct vec* buffer)
{
	if (self->pending.len == 0) {
		return 0;
	}

	vec_clear(buffer);

	uint32_t flags = self->needs_reset ? OPEN_H264_FLAG_RESET_CONTEXT : 0;
	self->needs_reset = false;

	if (open_h264_encode_header(self, buffer, self->pending.len, flags) < 0)
		return -1;

	if (vec_append(buffer, self->pending.data, self->pending.len) < 0)
		return -1;

	vec_clear(&self->pending);

	return 1;
}
