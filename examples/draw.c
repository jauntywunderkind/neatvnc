/*
 * Copyright (c) 2019 - 2021 Andri Yngvason
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

#include "neatvnc.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <tgmath.h>
#include <aml.h>
#include <signal.h>
#include <assert.h>
#include <pixman.h>
#include <sys/param.h>
#include <libdrm/drm_fourcc.h>

#include "sys/queue.h"

// TODO: Align pixel formats

struct coord {
	int x, y;
};

struct fb_side_data {
	struct pixman_region16 damage;
	LIST_ENTRY(fb_side_data) link;
};

LIST_HEAD(fb_side_data_list, fb_side_data);

struct draw {
	int width;
	int height;
	uint32_t format;

	pixman_image_t* whiteboard;
	uint32_t* whiteboard_buffer;

	struct nvnc_display* display;
	struct nvnc_fb_pool* fb_pool;

	struct fb_side_data_list fb_side_data_list;
};

static void fb_side_data_destroy(void* userdata)
{
	struct fb_side_data* fb_side_data = userdata;
	LIST_REMOVE(fb_side_data, link);
	pixman_region_fini(&fb_side_data->damage);
	free(fb_side_data);
}

static int coord_distance_between(struct coord a, struct coord b)
{
	float x = abs(a.x - b.x);
	float y = abs(a.y - b.y);
	return round(sqrt(x * x + y * y));
}

static void damage_all_buffers(struct draw* draw,
		struct pixman_region16* region)
{
	struct fb_side_data *item;
	LIST_FOREACH(item, &draw->fb_side_data_list, link)
		pixman_region_union(&item->damage, &item->damage, region);
}

static void update_vnc_buffer(struct draw* draw,
		struct pixman_region16* frame_damage)
{
	struct nvnc_fb *fb = nvnc_fb_pool_acquire(draw->fb_pool);
	assert(fb);

	struct fb_side_data* fb_side_data = nvnc_get_userdata(fb);
	if (!fb_side_data) {
		fb_side_data = calloc(1, sizeof(*fb_side_data));
		assert(fb_side_data);

		/* This is a new buffer, so the whole surface is damaged. */
		pixman_region_init_rect(&fb_side_data->damage, 0, 0,
				draw->width, draw->height);

		nvnc_set_userdata(fb, fb_side_data, fb_side_data_destroy);
		LIST_INSERT_HEAD(&draw->fb_side_data_list, fb_side_data, link);
	}

	pixman_image_t* dstimg = pixman_image_create_bits_no_clear(
			PIXMAN_r8g8b8x8, draw->width, draw->height,
			nvnc_fb_get_addr(fb), 4 * draw->width);

	/* Clip region is set to limit copying to only the damaged region. */
	pixman_image_set_clip_region(dstimg, &fb_side_data->damage);

	pixman_image_composite(PIXMAN_OP_OVER, draw->whiteboard, NULL, dstimg,
			0, 0,
			0, 0,
			0, 0,
			draw->width, draw->height);

	pixman_image_unref(dstimg);

	/* The buffer is now up to date, so the damage region can be cleared. */
	pixman_region_clear(&fb_side_data->damage);

	nvnc_display_feed_buffer(draw->display, fb, frame_damage);
	nvnc_fb_unref(fb);
}

static void composite_dot(struct draw *draw, uint32_t* image,
		struct coord coord, int radius, uint32_t colour,
		struct pixman_region16* damage)
{
	int width = draw->width;
	int height = draw->height;

	struct coord start, stop;

	start.x = MAX(0, coord.x - radius);
	start.y = MAX(0, coord.y - radius);
	stop.x = MIN(width, coord.x + radius);
	stop.y = MIN(height, coord.y + radius);

	/* The brute force method. ;) */
	for (int y = start.y; y < stop.y; ++y)
		for (int x = start.x; x < stop.x; ++x) {
			struct coord point = { .x = x, .y = y };
			if (coord_distance_between(point, coord) <= radius)
				image[x + y * width] = colour;
		}

	pixman_region_init_rect(damage, start.x, start.y,
	                        stop.x - start.x, stop.y - start.y);
}

static void draw_dot(struct draw *draw, struct coord coord, int radius,
		uint32_t colour)
{
	struct pixman_region16 region;
	composite_dot(draw, draw->whiteboard_buffer, coord, radius, colour,
			&region);

	/* All the buffers that are currently in the pool will need to be
	 * upgraded in this region before being sent to nvnc.
	 */
	damage_all_buffers(draw, &region);

	update_vnc_buffer(draw, &region);

	pixman_region_fini(&region);
}

static void on_pointer_event(struct nvnc_client* client, uint16_t x, uint16_t y,
                             enum nvnc_button_mask buttons)
{
	if (!(buttons & NVNC_BUTTON_LEFT))
		return;

	struct nvnc* server = nvnc_client_get_server(client);
	assert(server);

	struct draw* draw = nvnc_get_userdata(server);
	assert(draw);

	struct coord coord = { x, y };
	draw_dot(draw, coord, 16, 0);
}

static void on_sigint()
{
	aml_exit(aml_get_default());
}

int main(int argc, char* argv[])
{
	struct draw draw = { 0 };

	LIST_INIT(&draw.fb_side_data_list);

	struct aml* aml = aml_new();
	aml_set_default(aml);

	draw.width = 500;
	draw.height = 500;
	draw.format = DRM_FORMAT_RGBX8888;

	draw.whiteboard_buffer = malloc(draw.width * draw.height * 4);
	assert(draw.whiteboard_buffer);

	memset(draw.whiteboard_buffer, 0xff, draw.width * draw.height * 4);

	draw.whiteboard = pixman_image_create_bits_no_clear(PIXMAN_r8g8b8x8,
			draw.width, draw.height, draw.whiteboard_buffer,
			draw.width * 4);
	assert(draw.whiteboard);

	draw.fb_pool = nvnc_fb_pool_new(draw.width, draw.height, draw.format,
			draw.width);
	assert(draw.fb_pool);

	struct nvnc* server = nvnc_open("127.0.0.1", 5900);
	assert(server);

	draw.display = nvnc_display_new(0, 0);
	assert(draw.display);

	nvnc_add_display(server, draw.display);

	nvnc_set_name(server, "Draw");
	nvnc_set_pointer_fn(server, on_pointer_event);
	nvnc_set_userdata(server, &draw, NULL);

	struct aml_signal* sig = aml_signal_new(SIGINT, on_sigint, NULL, NULL);
	aml_start(aml_get_default(), sig);
	aml_unref(sig);

	struct pixman_region16 damage;
	pixman_region_init_rect(&damage, 0, 0, draw.width, draw.height);
	update_vnc_buffer(&draw, &damage);
	pixman_region_fini(&damage);

	aml_run(aml);

	nvnc_close(server);
	nvnc_display_unref(draw.display);
	nvnc_fb_pool_unref(draw.fb_pool);
	pixman_image_unref(draw.whiteboard);
	free(draw.whiteboard_buffer);
	aml_unref(aml);
}
