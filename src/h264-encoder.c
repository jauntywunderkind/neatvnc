#include "h264-encoder.h"
#include "neatvnc.h"
#include "fb.h"
#include "sys/queue.h"

#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <unistd.h>
#include <assert.h>
#include <gbm.h>
#include <aml.h>

#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_drm.h>
#include <libavutil/pixdesc.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>

#include <libdrm/drm_fourcc.h>

struct h264_encoder;

struct fb_queue_entry {
	struct nvnc_fb* fb;
	TAILQ_ENTRY(fb_queue_entry) link;
};

TAILQ_HEAD(fb_queue, fb_queue_entry);

struct h264_encoder {
	h264_encoder_packet_handler_fn on_packet_ready;
	void* userdata;

	uint32_t width;
	uint32_t height;
	uint32_t format;

	AVRational timebase;
	AVRational sample_aspect_ratio;
	enum AVPixelFormat av_pixel_format;

	/* type: AVHWDeviceContext */
	AVBufferRef* hw_device_ctx;

	/* type: AVHWFramesContext */
	AVBufferRef* hw_frames_ctx;

	AVCodecContext* codec_ctx;

	AVFilterGraph* filter_graph;
	AVFilterContext* filter_in;
	AVFilterContext* filter_out;

	bool next_frame_should_be_keyframe;
	struct fb_queue fb_queue;

	struct aml_work* work;
	struct nvnc_fb* current_fb;
	AVPacket* current_packet;
	bool current_frame_is_keyframe;
};

static enum AVPixelFormat drm_to_av_pixel_format(uint32_t format)
{
	switch (format) {
	case DRM_FORMAT_XRGB8888:
	case DRM_FORMAT_ARGB8888:
		return AV_PIX_FMT_BGR0;
	case DRM_FORMAT_XBGR8888:
	case DRM_FORMAT_ABGR8888:
		return AV_PIX_FMT_RGB0;
	case DRM_FORMAT_RGBX8888:
	case DRM_FORMAT_RGBA8888:
		return AV_PIX_FMT_0BGR;
	case DRM_FORMAT_BGRX8888:
	case DRM_FORMAT_BGRA8888:
		return AV_PIX_FMT_0RGB;
	}

	return AV_PIX_FMT_NONE;
}

// TODO: Maybe do this once per frame inside nvnc_fb?
static AVFrame* fb_to_avframe(struct nvnc_fb* fb)
{
	struct gbm_bo* bo = fb->bo;

	AVBufferRef* desc_ref = av_buffer_allocz(sizeof(AVDRMFrameDescriptor));
	if (!desc_ref)
		return NULL;

	AVDRMFrameDescriptor* desc = (AVDRMFrameDescriptor*)desc_ref->data;

	AVFrame* frame = av_frame_alloc();
	if (!frame) {
		av_buffer_unref(&desc_ref);
		return NULL;
	}

	int n_planes = gbm_bo_get_plane_count(bo);

	desc->nb_objects = n_planes;

	desc->nb_layers = 1;
	desc->layers[0].format = gbm_bo_get_format(bo);
	desc->layers[0].nb_planes = n_planes;

	for (int i = 0; i < n_planes; ++i) {
		uint32_t stride = gbm_bo_get_stride_for_plane(bo, i);

		// TODO: Does libav clean up this fd?
		desc->objects[i].fd = gbm_bo_get_fd_for_plane(bo, i);
		desc->objects[i].size = stride * fb->height;
		desc->objects[i].format_modifier = gbm_bo_get_modifier(bo);

		desc->layers[0].planes[i].object_index = i;
		desc->layers[0].planes[i].offset = gbm_bo_get_offset(bo, i);
		desc->layers[0].planes[i].pitch = stride;
	}

	frame->opaque = fb;
	frame->width = fb->width;
	frame->height = fb->height;
	frame->format = AV_PIX_FMT_DRM_PRIME;
	frame->sample_aspect_ratio = (AVRational){1, 1};
	frame->buf[0] = desc_ref;
	frame->data[0] = (void*)desc;

	// TODO: Set colorspace?

	return frame;
}

static struct nvnc_fb* fb_queue_dequeue(struct fb_queue* queue)
{
	if (TAILQ_EMPTY(queue))
		return NULL;

	struct fb_queue_entry* entry = TAILQ_FIRST(queue);
	TAILQ_REMOVE(queue, entry, link);
	struct nvnc_fb* fb = entry->fb;
	free(entry);

	return fb;
}

static int fb_queue_enqueue(struct fb_queue* queue, struct nvnc_fb* fb)
{
	struct fb_queue_entry* entry = calloc(1, sizeof(*entry));
	if (!entry)
		return -1;

	entry->fb = fb;
	nvnc_fb_ref(fb);
	TAILQ_INSERT_TAIL(queue, entry, link);

	return 0;
}

