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

#pragma once

#include "vec.h"

#include <stdint.h>
#include <stdbool.h>

struct open_h264_header;
struct h264_encoder;
struct nvnc_fb;

typedef void (*open_h264_ready_fn)(void*);

struct open_h264 {
	struct h264_encoder* encoder;

	struct vec pending;

	uint32_t width;
	uint32_t height;
	uint32_t format;

	bool needs_reset;

	open_h264_ready_fn on_ready;
	void* userdata;
};

int open_h264_init(struct open_h264*);
void open_h264_destroy(struct open_h264*);

int open_h264_feed_frame(struct open_h264*, struct nvnc_fb*);

int open_h264_read(struct open_h264*, struct vec* buffer);

void open_h264_request_keyframe(struct open_h264*);