static int h264_encoder__init_filters(struct h264_encoder* self)
{
	int rc;

	self->filter_graph = avfilter_graph_alloc();
	if (!self->filter_graph)
		return -1;

	char options[256];
	snprintf(options, sizeof(options),
			"video_size=%"PRIu32"x%"PRIu32":pix_fmt=%s"
			":time_base=%d/%d:pixel_aspect=%d/%d",
			self->width, self->height,
			av_get_pix_fmt_name(self->av_pixel_format),
			self->timebase.num, self->timebase.den,
			self->sample_aspect_ratio.num,
			self->sample_aspect_ratio.den);

	rc = avfilter_graph_create_filter(&self->filter_in,
			avfilter_get_by_name("buffer"), "in", options, NULL,
			self->filter_graph);
	if (rc != 0)
		goto failure;

	rc = avfilter_graph_create_filter(&self->filter_out,
			avfilter_get_by_name("buffersink"), "out", options,
			NULL, self->filter_graph);
	if (rc != 0)
		goto failure;

	AVFilterInOut* inputs = avfilter_inout_alloc();
	if (!inputs)
		goto failure;

	inputs->name = "in";
	inputs->filter_ctx = self->filter_in;
	inputs->pad_idx = 0;
	inputs->next = NULL;

	AVFilterInOut* outputs = avfilter_inout_alloc();
	if (!outputs) {
		avfilter_inout_free(&inputs);
		goto failure;
	}

	outputs->name = "out";
	outputs->filter_ctx = self->filter_out;
	outputs->pad_idx = 0;
	outputs->next = NULL;

	rc = avfilter_graph_parse(self->filter_graph,
			"hwmap,scale_vaapi=format=nv12:mode=fast",
			outputs, inputs, NULL);

	avfilter_inout_free(&outputs);
	avfilter_inout_free(&inputs);

	if (rc != 0)
		goto failure;

	assert(self->hw_device_ctx);

	for (unsigned int i = 0; i < self->filter_graph->nb_filters; ++i) {
		self->filter_graph->filters[i]->hw_device_ctx =
			av_buffer_ref(self->hw_device_ctx);
	}

	rc = avfilter_graph_config(self->filter_graph, NULL);
	if (rc != 0)
		goto failure;

	return 0;

failure:
	avfilter_graph_free(&self->filter_graph);
	return -1;
}

static int h264_encoder__init_codec_context(struct h264_encoder* self,
		const AVCodec* codec)
{
	self->codec_ctx = avcodec_alloc_context3(codec);
	if (!self->codec_ctx)
		return -1;

	struct AVCodecContext* c = self->codec_ctx;
	c->width = self->width;
	c->height = self->width;
	c->time_base = self->timebase;
	c->sample_aspect_ratio = self->sample_aspect_ratio;
	c->pix_fmt = AV_PIX_FMT_VAAPI;
	c->gop_size = INT32_MAX; /* We'll select key frames manually */
	c->max_b_frames = 0; /* B-frames are bad for latency */

	/* open-h264 requires baseline profile, so we use constrained
	 * baseline.
	 */
	c->profile = 578;

	return 0;
}

static int h264_encoder__init_hw_frames_context(struct h264_encoder* self)
{
	AVHWFramesContext* c = (AVHWFramesContext*)self->hw_frames_ctx->data;
	c->format = AV_PIX_FMT_VAAPI;
	c->sw_format = AV_PIX_FMT_NV12;
	c->width = self->width;
	c->height = self->height;
	
	return av_hwframe_ctx_init(self->hw_frames_ctx);
}

static int h264_encoder__schedule_work(struct h264_encoder* self)
{
	if (self->current_fb)
		return 0;

	self->current_fb = fb_queue_dequeue(&self->fb_queue);
	if (!self->current_fb)
		return 0;

	self->current_frame_is_keyframe = self->next_frame_should_be_keyframe;
	self->next_frame_should_be_keyframe = false;

	return aml_start(aml_get_default(), self->work);
}

static int h264_encoder__encode(struct h264_encoder* self,
		AVFrame* frame_in, AVPacket* packet_out)
{
	int rc;

	rc = av_buffersrc_add_frame_flags(self->filter_in, frame_in,
			AV_BUFFERSRC_FLAG_KEEP_REF);
	if (rc != 0)
		return -1;

	AVFrame* filtered_frame = av_frame_alloc();
	if (!filtered_frame)
		return -1;

	rc = av_buffersink_get_frame(self->filter_out, filtered_frame);
	if (rc != 0)
		goto get_frame_failure;

	rc = avcodec_send_frame(self->codec_ctx, filtered_frame);
	if (rc != 0)
		goto send_frame_failure;

	rc = avcodec_receive_packet(self->codec_ctx, packet_out);

send_frame_failure:
	av_frame_unref(filtered_frame);
get_frame_failure:
	av_frame_free(&filtered_frame);
	return rc;
}

static void h264_encoder__do_work(void* handle)
{
	struct h264_encoder* self = aml_get_userdata(handle);

	AVFrame* frame = fb_to_avframe(self->current_fb);
	assert(frame); // TODO

	frame->hw_frames_ctx = av_buffer_ref(self->hw_frames_ctx);

	if (self->current_frame_is_keyframe) {
		frame->key_frame = 1;
		frame->pict_type = AV_PICTURE_TYPE_I;
	} else {
		frame->key_frame = 0;
		frame->pict_type = AV_PICTURE_TYPE_P;
	}

	AVPacket* packet = av_packet_alloc();
	assert(packet); // TODO

	int rc = h264_encoder__encode(self, frame, packet);
	if (rc == 0) {
		// TODO: log failure
		av_packet_free(&packet);
		goto failure;
	}

	self->current_packet = packet;

failure:
	av_frame_unref(frame);
	av_frame_free(&frame);
}

static void h264_encoder__on_work_done(void* handle)
{
	struct h264_encoder* self = aml_get_userdata(handle);

	nvnc_fb_release(self->current_fb);
	nvnc_fb_unref(self->current_fb);
	self->current_fb = NULL;

	if (!self->current_packet)
		return;

	self->on_packet_ready(self->current_packet->data,
			self->current_packet->size, self->userdata);
	self->current_packet = NULL;

	h264_encoder__schedule_work(self);
}

struct h264_encoder* h264_encoder_create(uint32_t width, uint32_t height,
		uint32_t format)
{
	int rc;

	struct h264_encoder* self = calloc(1, sizeof(*self));
	if (!self)
		return NULL;

	self->work = aml_work_new(h264_encoder__do_work,
			h264_encoder__on_work_done, self, NULL);
	if (!self->work)
		goto worker_failure;

	rc = av_hwdevice_ctx_create(&self->hw_device_ctx,
			AV_HWDEVICE_TYPE_VAAPI, NULL, NULL, 0);
	if (rc != 0)
		goto hwdevice_ctx_failure;

	self->next_frame_should_be_keyframe = true;
	TAILQ_INIT(&self->fb_queue);

	self->width = width;
	self->height = height;
	self->format = format;
	self->timebase = (AVRational){1, 1000000};
	self->sample_aspect_ratio = (AVRational){1, 1};
	self->av_pixel_format = drm_to_av_pixel_format(format);
	if (self->av_pixel_format == AV_PIX_FMT_NONE)
		goto pix_fmt_failure;

	const AVCodec* codec = avcodec_find_encoder_by_name("h264_vaapi");
	if (!codec)
		goto codec_failure;

	if (h264_encoder__init_filters(self) < 0)
		goto filter_failure;

	if (h264_encoder__init_codec_context(self, codec) < 0)
		goto codec_context_failure;

	if (h264_encoder__init_hw_frames_context(self) < 0)
		goto hw_frames_context_failure;

	self->codec_ctx->hw_frames_ctx = av_buffer_ref(self->hw_frames_ctx);

	rc = avcodec_open2(self->codec_ctx, codec, NULL);
	if (rc != 0)
		goto avcodec_open_failure;

	return self;

avcodec_open_failure:
	av_buffer_unref(&self->hw_frames_ctx);
hw_frames_context_failure:
	avcodec_free_context(&self->codec_ctx);
codec_context_failure:
filter_failure:
codec_failure:
pix_fmt_failure:
	av_buffer_unref(&self->hw_device_ctx);
hwdevice_ctx_failure:
	aml_unref(self->work);
worker_failure:
	free(self);
	return NULL;
}

void h264_encoder_destroy(struct h264_encoder* self)
{
	av_buffer_unref(&self->hw_frames_ctx);
	avcodec_free_context(&self->codec_ctx);
	av_buffer_unref(&self->hw_device_ctx);
	avfilter_graph_free(&self->filter_graph);
	aml_unref(self->work);
	free(self);
}

void h264_encoder_set_packet_handler_fn(struct h264_encoder* self,
		h264_encoder_packet_handler_fn value)
{
	self->on_packet_ready = value;
}

void h264_encoder_set_userdata(struct h264_encoder* self, void* value)
{
	self->userdata = value;
}

void h264_encoder_request_keyframe(struct h264_encoder* self)
{
	self->next_frame_should_be_keyframe = true;
}

void h264_encoder_feed(struct h264_encoder* self, struct nvnc_fb* fb)
{
	assert(fb->type == NVNC_FB_GBM_BO);

	// TODO: Add transform filter
	assert(fb->transform == NVNC_TRANSFORM_NORMAL);

	int rc = fb_queue_enqueue(&self->fb_queue, fb);
	assert(rc == 0); // TODO

	nvnc_fb_hold(fb);

	rc = h264_encoder__schedule_work(self);
	assert(rc == 0); // TODO
}
