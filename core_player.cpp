#include "core_player.h"
#include <functional>
#include "common_utils.h"
#include <thread>

CorePlayer::CorePlayer()
{
	for (int i = 0; i < AVMEDIA_TYPE_NB; i++)
	{
		wanted_stream_spec[i] = NULL;
	}
}

CorePlayer::~CorePlayer()
{

}

int CorePlayer::opt_add_vfilter(void *optctx, const char *opt, const char *arg)
{
	grow_array(vfilters_list, sizeof(*vfilters_list), &nb_vfilters, nb_vfilters + 1);
	vfilters_list[nb_vfilters - 1] = arg;
	return 0;
}

void CorePlayer::free_picture(Frame *vp)
{
	if (vp->bmp) {
		SDL_DestroyTexture(vp->bmp);
		vp->bmp = NULL;
	}
}

int CorePlayer::packet_queue_put_private(PacketQueue *q, AVPacket *pkt)
{
	MyAVPacketList *pkt1;

	if (q->abort_request)
		return -1;

	pkt1 = (MyAVPacketList*)av_malloc(sizeof(MyAVPacketList));
	if (!pkt1)
		return -1;
	pkt1->pkt = *pkt;
	pkt1->next = NULL;
	if (pkt == &flush_pkt)
		q->serial++;
	pkt1->serial = q->serial;

	if (!q->last_pkt)
		q->first_pkt = pkt1;
	else
		q->last_pkt->next = pkt1;
	q->last_pkt = pkt1;
	q->nb_packets++;
	q->size += pkt1->pkt.size + sizeof(*pkt1);
	q->duration += pkt1->pkt.duration;
	/* XXX: should duplicate packet data in DV case */
	SDL_CondSignal(q->cond);
	return 0;
}

int CorePlayer::packet_queue_put(PacketQueue *q, AVPacket *pkt)
{
	int ret;

	SDL_LockMutex(q->mutex);
	ret = packet_queue_put_private(q, pkt);
	SDL_UnlockMutex(q->mutex);

	if (pkt != &flush_pkt && ret < 0)
		av_packet_unref(pkt);

	return ret;
}

int CorePlayer::packet_queue_put_nullpacket(PacketQueue *q, int stream_index)
{
	AVPacket pkt1, *pkt = &pkt1;
	av_init_packet(pkt);
	pkt->data = NULL;
	pkt->size = 0;
	pkt->stream_index = stream_index;
	return packet_queue_put(q, pkt);
}

int CorePlayer::packet_queue_init(PacketQueue *q)
{
	memset(q, 0, sizeof(PacketQueue));
	q->mutex = SDL_CreateMutex();
	if (!q->mutex) {
		av_log(NULL, AV_LOG_FATAL, "SDL_CreateMutex(): %s\n", SDL_GetError());
		return AVERROR(ENOMEM);
	}
	q->cond = SDL_CreateCond();
	if (!q->cond) {
		av_log(NULL, AV_LOG_FATAL, "SDL_CreateCond(): %s\n", SDL_GetError());
		return AVERROR(ENOMEM);
	}
	q->abort_request = 1;
	return 0;
}

void CorePlayer::packet_queue_flush(PacketQueue *q)
{
	MyAVPacketList *pkt, *pkt1;

	SDL_LockMutex(q->mutex);
	for (pkt = q->first_pkt; pkt; pkt = pkt1) {
		pkt1 = pkt->next;
		av_packet_unref(&pkt->pkt);
		av_freep(&pkt);
	}
	q->last_pkt = NULL;
	q->first_pkt = NULL;
	q->nb_packets = 0;
	q->size = 0;
	q->duration = 0;
	SDL_UnlockMutex(q->mutex);
}

void CorePlayer::packet_queue_destroy(PacketQueue *q)
{
	packet_queue_flush(q);
	SDL_DestroyMutex(q->mutex);
	SDL_DestroyCond(q->cond);
}

void CorePlayer::packet_queue_abort(PacketQueue *q)
{
	SDL_LockMutex(q->mutex);

	q->abort_request = 1;

	SDL_CondSignal(q->cond);

	SDL_UnlockMutex(q->mutex);
}

void CorePlayer::packet_queue_start(PacketQueue *q)
{
	SDL_LockMutex(q->mutex);
	q->abort_request = 0;
	packet_queue_put_private(q, &flush_pkt);
	SDL_UnlockMutex(q->mutex);
}

int CorePlayer::packet_queue_get(PacketQueue *q, AVPacket *pkt, int block, int *serial)
{
	MyAVPacketList *pkt1;
	int ret;

	SDL_LockMutex(q->mutex);

	for (;;) {
		if (q->abort_request) {
			ret = -1;
			break;
		}

		pkt1 = q->first_pkt;
		if (pkt1) {
			q->first_pkt = pkt1->next;
			if (!q->first_pkt)
				q->last_pkt = NULL;
			q->nb_packets--;
			q->size -= pkt1->pkt.size + sizeof(*pkt1);
			q->duration -= pkt1->pkt.duration;
			*pkt = pkt1->pkt;
			if (serial)
				*serial = pkt1->serial;
			av_free(pkt1);
			ret = 1;
			break;
		}
		else if (!block) {
			ret = 0;
			break;
		}
		else {
			SDL_CondWait(q->cond, q->mutex);
		}
	}
	SDL_UnlockMutex(q->mutex);
	return ret;
}

void CorePlayer::decoder_init(Decoder *d, AVCodecContext *avctx, PacketQueue *queue, SDL_cond *empty_queue_cond)
{
	memset(d, 0, sizeof(Decoder));
	d->avctx = avctx;
	d->queue = queue;
	d->empty_queue_cond = empty_queue_cond;
	d->start_pts = AV_NOPTS_VALUE;
}

int CorePlayer::decoder_decode_frame(Decoder *d, AVFrame *frame, AVSubtitle *sub)
{
	int got_frame = 0;

	do {
		int ret = -1;

		if (d->queue->abort_request)
			return -1;

		if (!d->packet_pending || d->queue->serial != d->pkt_serial) {
			AVPacket pkt;
			do {
				if (d->queue->nb_packets == 0)
					SDL_CondSignal(d->empty_queue_cond);
				if (packet_queue_get(d->queue, &pkt, 1, &d->pkt_serial) < 0)
					return -1;
				if (pkt.data == flush_pkt.data) {
					avcodec_flush_buffers(d->avctx);
					d->finished = 0;
					d->next_pts = d->start_pts;
					d->next_pts_tb = d->start_pts_tb;
				}
			} while (pkt.data == flush_pkt.data || d->queue->serial != d->pkt_serial);
			av_packet_unref(&d->pkt);
			d->pkt_temp = d->pkt = pkt;
			d->packet_pending = 1;
		}

		switch (d->avctx->codec_type) {
		case AVMEDIA_TYPE_VIDEO:
			ret = avcodec_decode_video2(d->avctx, frame, &got_frame, &d->pkt_temp);
			if (got_frame) {
				if (decoder_reorder_pts == -1) {
					frame->pts = av_frame_get_best_effort_timestamp(frame);
				}
				else if (!decoder_reorder_pts) {
					frame->pts = frame->pkt_dts;
				}
			}
			break;
		case AVMEDIA_TYPE_AUDIO:
			ret = avcodec_decode_audio4(d->avctx, frame, &got_frame, &d->pkt_temp);
			if (got_frame) {
				AVRational tb{ 1, frame->sample_rate };
				if (frame->pts != AV_NOPTS_VALUE)
					frame->pts = av_rescale_q(frame->pts, av_codec_get_pkt_timebase(d->avctx), tb);
				else if (d->next_pts != AV_NOPTS_VALUE)
					frame->pts = av_rescale_q(d->next_pts, d->next_pts_tb, tb);
				if (frame->pts != AV_NOPTS_VALUE) {
					d->next_pts = frame->pts + frame->nb_samples;
					d->next_pts_tb = tb;
				}
			}
			break;
		case AVMEDIA_TYPE_SUBTITLE:
			ret = avcodec_decode_subtitle2(d->avctx, sub, &got_frame, &d->pkt_temp);
			break;
		}

		if (ret < 0) {
			d->packet_pending = 0;
		}
		else {
			d->pkt_temp.dts =
				d->pkt_temp.pts = AV_NOPTS_VALUE;
			if (d->pkt_temp.data) {
				if (d->avctx->codec_type != AVMEDIA_TYPE_AUDIO)
					ret = d->pkt_temp.size;
				d->pkt_temp.data += ret;
				d->pkt_temp.size -= ret;
				if (d->pkt_temp.size <= 0)
					d->packet_pending = 0;
			}
			else {
				if (!got_frame) {
					d->packet_pending = 0;
					d->finished = d->pkt_serial;
				}
			}
		}
	} while (!got_frame && !d->finished);

	return got_frame;
}

void CorePlayer::decoder_destroy(Decoder *d)
{
	av_packet_unref(&d->pkt);
	avcodec_free_context(&d->avctx);
}

void CorePlayer::frame_queue_unref_item(Frame *vp)
{
	av_frame_unref(vp->frame);
	avsubtitle_free(&vp->sub);
}

int CorePlayer::frame_queue_init(FrameQueue *f, PacketQueue *pktq, int max_size, int keep_last)
{
	int i;
	memset(f, 0, sizeof(FrameQueue));
	if (!(f->mutex = SDL_CreateMutex())) {
		av_log(NULL, AV_LOG_FATAL, "SDL_CreateMutex(): %s\n", SDL_GetError());
		return AVERROR(ENOMEM);
	}
	if (!(f->cond = SDL_CreateCond())) {
		av_log(NULL, AV_LOG_FATAL, "SDL_CreateCond(): %s\n", SDL_GetError());
		return AVERROR(ENOMEM);
	}
	f->pktq = pktq;
	f->max_size = FFMIN(max_size, FRAME_QUEUE_SIZE);
	f->keep_last = !!keep_last;
	for (i = 0; i < f->max_size; i++)
		if (!(f->queue[i].frame = av_frame_alloc()))
			return AVERROR(ENOMEM);
	return 0;
}

void CorePlayer::frame_queue_destory(FrameQueue *f)
{
	int i;
	for (i = 0; i < f->max_size; i++) {
		Frame *vp = &f->queue[i];
		frame_queue_unref_item(vp);
		av_frame_free(&vp->frame);
		free_picture(vp);
	}
	SDL_DestroyMutex(f->mutex);
	SDL_DestroyCond(f->cond);
}

void CorePlayer::frame_queue_signal(FrameQueue *f)
{
	SDL_LockMutex(f->mutex);
	SDL_CondSignal(f->cond);
	SDL_UnlockMutex(f->mutex);
}

Frame * CorePlayer::frame_queue_peek(FrameQueue *f)
{
	return &f->queue[(f->rindex + f->rindex_shown) % f->max_size];
}

Frame * CorePlayer::frame_queue_peek_next(FrameQueue *f)
{
	return &f->queue[(f->rindex + f->rindex_shown + 1) % f->max_size];
}

Frame * CorePlayer::frame_queue_peek_last(FrameQueue *f)
{
	return &f->queue[f->rindex];
}

Frame * CorePlayer::frame_queue_peek_writable(FrameQueue *f)
{
	/* wait until we have space to put a new frame */
	SDL_LockMutex(f->mutex);
	while (f->size >= f->max_size &&
		!f->pktq->abort_request) {
		SDL_CondWait(f->cond, f->mutex);
	}
	SDL_UnlockMutex(f->mutex);

	if (f->pktq->abort_request)
		return NULL;

	return &f->queue[f->windex];
}

Frame * CorePlayer::frame_queue_peek_readable(FrameQueue *f)
{
	/* wait until we have a readable a new frame */
	SDL_LockMutex(f->mutex);
	while (f->size - f->rindex_shown <= 0 &&
		!f->pktq->abort_request) {
		SDL_CondWait(f->cond, f->mutex);
	}
	SDL_UnlockMutex(f->mutex);

	if (f->pktq->abort_request)
		return NULL;

	return &f->queue[(f->rindex + f->rindex_shown) % f->max_size];
}

void CorePlayer::frame_queue_push(FrameQueue *f)
{
	if (++f->windex == f->max_size)
		f->windex = 0;
	SDL_LockMutex(f->mutex);
	f->size++;
	SDL_CondSignal(f->cond);
	SDL_UnlockMutex(f->mutex);
}

void CorePlayer::frame_queue_next(FrameQueue *f)
{
	if (f->keep_last && !f->rindex_shown) {
		f->rindex_shown = 1;
		return;
	}
	frame_queue_unref_item(&f->queue[f->rindex]);
	if (++f->rindex == f->max_size)
		f->rindex = 0;
	SDL_LockMutex(f->mutex);
	f->size--;
	SDL_CondSignal(f->cond);
	SDL_UnlockMutex(f->mutex);
}

int CorePlayer::frame_queue_nb_remaining(FrameQueue *f)
{
	return f->size - f->rindex_shown;
}

int64_t CorePlayer::frame_queue_last_pos(FrameQueue *f)
{
	Frame *fp = &f->queue[f->rindex];
	if (f->rindex_shown && fp->serial == f->pktq->serial)
		return fp->pos;
	else
		return -1;
}

void CorePlayer::decoder_abort(Decoder *d, FrameQueue *fq)
{
	packet_queue_abort(d->queue);
	frame_queue_signal(fq);
	SDL_WaitThread(d->decoder_tid, NULL);
	d->decoder_tid = NULL;
	packet_queue_flush(d->queue);
}

int CorePlayer::realloc_texture(SDL_Texture **texture, Uint32 new_format, int new_width, int new_height, SDL_BlendMode blendmode, int init_texture)
{
	Uint32 format;
	int access, w, h;
	if (SDL_QueryTexture(*texture, &format, &access, &w, &h) < 0 || new_width != w || new_height != h || new_format != format) {
		void *pixels;
		int pitch;
		SDL_DestroyTexture(*texture);
		if (!(*texture = SDL_CreateTexture(renderer, new_format, SDL_TEXTUREACCESS_STREAMING, new_width, new_height)))
			return -1;
		if (SDL_SetTextureBlendMode(*texture, blendmode) < 0)
			return -1;
		if (init_texture) {
			if (SDL_LockTexture(*texture, NULL, &pixels, &pitch) < 0)
				return -1;
			memset(pixels, 0, pitch * new_height);
			SDL_UnlockTexture(*texture);
		}
	}
	return 0;
}

void CorePlayer::calculate_display_rect(SDL_Rect *rect, int scr_xleft, int scr_ytop, int scr_width, int scr_height, int pic_width, int pic_height, AVRational pic_sar)
{
	float aspect_ratio;
	int width, height, x, y;

	if (pic_sar.num == 0)
		aspect_ratio = 0;
	else
		aspect_ratio = av_q2d(pic_sar);

	if (aspect_ratio <= 0.0)
		aspect_ratio = 1.0;
	aspect_ratio *= (float)pic_width / (float)pic_height;

	/* XXX: we suppose the screen has a 1.0 pixel ratio */
	height = scr_height;
	width = lrint(height * aspect_ratio) & ~1;
	if (width > scr_width) {
		width = scr_width;
		height = lrint(width / aspect_ratio) & ~1;
	}
	x = (scr_width - width) / 2;
	y = (scr_height - height) / 2;
	rect->x = scr_xleft + x;
	rect->y = scr_ytop + y;
	rect->w = FFMAX(width, 1);
	rect->h = FFMAX(height, 1);
}

int CorePlayer::upload_texture(SDL_Texture *tex, AVFrame *frame, struct SwsContext **img_convert_ctx)
{
	int ret = 0;
	switch (frame->format) {
	case AV_PIX_FMT_YUV420P:
		ret = SDL_UpdateYUVTexture(tex, NULL, frame->data[0], frame->linesize[0],
			frame->data[1], frame->linesize[1],
			frame->data[2], frame->linesize[2]);
		break;
	case AV_PIX_FMT_BGRA:
		ret = SDL_UpdateTexture(tex, NULL, frame->data[0], frame->linesize[0]);
		break;
	default:
		/* This should only happen if we are not using avfilter... */
		*img_convert_ctx = sws_getCachedContext(*img_convert_ctx,
			frame->width, frame->height, (AVPixelFormat)frame->format, frame->width, frame->height,
			AV_PIX_FMT_BGRA, sws_flags, NULL, NULL, NULL);
		if (*img_convert_ctx != NULL) {
			uint8_t *pixels;
			int pitch;
			if (!SDL_LockTexture(tex, NULL, (void **)&pixels, &pitch)) {
				sws_scale(*img_convert_ctx, (const uint8_t * const *)frame->data, frame->linesize,
					0, frame->height, &pixels, &pitch);
				SDL_UnlockTexture(tex);
			}
		}
		else {
			av_log(NULL, AV_LOG_FATAL, "Cannot initialize the conversion context\n");
			ret = -1;
		}
		break;
	}
	return ret;
}

void CorePlayer::video_image_display(VideoState *is)
{
	Frame *vp;
	Frame *sp = NULL;
	SDL_Rect rect;

	vp = frame_queue_peek_last(&is->pictq);
	if (vp->bmp) {
		if (is->subtitle_st) {
			if (frame_queue_nb_remaining(&is->subpq) > 0) {
				sp = frame_queue_peek(&is->subpq);

				if (vp->pts >= sp->pts + ((float)sp->sub.start_display_time / 1000)) {
					if (!sp->uploaded) {
						uint8_t *pixels;
						int pitch;
						int i;
						if (!sp->width || !sp->height) {
							sp->width = vp->width;
							sp->height = vp->height;
						}
						if (realloc_texture(&is->sub_texture, SDL_PIXELFORMAT_ARGB8888, sp->width, sp->height, SDL_BLENDMODE_BLEND, 1) < 0)
							return;

						for (i = 0; i < sp->sub.num_rects; i++) {
							AVSubtitleRect *sub_rect = sp->sub.rects[i];

							sub_rect->x = av_clip(sub_rect->x, 0, sp->width);
							sub_rect->y = av_clip(sub_rect->y, 0, sp->height);
							sub_rect->w = av_clip(sub_rect->w, 0, sp->width - sub_rect->x);
							sub_rect->h = av_clip(sub_rect->h, 0, sp->height - sub_rect->y);

							is->sub_convert_ctx = sws_getCachedContext(is->sub_convert_ctx,
								sub_rect->w, sub_rect->h, AV_PIX_FMT_PAL8,
								sub_rect->w, sub_rect->h, AV_PIX_FMT_BGRA,
								0, NULL, NULL, NULL);
							if (!is->sub_convert_ctx) {
								av_log(NULL, AV_LOG_FATAL, "Cannot initialize the conversion context\n");
								return;
							}
							if (!SDL_LockTexture(is->sub_texture, (SDL_Rect *)sub_rect, (void **)&pixels, &pitch)) {
								sws_scale(is->sub_convert_ctx, (const uint8_t * const *)sub_rect->data, sub_rect->linesize,
									0, sub_rect->h, &pixels, &pitch);
								SDL_UnlockTexture(is->sub_texture);
							}
						}
						sp->uploaded = 1;
					}
				}
				else
					sp = NULL;
			}
		}

		calculate_display_rect(&rect, is->xleft, is->ytop, is->width, is->height, vp->width, vp->height, vp->sar);

		if (!vp->uploaded) {
			if (upload_texture(vp->bmp, vp->frame, &is->img_convert_ctx) < 0)
				return;
			vp->uploaded = 1;
		}

		SDL_RenderCopy(renderer, vp->bmp, NULL, &rect);
		if (sp) {
#if USE_ONEPASS_SUBTITLE_RENDER
			SDL_RenderCopy(renderer, is->sub_texture, NULL, &rect);
#else
			int i;
			double xratio = (double)rect.w / (double)sp->width;
			double yratio = (double)rect.h / (double)sp->height;
			for (i = 0; i < sp->sub.num_rects; i++) {
				SDL_Rect *sub_rect = (SDL_Rect*)sp->sub.rects[i];
				SDL_Rect target = { .x = rect.x + sub_rect->x * xratio,
					.y = rect.y + sub_rect->y * yratio,
					.w = sub_rect->w * xratio,
					.h = sub_rect->h * yratio };
				SDL_RenderCopy(renderer, is->sub_texture, sub_rect, &target);
			}
#endif
		}
	}
}

void CorePlayer::video_audio_display(VideoState *s)
{
	int i, i_start, x, y1, y, ys, delay, n, nb_display_channels;
	int ch, channels, h, h2;
	int64_t time_diff;
	int rdft_bits, nb_freq;

	for (rdft_bits = 1; (1 << rdft_bits) < 2 * s->height; rdft_bits++)
		;
	nb_freq = 1 << (rdft_bits - 1);

	/* compute display index : center on currently output samples */
	channels = s->audio_tgt.channels;
	nb_display_channels = channels;
	if (!s->paused) {
		int data_used = s->show_mode == SHOW_MODE_WAVES ? s->width : (2 * nb_freq);
		n = 2 * channels;
		delay = s->audio_write_buf_size;
		delay /= n;

		/* to be more precise, we take into account the time spent since
		the last buffer computation */
		if (audio_callback_time) {
			time_diff = av_gettime_relative() - audio_callback_time;
			delay -= (time_diff * s->audio_tgt.freq) / 1000000;
		}

		delay += 2 * data_used;
		if (delay < data_used)
			delay = data_used;

		i_start = x = compute_mod(s->sample_array_index - delay * channels, SAMPLE_ARRAY_SIZE);
		if (s->show_mode == SHOW_MODE_WAVES) {
			h = INT_MIN;
			for (i = 0; i < 1000; i += channels) {
				int idx = (SAMPLE_ARRAY_SIZE + x - i) % SAMPLE_ARRAY_SIZE;
				int a = s->sample_array[idx];
				int b = s->sample_array[(idx + 4 * channels) % SAMPLE_ARRAY_SIZE];
				int c = s->sample_array[(idx + 5 * channels) % SAMPLE_ARRAY_SIZE];
				int d = s->sample_array[(idx + 9 * channels) % SAMPLE_ARRAY_SIZE];
				int score = a - d;
				if (h < score && (b ^ c) < 0) {
					h = score;
					i_start = idx;
				}
			}
		}

		s->last_i_start = i_start;
	}
	else {
		i_start = s->last_i_start;
	}

	if (s->show_mode == SHOW_MODE_WAVES) {
		SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);

		/* total height for one channel */
		h = s->height / nb_display_channels;
		/* graph height / 2 */
		h2 = (h * 9) / 20;
		for (ch = 0; ch < nb_display_channels; ch++) {
			i = i_start + ch;
			y1 = s->ytop + ch * h + (h / 2); /* position of center line */
			for (x = 0; x < s->width; x++) {
				y = (s->sample_array[i] * h2) >> 15;
				if (y < 0) {
					y = -y;
					ys = y1 - y;
				}
				else {
					ys = y1;
				}
				fill_rectangle(s->xleft + x, ys, 1, y);
				i += channels;
				if (i >= SAMPLE_ARRAY_SIZE)
					i -= SAMPLE_ARRAY_SIZE;
			}
		}

		SDL_SetRenderDrawColor(renderer, 0, 0, 255, 255);

		for (ch = 1; ch < nb_display_channels; ch++) {
			y = s->ytop + ch * h;
			fill_rectangle(s->xleft, y, s->width, 1);
		}
	}
	else {
		if (realloc_texture(&s->vis_texture, SDL_PIXELFORMAT_ARGB8888, s->width, s->height, SDL_BLENDMODE_NONE, 1) < 0)
			return;

		nb_display_channels = FFMIN(nb_display_channels, 2);
		if (rdft_bits != s->rdft_bits) {
			av_rdft_end(s->rdft);
			av_free(s->rdft_data);
			s->rdft = av_rdft_init(rdft_bits, DFT_R2C);
			s->rdft_bits = rdft_bits;
			s->rdft_data = (FFTSample*)av_malloc_array(nb_freq, 4 * sizeof(*s->rdft_data));
		}
		if (!s->rdft || !s->rdft_data){
			av_log(NULL, AV_LOG_ERROR, "Failed to allocate buffers for RDFT, switching to waves display\n");
			s->show_mode = SHOW_MODE_WAVES;
		}
		else {
			FFTSample *data[2];
			SDL_Rect rect{ s->xpos, 0, 1, s->height };
			uint32_t *pixels;
			int pitch;
			for (ch = 0; ch < nb_display_channels; ch++) {
				data[ch] = s->rdft_data + 2 * nb_freq * ch;
				i = i_start + ch;
				for (x = 0; x < 2 * nb_freq; x++) {
					double w = (x - nb_freq) * (1.0 / nb_freq);
					data[ch][x] = s->sample_array[i] * (1.0 - w * w);
					i += channels;
					if (i >= SAMPLE_ARRAY_SIZE)
						i -= SAMPLE_ARRAY_SIZE;
				}
				av_rdft_calc(s->rdft, data[ch]);
			}
			/* Least efficient way to do this, we should of course
			* directly access it but it is more than fast enough. */
			if (!SDL_LockTexture(s->vis_texture, &rect, (void **)&pixels, &pitch)) {
				pitch >>= 2;
				pixels += pitch * s->height;
				for (y = 0; y < s->height; y++) {
					double w = 1 / sqrt((float)nb_freq);
					int a = sqrt(w * sqrt(data[0][2 * y + 0] * data[0][2 * y + 0] + data[0][2 * y + 1] * data[0][2 * y + 1]));
					int b = (nb_display_channels == 2) ? sqrt(w * hypot(data[1][2 * y + 0], data[1][2 * y + 1]))
						: a;
					a = FFMIN(a, 255);
					b = FFMIN(b, 255);
					pixels -= pitch;
					*pixels = (a << 16) + (b << 8) + ((a + b) >> 1);
				}
				SDL_UnlockTexture(s->vis_texture);
			}
			SDL_RenderCopy(renderer, s->vis_texture, NULL, NULL);
		}
		if (!s->paused)
			s->xpos++;
		if (s->xpos >= s->width)
			s->xpos = s->xleft;
	}
}

void CorePlayer::stream_component_close(VideoState *is, int stream_index)
{
	AVFormatContext *ic = is->ic;
	AVCodecParameters *codecpar;

	if (stream_index < 0 || stream_index >= ic->nb_streams)
		return;
	codecpar = ic->streams[stream_index]->codecpar;

	switch (codecpar->codec_type) {
	case AVMEDIA_TYPE_AUDIO:
		decoder_abort(&is->auddec, &is->sampq);
		SDL_CloseAudio();
		decoder_destroy(&is->auddec);
		swr_free(&is->swr_ctx);
		av_freep(&is->audio_buf1);
		is->audio_buf1_size = 0;
		is->audio_buf = NULL;

		if (is->rdft) {
			av_rdft_end(is->rdft);
			av_freep(&is->rdft_data);
			is->rdft = NULL;
			is->rdft_bits = 0;
		}
		break;
	case AVMEDIA_TYPE_VIDEO:
		decoder_abort(&is->viddec, &is->pictq);
		decoder_destroy(&is->viddec);
		break;
	case AVMEDIA_TYPE_SUBTITLE:
		decoder_abort(&is->subdec, &is->subpq);
		decoder_destroy(&is->subdec);
		break;
	default:
		break;
	}

	ic->streams[stream_index]->discard = AVDISCARD_ALL;
	switch (codecpar->codec_type) {
	case AVMEDIA_TYPE_AUDIO:
		is->audio_st = NULL;
		is->audio_stream = -1;
		break;
	case AVMEDIA_TYPE_VIDEO:
		is->video_st = NULL;
		is->video_stream = -1;
		break;
	case AVMEDIA_TYPE_SUBTITLE:
		is->subtitle_st = NULL;
		is->subtitle_stream = -1;
		break;
	default:
		break;
	}
}

void CorePlayer::stream_close(VideoState *is)
{
	/* XXX: use a special url_shutdown call to abort parse cleanly */
	is->abort_request = 1;
	SDL_WaitThread(is->read_tid, NULL);

	/* close each stream */
	if (is->audio_stream >= 0)
		stream_component_close(is, is->audio_stream);
	if (is->video_stream >= 0)
		stream_component_close(is, is->video_stream);
	if (is->subtitle_stream >= 0)
		stream_component_close(is, is->subtitle_stream);

	avformat_close_input(&is->ic);

	packet_queue_destroy(&is->videoq);
	packet_queue_destroy(&is->audioq);
	packet_queue_destroy(&is->subtitleq);

	/* free all pictures */
	frame_queue_destory(&is->pictq);
	frame_queue_destory(&is->sampq);
	frame_queue_destory(&is->subpq);
	SDL_DestroyCond(is->continue_read_thread);
	sws_freeContext(is->img_convert_ctx);
	sws_freeContext(is->sub_convert_ctx);
	av_free(is->filename);
	if (is->vis_texture)
		SDL_DestroyTexture(is->vis_texture);
	if (is->sub_texture)
		SDL_DestroyTexture(is->sub_texture);
	av_free(is);
}

void CorePlayer::do_exit(VideoState *is)
{
	if (is) {
		stream_close(is);
	}
	if (renderer)
		SDL_DestroyRenderer(renderer);
	if (window)
		SDL_DestroyWindow(window);
	av_lockmgr_register(NULL);
	uninit_opts();
#if CONFIG_AVFILTER
	av_freep(&vfilters_list);
#endif
	avformat_network_deinit();
	if (show_status)
		printf("\n");
	SDL_Quit();
	av_log(NULL, AV_LOG_QUIET, "%s", "");
	exit(0);
}

void CorePlayer::uninit_opts(void)
{
	av_dict_free(&swr_opts);
	av_dict_free(&sws_dict);
	av_dict_free(&format_opts);
	av_dict_free(&codec_opts);
	av_dict_free(&resample_opts);
}

void CorePlayer::sigterm_handler(int sig)
{
	exit(123);
}

void CorePlayer::set_default_window_size(int width, int height, AVRational sar)
{
	SDL_Rect rect;
	calculate_display_rect(&rect, 0, 0, INT_MAX, height, width, height, sar);
	default_width = rect.w;
	default_height = rect.h;
}

int CorePlayer::video_open(VideoState *is, Frame *vp)
{
	int w, h;

	if (vp && vp->width)
		set_default_window_size(vp->width, vp->height, vp->sar);

	if (screen_width) {
		w = screen_width;
		h = screen_height;
	}
	else {
		w = default_width;
		h = default_height;
	}

	if (!window) {
		int flags = SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE;
		if (!window_title)
			window_title = input_filename;
		if (is_full_screen)
			flags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
		//window = SDL_CreateWindow(window_title, SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, w, h, flags);
		if (au_or_vedio)
		{
			window = SDL_CreateWindowFrom(win_hwnd);
		}
		else
		{
			window = SDL_CreateWindow("", 0, 0, 0, 0, SDL_WINDOW_HIDDEN);
		}
		
		SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "linear");
		if (window) {
			SDL_RendererInfo info;
			renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
			if (renderer) {
				if (!SDL_GetRendererInfo(renderer, &info))
					av_log(NULL, AV_LOG_VERBOSE, "Initialized %s renderer.\n", info.name);
			}
		}
	}
	else {
		SDL_SetWindowSize(window, w, h);
	}

	if (!window || !renderer) {
		av_log(NULL, AV_LOG_FATAL, "SDL: could not set video mode - exiting\n");
		do_exit(is);
	}

	is->width = w;
	is->height = h;

	//设备屏幕大小
	if (win_size_cb_)
	{
		win_size_cb_(w, h);
	}

	return 0;
}

void CorePlayer::video_display(VideoState *is)
{
	if (!window)
		video_open(is, NULL);

	SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
	SDL_RenderClear(renderer);
	if (is->audio_st && is->show_mode != SHOW_MODE_VIDEO)
		video_audio_display(is);
	else if (is->video_st)
		video_image_display(is);
	SDL_RenderPresent(renderer);
}

double CorePlayer::get_clock(Clock *c)
{
	if (*c->queue_serial != c->serial)
		return NAN;
	if (c->paused) {
		return c->pts;
	}
	else {
		double time = av_gettime_relative() / 1000000.0;
		return c->pts_drift + time - (time - c->last_updated) * (1.0 - c->speed);
	}
}

void CorePlayer::set_clock_at(Clock *c, double pts, int serial, double time)
{
	c->pts = pts;
	c->last_updated = time;
	c->pts_drift = c->pts - time;
	c->serial = serial;
}

void CorePlayer::set_clock(Clock *c, double pts, int serial)
{
	double time = av_gettime_relative() / 1000000.0;
	set_clock_at(c, pts, serial, time);
}

void CorePlayer::set_clock_speed(Clock *c, double speed)
{
	set_clock(c, get_clock(c), c->serial);
	c->speed = speed;
}

void CorePlayer::init_clock(Clock *c, int *queue_serial)
{
	c->speed = 1.0;
	c->paused = 0;
	c->queue_serial = queue_serial;
	set_clock(c, NAN, -1);
}

void CorePlayer::sync_clock_to_slave(Clock *c, Clock *slave)
{
	double clock = get_clock(c);
	double slave_clock = get_clock(slave);
	if (!isnan(slave_clock) && (isnan(clock) || fabs(clock - slave_clock) > AV_NOSYNC_THRESHOLD))
		set_clock(c, slave_clock, slave->serial);
}

int CorePlayer::get_master_sync_type(VideoState *is)
{
	if (is->av_sync_type == AV_SYNC_VIDEO_MASTER) {
		if (is->video_st)
			return AV_SYNC_VIDEO_MASTER;
		else
			return AV_SYNC_AUDIO_MASTER;
	}
	else if (is->av_sync_type == AV_SYNC_AUDIO_MASTER) {
		if (is->audio_st)
			return AV_SYNC_AUDIO_MASTER;
		else
			return AV_SYNC_EXTERNAL_CLOCK;
	}
	else {
		return AV_SYNC_EXTERNAL_CLOCK;
	}
}

double CorePlayer::get_master_clock(VideoState *is)
{
	double val;

	switch (get_master_sync_type(is)) {
	case AV_SYNC_VIDEO_MASTER:
		val = get_clock(&is->vidclk);
		break;
	case AV_SYNC_AUDIO_MASTER:
		val = get_clock(&is->audclk);
		break;
	default:
		val = get_clock(&is->extclk);
		break;
	}
	return val;
}

void CorePlayer::check_external_clock_speed(VideoState *is)
{
	if (is->video_stream >= 0 && is->videoq.nb_packets <= EXTERNAL_CLOCK_MIN_FRAMES ||
		is->audio_stream >= 0 && is->audioq.nb_packets <= EXTERNAL_CLOCK_MIN_FRAMES) {
		set_clock_speed(&is->extclk, FFMAX(EXTERNAL_CLOCK_SPEED_MIN, is->extclk.speed - EXTERNAL_CLOCK_SPEED_STEP));
	}
	else if ((is->video_stream < 0 || is->videoq.nb_packets > EXTERNAL_CLOCK_MAX_FRAMES) &&
		(is->audio_stream < 0 || is->audioq.nb_packets > EXTERNAL_CLOCK_MAX_FRAMES)) {
		set_clock_speed(&is->extclk, FFMIN(EXTERNAL_CLOCK_SPEED_MAX, is->extclk.speed + EXTERNAL_CLOCK_SPEED_STEP));
	}
	else {
		double speed = is->extclk.speed;
		if (speed != 1.0)
			set_clock_speed(&is->extclk, speed + EXTERNAL_CLOCK_SPEED_STEP * (1.0 - speed) / fabs(1.0 - speed));
	}
}

void CorePlayer::stream_seek(VideoState *is, int64_t pos, int64_t rel, int seek_by_bytes)
{
	if (!is->seek_req) {
		is->seek_pos = pos;
		is->seek_rel = rel;
		is->seek_flags &= ~AVSEEK_FLAG_BYTE;
		if (seek_by_bytes)
			is->seek_flags |= AVSEEK_FLAG_BYTE;
		is->seek_req = 1;
		SDL_CondSignal(is->continue_read_thread);
	}
}

void CorePlayer::stream_toggle_pause(VideoState *is)
{
	if (is->paused) {
		is->frame_timer += av_gettime_relative() / 1000000.0 - is->vidclk.last_updated;
		if (is->read_pause_return != AVERROR(ENOSYS)) {
			is->vidclk.paused = 0;
		}
		set_clock(&is->vidclk, get_clock(&is->vidclk), is->vidclk.serial);
	}
	set_clock(&is->extclk, get_clock(&is->extclk), is->extclk.serial);
	is->paused = is->audclk.paused = is->vidclk.paused = is->extclk.paused = !is->paused;
}

void CorePlayer::toggle_pause(VideoState *is)
{
	stream_toggle_pause(is);
	is->step = 0;
}

void CorePlayer::toggle_mute(VideoState *is)
{
	is->muted = !is->muted;
}

void CorePlayer::update_volume(VideoState *is, int sign, int step)
{
	is->audio_volume = av_clip(is->audio_volume + sign * step, 0, SDL_MIX_MAXVOLUME);
}

void CorePlayer::step_to_next_frame(VideoState *is)
{
	/* if the stream is paused unpause it, then step */
	if (is->paused)
		stream_toggle_pause(is);
	is->step = 1;
}

double CorePlayer::compute_target_delay(double delay, VideoState *is)
{
	double sync_threshold, diff = 0;

	/* update delay to follow master synchronisation source */
	if (get_master_sync_type(is) != AV_SYNC_VIDEO_MASTER) {
		/* if video is slave, we try to correct big delays by
		duplicating or deleting a frame */
		diff = get_clock(&is->vidclk) - get_master_clock(is);

		/* skip or repeat frame. We take into account the
		delay to compute the threshold. I still don't know
		if it is the best guess */
		sync_threshold = FFMAX(AV_SYNC_THRESHOLD_MIN, FFMIN(AV_SYNC_THRESHOLD_MAX, delay));
		if (!isnan(diff) && fabs(diff) < is->max_frame_duration) {
			if (diff <= -sync_threshold)
				delay = FFMAX(0, delay + diff);
			else if (diff >= sync_threshold && delay > AV_SYNC_FRAMEDUP_THRESHOLD)
				delay = delay + diff;
			else if (diff >= sync_threshold)
				delay = 2 * delay;
		}
	}

	av_log(NULL, AV_LOG_TRACE, "video: delay=%0.3f A-V=%f\n",
		delay, -diff);

	return delay;
}

double CorePlayer::vp_duration(VideoState *is, Frame *vp, Frame *nextvp)
{
	if (vp->serial == nextvp->serial) {
		double duration = nextvp->pts - vp->pts;
		if (isnan(duration) || duration <= 0 || duration > is->max_frame_duration)
			return vp->duration;
		else
			return duration;
	}
	else {
		return 0.0;
	}
}

void CorePlayer::update_video_pts(VideoState *is, double pts, int64_t pos, int serial)
{
	/* update current video pts */
	set_clock(&is->vidclk, pts, serial);
	sync_clock_to_slave(&is->extclk, &is->vidclk);
}

void CorePlayer::video_refresh(void *opaque, double *remaining_time)
{
	VideoState *is = (VideoState*)opaque;
	double time;

	Frame *sp, *sp2;

	if (!is->paused && get_master_sync_type(is) == AV_SYNC_EXTERNAL_CLOCK && is->realtime)
		check_external_clock_speed(is);

	if (!display_disable && is->show_mode != SHOW_MODE_VIDEO && is->audio_st) {
		time = av_gettime_relative() / 1000000.0;
		if (is->force_refresh || is->last_vis_time + rdftspeed < time) {
			video_display(is);
			is->last_vis_time = time;
		}
		*remaining_time = FFMIN(*remaining_time, is->last_vis_time + rdftspeed - time);
	}

	if (is->video_st) {
	retry:
		if (frame_queue_nb_remaining(&is->pictq) == 0) {
			// nothing to do, no picture to display in the queue
		}
		else {
			double last_duration, duration, delay;
			Frame *vp, *lastvp;

			/* dequeue the picture */
			lastvp = frame_queue_peek_last(&is->pictq);
			vp = frame_queue_peek(&is->pictq);

			if (vp->serial != is->videoq.serial) {
				frame_queue_next(&is->pictq);
				goto retry;
			}

			if (lastvp->serial != vp->serial)
				is->frame_timer = av_gettime_relative() / 1000000.0;

			if (is->paused)
				goto display;

			/* compute nominal last_duration */
			last_duration = vp_duration(is, lastvp, vp);
			delay = compute_target_delay(last_duration, is);

			time = av_gettime_relative() / 1000000.0;
			if (time < is->frame_timer + delay) {
				*remaining_time = FFMIN(is->frame_timer + delay - time, *remaining_time);
				goto display;
			}

			is->frame_timer += delay;
			if (delay > 0 && time - is->frame_timer > AV_SYNC_THRESHOLD_MAX)
				is->frame_timer = time;

			SDL_LockMutex(is->pictq.mutex);
			if (!isnan(vp->pts))
				update_video_pts(is, vp->pts, vp->pos, vp->serial);
			SDL_UnlockMutex(is->pictq.mutex);

			if (frame_queue_nb_remaining(&is->pictq) > 1) {
				Frame *nextvp = frame_queue_peek_next(&is->pictq);
				duration = vp_duration(is, vp, nextvp);
				if (!is->step && (framedrop > 0 || (framedrop && get_master_sync_type(is) != AV_SYNC_VIDEO_MASTER)) && time > is->frame_timer + duration){
					is->frame_drops_late++;
					frame_queue_next(&is->pictq);
					goto retry;
				}
			}

			if (is->subtitle_st) {
				while (frame_queue_nb_remaining(&is->subpq) > 0) {
					sp = frame_queue_peek(&is->subpq);

					if (frame_queue_nb_remaining(&is->subpq) > 1)
						sp2 = frame_queue_peek_next(&is->subpq);
					else
						sp2 = NULL;

					if (sp->serial != is->subtitleq.serial
						|| (is->vidclk.pts > (sp->pts + ((float)sp->sub.end_display_time / 1000)))
						|| (sp2 && is->vidclk.pts > (sp2->pts + ((float)sp2->sub.start_display_time / 1000))))
					{
						if (sp->uploaded) {
							int i;
							for (i = 0; i < sp->sub.num_rects; i++) {
								AVSubtitleRect *sub_rect = sp->sub.rects[i];
								uint8_t *pixels;
								int pitch, j;

								if (!SDL_LockTexture(is->sub_texture, (SDL_Rect *)sub_rect, (void **)&pixels, &pitch)) {
									for (j = 0; j < sub_rect->h; j++, pixels += pitch)
										memset(pixels, 0, sub_rect->w << 2);
									SDL_UnlockTexture(is->sub_texture);
								}
							}
						}
						frame_queue_next(&is->subpq);
					}
					else {
						break;
					}
				}
			}

			frame_queue_next(&is->pictq);
			is->force_refresh = 1;

			if (is->step && !is->paused)
				stream_toggle_pause(is);
		}
	display:
		/* display picture */
		if (!display_disable && is->force_refresh && is->show_mode == SHOW_MODE_VIDEO && is->pictq.rindex_shown)
			video_display(is);
	}
	is->force_refresh = 0;
	if (show_status) {
		//static int64_t last_time;
		int64_t cur_time;
		int aqsize, vqsize, sqsize;
		double av_diff;

		cur_time = av_gettime_relative();
		if (!last_time || (cur_time - last_time) >= 30000) {
			aqsize = 0;
			vqsize = 0;
			sqsize = 0;
			if (is->audio_st)
				aqsize = is->audioq.size;
			if (is->video_st)
				vqsize = is->videoq.size;
			if (is->subtitle_st)
				sqsize = is->subtitleq.size;
			av_diff = 0;
			if (is->audio_st && is->video_st)
				av_diff = get_clock(&is->audclk) - get_clock(&is->vidclk);
			else if (is->video_st)
				av_diff = get_master_clock(is) - get_clock(&is->vidclk);
			else if (is->audio_st)
				av_diff = get_master_clock(is) - get_clock(&is->audclk);
			av_log(NULL, AV_LOG_INFO,
				"%7.2f %s:%7.3f fd=%4d aq=%5dKB vq=%5dKB sq=%5dB f=%"PRId64"/%"PRId64"   \r",
				get_master_clock(is),
				(is->audio_st && is->video_st) ? "A-V" : (is->video_st ? "M-V" : (is->audio_st ? "M-A" : "   ")),
				av_diff,
				is->frame_drops_early + is->frame_drops_late,
				aqsize / 1024,
				vqsize / 1024,
				sqsize,
				is->video_st ? is->viddec.avctx->pts_correction_num_faulty_dts : 0,
				is->video_st ? is->viddec.avctx->pts_correction_num_faulty_pts : 0);

			//输出时间信息（时间轴）-----------------

			int vedio_current_ts = get_master_clock(is);
			if (vedio_current_ts >=0)
			{
				int thh, tmm, tss;
				thh = vedio_current_ts / 3600;
				tmm = (vedio_current_ts % 3600) / 60;
				tss = (vedio_current_ts % 60);
				//currentclockstr.Format(_T("%02d:%02d:%02d"),thh,tmm,tss);
				std::wstring currentclockstr = nbase::StringPrintf(L"%02d:%02d:%02d", thh, tmm, tss);
				if (start_label_cb_)
				{
					start_label_cb_(currentclockstr);
				}

				if (is->ic)
				{
					int progress_val = 1000 * vedio_current_ts / (is->ic->duration / 1000000);
					if (progress_val_cb_)
					{
						progress_val_cb_(progress_val);
					}
				}
			}

			fflush(stdout);
			last_time = cur_time;
		}
	}
}

void CorePlayer::alloc_picture(VideoState *is)
{
	Frame *vp;
	int sdl_format;

	vp = &is->pictq.queue[is->pictq.windex];

	video_open(is, vp);

	if (vp->format == AV_PIX_FMT_YUV420P)
		sdl_format = SDL_PIXELFORMAT_YV12;
	else
		sdl_format = SDL_PIXELFORMAT_ARGB8888;

	if (realloc_texture(&vp->bmp, sdl_format, vp->width, vp->height, SDL_BLENDMODE_NONE, 0) < 0) {
		/* SDL allocates a buffer smaller than requested if the video
		* overlay hardware is unable to support the requested size. */
		av_log(NULL, AV_LOG_FATAL,
			"Error: the video system does not support an image\n"
			"size of %dx%d pixels. Try using -lowres or -vf \"scale=w:h\"\n"
			"to reduce the image size.\n", vp->width, vp->height);
		do_exit(is);
	}

	SDL_LockMutex(is->pictq.mutex);
	vp->allocated = 1;
	SDL_CondSignal(is->pictq.cond);
	SDL_UnlockMutex(is->pictq.mutex);
}

int CorePlayer::queue_picture(VideoState *is, AVFrame *src_frame, double pts, double duration, int64_t pos, int serial)
{
	Frame *vp;

#if defined(DEBUG_SYNC)
	printf("frame_type=%c pts=%0.3f\n",
		av_get_picture_type_char(src_frame->pict_type), pts);
#endif

	if (!(vp = frame_queue_peek_writable(&is->pictq)))
		return -1;

	vp->sar = src_frame->sample_aspect_ratio;
	vp->uploaded = 0;

	/* alloc or resize hardware picture buffer */
	if (!vp->bmp || !vp->allocated ||
		vp->width != src_frame->width ||
		vp->height != src_frame->height ||
		vp->format != src_frame->format) {
		SDL_Event event;

		vp->allocated = 0;
		vp->width = src_frame->width;
		vp->height = src_frame->height;
		vp->format = src_frame->format;

		/* the allocation must be done in the main thread to avoid
		locking problems. */
		event.type = FF_ALLOC_EVENT;
		event.user.data1 = is;
		SDL_PushEvent(&event);

		/* wait until the picture is allocated */
		SDL_LockMutex(is->pictq.mutex);
		while (!vp->allocated && !is->videoq.abort_request) {
			SDL_CondWait(is->pictq.cond, is->pictq.mutex);
		}
		/* if the queue is aborted, we have to pop the pending ALLOC event or wait for the allocation to complete */
		if (is->videoq.abort_request && SDL_PeepEvents(&event, 1, SDL_GETEVENT, FF_ALLOC_EVENT, FF_ALLOC_EVENT) != 1) {
			while (!vp->allocated && !is->abort_request) {
				SDL_CondWait(is->pictq.cond, is->pictq.mutex);
			}
		}
		SDL_UnlockMutex(is->pictq.mutex);

		if (is->videoq.abort_request)
			return -1;
	}

	/* if the frame is not skipped, then display it */
	if (vp->bmp) {
		vp->pts = pts;
		vp->duration = duration;
		vp->pos = pos;
		vp->serial = serial;

		av_frame_move_ref(vp->frame, src_frame);
		frame_queue_push(&is->pictq);
	}
	return 0;
}

int CorePlayer::get_video_frame(VideoState *is, AVFrame *frame)
{
	int got_picture;

	if ((got_picture = decoder_decode_frame(&is->viddec, frame, NULL)) < 0)
		return -1;

	if (got_picture) {
		double dpts = NAN;

		if (frame->pts != AV_NOPTS_VALUE)
			dpts = av_q2d(is->video_st->time_base) * frame->pts;

		frame->sample_aspect_ratio = av_guess_sample_aspect_ratio(is->ic, is->video_st, frame);

		if (framedrop > 0 || (framedrop && get_master_sync_type(is) != AV_SYNC_VIDEO_MASTER)) {
			if (frame->pts != AV_NOPTS_VALUE) {
				double diff = dpts - get_master_clock(is);
				if (!isnan(diff) && fabs(diff) < AV_NOSYNC_THRESHOLD &&
					diff - is->frame_last_filter_delay < 0 &&
					is->viddec.pkt_serial == is->vidclk.serial &&
					is->videoq.nb_packets) {
					is->frame_drops_early++;
					av_frame_unref(frame);
					got_picture = 0;
				}
			}
		}
	}

	return got_picture;
}

int CorePlayer::configure_filtergraph(AVFilterGraph *graph, const char *filtergraph, AVFilterContext *source_ctx, AVFilterContext *sink_ctx)
{
	int ret, i;
	int nb_filters = graph->nb_filters;
	AVFilterInOut *outputs = NULL, *inputs = NULL;

	if (filtergraph) {
		outputs = avfilter_inout_alloc();
		inputs = avfilter_inout_alloc();
		if (!outputs || !inputs) {
			ret = AVERROR(ENOMEM);
			goto fail;
		}

		outputs->name = av_strdup("in");
		outputs->filter_ctx = source_ctx;
		outputs->pad_idx = 0;
		outputs->next = NULL;

		inputs->name = av_strdup("out");
		inputs->filter_ctx = sink_ctx;
		inputs->pad_idx = 0;
		inputs->next = NULL;

		if ((ret = avfilter_graph_parse_ptr(graph, filtergraph, &inputs, &outputs, NULL)) < 0)
			goto fail;
	}
	else {
		if ((ret = avfilter_link(source_ctx, 0, sink_ctx, 0)) < 0)
			goto fail;
	}

	/* Reorder the filters to ensure that inputs of the custom filters are merged first */
	for (i = 0; i < graph->nb_filters - nb_filters; i++)
		FFSWAP(AVFilterContext*, graph->filters[i], graph->filters[i + nb_filters]);

	ret = avfilter_graph_config(graph, NULL);
fail:
	avfilter_inout_free(&outputs);
	avfilter_inout_free(&inputs);
	return ret;
}

int CorePlayer::configure_video_filters(AVFilterGraph *graph, VideoState *is, const char *vfilters, AVFrame *frame)
{
	static const enum AVPixelFormat pix_fmts[] = { AV_PIX_FMT_YUV420P, AV_PIX_FMT_BGRA, AV_PIX_FMT_NONE };
	char sws_flags_str[512] = "";
	char buffersrc_args[256];
	int ret;
	AVFilterContext *filt_src = NULL, *filt_out = NULL, *last_filter = NULL;
	AVCodecParameters *codecpar = is->video_st->codecpar;
	AVRational fr = av_guess_frame_rate(is->ic, is->video_st, NULL);
	AVDictionaryEntry *e = NULL;

	while ((e = av_dict_get(sws_dict, "", e, AV_DICT_IGNORE_SUFFIX))) {
		if (!strcmp(e->key, "sws_flags")) {
			av_strlcatf(sws_flags_str, sizeof(sws_flags_str), "%s=%s:", "flags", e->value);
		}
		else
			av_strlcatf(sws_flags_str, sizeof(sws_flags_str), "%s=%s:", e->key, e->value);
	}
	if (strlen(sws_flags_str))
		sws_flags_str[strlen(sws_flags_str) - 1] = '\0';

	graph->scale_sws_opts = av_strdup(sws_flags_str);

	sprintf_s(buffersrc_args, sizeof(buffersrc_args),
		"video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:pixel_aspect=%d/%d",
		frame->width, frame->height, frame->format,
		is->video_st->time_base.num, is->video_st->time_base.den,
		codecpar->sample_aspect_ratio.num, FFMAX(codecpar->sample_aspect_ratio.den, 1));
	if (fr.num && fr.den)
		av_strlcatf(buffersrc_args, sizeof(buffersrc_args), ":frame_rate=%d/%d", fr.num, fr.den);

	if ((ret = avfilter_graph_create_filter(&filt_src,
		avfilter_get_by_name("buffer"),
		"ffplay_buffer", buffersrc_args, NULL,
		graph)) < 0)
		goto fail;

	ret = avfilter_graph_create_filter(&filt_out,
		avfilter_get_by_name("buffersink"),
		"ffplay_buffersink", NULL, NULL, graph);
	if (ret < 0)
		goto fail;

	if ((ret = av_opt_set_int_list(filt_out, "pix_fmts", pix_fmts, AV_PIX_FMT_NONE, AV_OPT_SEARCH_CHILDREN)) < 0)
		goto fail;

	last_filter = filt_out;

	/* Note: this macro adds a filter before the lastly added filter, so the
	* processing order of the filters is in reverse */
#define INSERT_FILT(name, arg) do {                                          \
	AVFilterContext *filt_ctx;                                               \
	\
	ret = avfilter_graph_create_filter(&filt_ctx, \
	avfilter_get_by_name(name), \
	"ffplay_" name, arg, NULL, graph);    \
	if (ret < 0)                                                             \
	goto fail;                                                           \
	\
	ret = avfilter_link(filt_ctx, 0, last_filter, 0);                        \
	if (ret < 0)                                                             \
	goto fail;                                                           \
	\
	last_filter = filt_ctx;                                                  \
		} while (0)

	if (autorotate) {
		double theta = get_rotation(is->video_st);

		if (fabs(theta - 90) < 1.0) {
			INSERT_FILT("transpose", "clock");
		}
		else if (fabs(theta - 180) < 1.0) {
			INSERT_FILT("hflip", NULL);
			INSERT_FILT("vflip", NULL);
		}
		else if (fabs(theta - 270) < 1.0) {
			INSERT_FILT("transpose", "cclock");
		}
		else if (fabs(theta) > 1.0) {
			char rotate_buf[64];
			sprintf_s(rotate_buf, sizeof(rotate_buf), "%f*PI/180", theta);
			INSERT_FILT("rotate", rotate_buf);
		}
	}

	if ((ret = configure_filtergraph(graph, vfilters, filt_src, last_filter)) < 0)
		goto fail;

	is->in_video_filter = filt_src;
	is->out_video_filter = filt_out;

fail:
	return ret;
}

double CorePlayer::get_rotation(AVStream *st)
{
	AVDictionaryEntry *rotate_tag = av_dict_get(st->metadata, "rotate", NULL, 0);
	uint8_t* displaymatrix = av_stream_get_side_data(st,
		AV_PKT_DATA_DISPLAYMATRIX, NULL);
	double theta = 0;

	if (rotate_tag && *rotate_tag->value && strcmp(rotate_tag->value, "0")) {
		char *tail;
		theta = av_strtod(rotate_tag->value, &tail);
		if (*tail)
			theta = 0;
	}
	if (displaymatrix && !theta)
		theta = -av_display_rotation_get((int32_t*)displaymatrix);

	theta -= 360 * floor(theta / 360 + 0.9 / 360);

	if (fabs(theta - 90 * round(theta / 90)) > 2)
		av_log(NULL, AV_LOG_WARNING, "Odd rotation angle.\n"
		"If you want to help, upload a sample "
		"of this file to ftp://upload.ffmpeg.org/incoming/ "
		"and contact the ffmpeg-devel mailing list. (ffmpeg-devel@ffmpeg.org)");

	return theta;
}

int CorePlayer::configure_audio_filters(VideoState *is, const char *afilters, int force_output_format)
{
	static const enum AVSampleFormat sample_fmts[] = { AV_SAMPLE_FMT_S16, AV_SAMPLE_FMT_NONE };
	int sample_rates[2] = { 0, -1 };
	int64_t channel_layouts[2] = { 0, -1 };
	int channels[2] = { 0, -1 };
	AVFilterContext *filt_asrc = NULL, *filt_asink = NULL;
	char aresample_swr_opts[512] = "";
	AVDictionaryEntry *e = NULL;
	char asrc_args[256];
	int ret;

	avfilter_graph_free(&is->agraph);
	if (!(is->agraph = avfilter_graph_alloc()))
		return AVERROR(ENOMEM);

	while ((e = av_dict_get(swr_opts, "", e, AV_DICT_IGNORE_SUFFIX)))
		av_strlcatf(aresample_swr_opts, sizeof(aresample_swr_opts), "%s=%s:", e->key, e->value);
	if (strlen(aresample_swr_opts))
		aresample_swr_opts[strlen(aresample_swr_opts) - 1] = '\0';
	av_opt_set(is->agraph, "aresample_swr_opts", aresample_swr_opts, 0);

	ret = sprintf_s(asrc_args, sizeof(asrc_args),
		"sample_rate=%d:sample_fmt=%s:channels=%d:time_base=%d/%d",
		is->audio_filter_src.freq, av_get_sample_fmt_name(is->audio_filter_src.fmt),
		is->audio_filter_src.channels,
		1, is->audio_filter_src.freq);
	if (is->audio_filter_src.channel_layout)
		sprintf_s(asrc_args + ret, sizeof(asrc_args) - ret,
		":channel_layout=0x%"PRIx64, is->audio_filter_src.channel_layout);

	ret = avfilter_graph_create_filter(&filt_asrc,
		avfilter_get_by_name("abuffer"), "ffplay_abuffer",
		asrc_args, NULL, is->agraph);
	if (ret < 0)
		goto end;


	ret = avfilter_graph_create_filter(&filt_asink,
		avfilter_get_by_name("abuffersink"), "ffplay_abuffersink",
		NULL, NULL, is->agraph);
	if (ret < 0)
		goto end;

	if ((ret = av_opt_set_int_list(filt_asink, "sample_fmts", sample_fmts, AV_SAMPLE_FMT_NONE, AV_OPT_SEARCH_CHILDREN)) < 0)
		goto end;
	if ((ret = av_opt_set_int(filt_asink, "all_channel_counts", 1, AV_OPT_SEARCH_CHILDREN)) < 0)
		goto end;

	if (force_output_format) {
		channel_layouts[0] = is->audio_tgt.channel_layout;
		channels[0] = is->audio_tgt.channels;
		sample_rates[0] = is->audio_tgt.freq;
		if ((ret = av_opt_set_int(filt_asink, "all_channel_counts", 0, AV_OPT_SEARCH_CHILDREN)) < 0)
			goto end;
		if ((ret = av_opt_set_int_list(filt_asink, "channel_layouts", channel_layouts, -1, AV_OPT_SEARCH_CHILDREN)) < 0)
			goto end;
		if ((ret = av_opt_set_int_list(filt_asink, "channel_counts", channels, -1, AV_OPT_SEARCH_CHILDREN)) < 0)
			goto end;
		if ((ret = av_opt_set_int_list(filt_asink, "sample_rates", sample_rates, -1, AV_OPT_SEARCH_CHILDREN)) < 0)
			goto end;
	}


	if ((ret = configure_filtergraph(is->agraph, afilters, filt_asrc, filt_asink)) < 0)
		goto end;

	is->in_audio_filter = filt_asrc;
	is->out_audio_filter = filt_asink;

end:
	if (ret < 0)
		avfilter_graph_free(&is->agraph);
	return ret;
}

int CorePlayer::audio_thread()
{
	VideoState *is = m_is;
	AVFrame *frame = av_frame_alloc();
	Frame *af;
#if CONFIG_AVFILTER
	int last_serial = -1;
	int64_t dec_channel_layout;
	int reconfigure;
#endif
	int got_frame = 0;
	AVRational tb;
	int ret = 0;

	if (!frame)
		return AVERROR(ENOMEM);

	do {
		if ((got_frame = decoder_decode_frame(&is->auddec, frame, NULL)) < 0)
			goto the_end;

		if (got_frame) {
			tb = { 1, frame->sample_rate };

#if CONFIG_AVFILTER
			dec_channel_layout = get_valid_channel_layout(frame->channel_layout, av_frame_get_channels(frame));

			reconfigure =
				cmp_audio_fmts(is->audio_filter_src.fmt, is->audio_filter_src.channels,
				(AVSampleFormat)frame->format, av_frame_get_channels(frame)) ||
				is->audio_filter_src.channel_layout != dec_channel_layout ||
				is->audio_filter_src.freq != frame->sample_rate ||
				is->auddec.pkt_serial != last_serial;

			if (reconfigure) {
				char buf1[1024], buf2[1024];
				av_get_channel_layout_string(buf1, sizeof(buf1), -1, is->audio_filter_src.channel_layout);
				av_get_channel_layout_string(buf2, sizeof(buf2), -1, dec_channel_layout);
				av_log(NULL, AV_LOG_DEBUG,
					"Audio frame changed from rate:%d ch:%d fmt:%s layout:%s serial:%d to rate:%d ch:%d fmt:%s layout:%s serial:%d\n",
					is->audio_filter_src.freq, is->audio_filter_src.channels, av_get_sample_fmt_name(is->audio_filter_src.fmt), buf1, last_serial,
					frame->sample_rate, av_frame_get_channels(frame),
					av_get_sample_fmt_name((AVSampleFormat)frame->format), buf2, is->auddec.pkt_serial);

				is->audio_filter_src.fmt = (AVSampleFormat)frame->format;
				is->audio_filter_src.channels = av_frame_get_channels(frame);
				is->audio_filter_src.channel_layout = dec_channel_layout;
				is->audio_filter_src.freq = frame->sample_rate;
				last_serial = is->auddec.pkt_serial;

				if ((ret = configure_audio_filters(is, afilters, 1)) < 0)
					goto the_end;
			}

			if ((ret = av_buffersrc_add_frame(is->in_audio_filter, frame)) < 0)
				goto the_end;

			while ((ret = av_buffersink_get_frame_flags(is->out_audio_filter, frame, 0)) >= 0) {
				tb = is->out_audio_filter->inputs[0]->time_base;
#endif
				if (!(af = frame_queue_peek_writable(&is->sampq)))
					goto the_end;

				af->pts = (frame->pts == AV_NOPTS_VALUE) ? NAN : frame->pts * av_q2d(tb);
				af->pos = av_frame_get_pkt_pos(frame);
				af->serial = is->auddec.pkt_serial;
				af->duration = av_q2d({ frame->nb_samples, frame->sample_rate });

				av_frame_move_ref(af->frame, frame);
				frame_queue_push(&is->sampq);

#if CONFIG_AVFILTER
				if (is->audioq.serial != is->auddec.pkt_serial)
					break;
			}
			if (ret == AVERROR_EOF)
				is->auddec.finished = is->auddec.pkt_serial;
#endif
		}
	} while (ret >= 0 || ret == AVERROR(EAGAIN) || ret == AVERROR_EOF);
the_end:
#if CONFIG_AVFILTER
	avfilter_graph_free(&is->agraph);
#endif
	av_frame_free(&frame);
	return ret;
}

int64_t CorePlayer::get_valid_channel_layout(int64_t channel_layout, int channels)
{
	if (channel_layout && av_get_channel_layout_nb_channels(channel_layout) == channels)
		return channel_layout;
	else
		return 0;
}

int CorePlayer::decoder_start(Decoder *d, int(*fn)(void *), void *arg)
{
	packet_queue_start(d->queue);
	d->decoder_tid = SDL_CreateThread(fn, "decoder", arg);
	if (!d->decoder_tid) {
		av_log(NULL, AV_LOG_ERROR, "SDL_CreateThread(): %s\n", SDL_GetError());
		return AVERROR(ENOMEM);
	}
	return 0;
}

int CorePlayer::video_thread()
{
	VideoState *is =m_is;
	AVFrame *frame = av_frame_alloc();
	double pts;
	double duration;
	int ret;
	AVRational tb = is->video_st->time_base;
	AVRational frame_rate = av_guess_frame_rate(is->ic, is->video_st, NULL);

#if CONFIG_AVFILTER
	AVFilterGraph *graph = avfilter_graph_alloc();
	AVFilterContext *filt_out = NULL, *filt_in = NULL;
	int last_w = 0;
	int last_h = 0;
	enum AVPixelFormat last_format = (AVPixelFormat)-2;
	int last_serial = -1;
	int last_vfilter_idx = 0;
	if (!graph) {
		av_frame_free(&frame);
		return AVERROR(ENOMEM);
	}

#endif

	if (!frame) {
#if CONFIG_AVFILTER
		avfilter_graph_free(&graph);
#endif
		return AVERROR(ENOMEM);
	}

	for (;;) {
		ret = get_video_frame(is, frame);
		if (ret < 0)
			goto the_end;
		if (!ret)
			continue;

#if CONFIG_AVFILTER
		if (last_w != frame->width
			|| last_h != frame->height
			|| last_format != frame->format
			|| last_serial != is->viddec.pkt_serial
			|| last_vfilter_idx != is->vfilter_idx) {
			av_log(NULL, AV_LOG_DEBUG,
				"Video frame changed from size:%dx%d format:%s serial:%d to size:%dx%d format:%s serial:%d\n",
				last_w, last_h,
				(const char *)av_x_if_null(av_get_pix_fmt_name(last_format), "none"), last_serial,
				frame->width, frame->height,
				(const char *)av_x_if_null(av_get_pix_fmt_name((AVPixelFormat)frame->format), "none"), is->viddec.pkt_serial);
			avfilter_graph_free(&graph);
			graph = avfilter_graph_alloc();
			if ((ret = configure_video_filters(graph, is, vfilters_list ? vfilters_list[is->vfilter_idx] : NULL, frame)) < 0) {
				SDL_Event event;
				event.type = FF_QUIT_EVENT;
				event.user.data1 = is;
				SDL_PushEvent(&event);
				goto the_end;
			}
			filt_in = is->in_video_filter;
			filt_out = is->out_video_filter;
			last_w = frame->width;
			last_h = frame->height;
			last_format = (AVPixelFormat)frame->format;
			last_serial = is->viddec.pkt_serial;
			last_vfilter_idx = is->vfilter_idx;
			frame_rate = filt_out->inputs[0]->frame_rate;
		}

		ret = av_buffersrc_add_frame(filt_in, frame);
		if (ret < 0)
			goto the_end;

		while (ret >= 0) {
			is->frame_last_returned_time = av_gettime_relative() / 1000000.0;

			ret = av_buffersink_get_frame_flags(filt_out, frame, 0);
			if (ret < 0) {
				if (ret == AVERROR_EOF)
					is->viddec.finished = is->viddec.pkt_serial;
				ret = 0;
				break;
			}

			is->frame_last_filter_delay = av_gettime_relative() / 1000000.0 - is->frame_last_returned_time;
			if (fabs(is->frame_last_filter_delay) > AV_NOSYNC_THRESHOLD / 10.0)
				is->frame_last_filter_delay = 0;
			tb = filt_out->inputs[0]->time_base;
#endif
			duration = (frame_rate.num && frame_rate.den ? av_q2d({ frame_rate.den, frame_rate.num }) : 0);
			pts = (frame->pts == AV_NOPTS_VALUE) ? NAN : frame->pts * av_q2d(tb);
			ret = queue_picture(is, frame, pts, duration, av_frame_get_pkt_pos(frame), is->viddec.pkt_serial);
			av_frame_unref(frame);
#if CONFIG_AVFILTER
		}
#endif

		if (ret < 0)
			goto the_end;
	}
the_end:
#if CONFIG_AVFILTER
	avfilter_graph_free(&graph);
#endif
	av_frame_free(&frame);
	return 0;
}

int CorePlayer::subtitle_thread()
{
	VideoState *is = m_is;
	Frame *sp;
	int got_subtitle;
	double pts;

	for (;;) {
		if (!(sp = frame_queue_peek_writable(&is->subpq)))
			return 0;

		if ((got_subtitle = decoder_decode_frame(&is->subdec, NULL, &sp->sub)) < 0)
			break;

		pts = 0;

		if (got_subtitle && sp->sub.format == 0) {
			if (sp->sub.pts != AV_NOPTS_VALUE)
				pts = sp->sub.pts / (double)AV_TIME_BASE;
			sp->pts = pts;
			sp->serial = is->subdec.pkt_serial;
			sp->width = is->subdec.avctx->width;
			sp->height = is->subdec.avctx->height;
			sp->uploaded = 0;

			/* now we can update the picture count */
			frame_queue_push(&is->subpq);
		}
		else if (got_subtitle) {
			avsubtitle_free(&sp->sub);
		}
	}
	return 0;
}

void CorePlayer::update_sample_display(VideoState *is, short *samples, int samples_size)
{
	int size, len;

	size = samples_size / sizeof(short);
	while (size > 0) {
		len = SAMPLE_ARRAY_SIZE - is->sample_array_index;
		if (len > size)
			len = size;
		memcpy(is->sample_array + is->sample_array_index, samples, len * sizeof(short));
		samples += len;
		is->sample_array_index += len;
		if (is->sample_array_index >= SAMPLE_ARRAY_SIZE)
			is->sample_array_index = 0;
		size -= len;
	}
}

int CorePlayer::synchronize_audio(VideoState *is, int nb_samples)
{
	int wanted_nb_samples = nb_samples;

	/* if not master, then we try to remove or add samples to correct the clock */
	if (get_master_sync_type(is) != AV_SYNC_AUDIO_MASTER) {
		double diff, avg_diff;
		int min_nb_samples, max_nb_samples;

		diff = get_clock(&is->audclk) - get_master_clock(is);

		if (!isnan(diff) && fabs(diff) < AV_NOSYNC_THRESHOLD) {
			is->audio_diff_cum = diff + is->audio_diff_avg_coef * is->audio_diff_cum;
			if (is->audio_diff_avg_count < AUDIO_DIFF_AVG_NB) {
				/* not enough measures to have a correct estimate */
				is->audio_diff_avg_count++;
			}
			else {
				/* estimate the A-V difference */
				avg_diff = is->audio_diff_cum * (1.0 - is->audio_diff_avg_coef);

				if (fabs(avg_diff) >= is->audio_diff_threshold) {
					wanted_nb_samples = nb_samples + (int)(diff * is->audio_src.freq);
					min_nb_samples = ((nb_samples * (100 - SAMPLE_CORRECTION_PERCENT_MAX) / 100));
					max_nb_samples = ((nb_samples * (100 + SAMPLE_CORRECTION_PERCENT_MAX) / 100));
					wanted_nb_samples = av_clip(wanted_nb_samples, min_nb_samples, max_nb_samples);
				}
				av_log(NULL, AV_LOG_TRACE, "diff=%f adiff=%f sample_diff=%d apts=%0.3f %f\n",
					diff, avg_diff, wanted_nb_samples - nb_samples,
					is->audio_clock, is->audio_diff_threshold);
			}
		}
		else {
			/* too big difference : may be initial PTS errors, so
			reset A-V filter */
			is->audio_diff_avg_count = 0;
			is->audio_diff_cum = 0;
		}
	}

	return wanted_nb_samples;
}

int CorePlayer::audio_decode_frame(VideoState *is)
{
	int data_size, resampled_data_size;
	int64_t dec_channel_layout;
	av_unused double audio_clock0;
	int wanted_nb_samples;
	Frame *af;

	if (is->paused)
		return -1;

	do {
#if defined(_WIN32)
		while (frame_queue_nb_remaining(&is->sampq) == 0) {
			if ((av_gettime_relative() - audio_callback_time) > 1000000LL * is->audio_hw_buf_size / is->audio_tgt.bytes_per_sec / 2)
				return -1;
			av_usleep(1000);
		}
#endif
		if (!(af = frame_queue_peek_readable(&is->sampq)))
			return -1;
		frame_queue_next(&is->sampq);
	} while (af->serial != is->audioq.serial);

	data_size = av_samples_get_buffer_size(NULL, av_frame_get_channels(af->frame),
		af->frame->nb_samples,
		(AVSampleFormat)af->frame->format, 1);

	dec_channel_layout =
		(af->frame->channel_layout && av_frame_get_channels(af->frame) == av_get_channel_layout_nb_channels(af->frame->channel_layout)) ?
		af->frame->channel_layout : av_get_default_channel_layout(av_frame_get_channels(af->frame));
	wanted_nb_samples = synchronize_audio(is, af->frame->nb_samples);

	if (af->frame->format != is->audio_src.fmt ||
		dec_channel_layout != is->audio_src.channel_layout ||
		af->frame->sample_rate != is->audio_src.freq ||
		(wanted_nb_samples != af->frame->nb_samples && !is->swr_ctx)) {
		swr_free(&is->swr_ctx);
		is->swr_ctx = swr_alloc_set_opts(NULL,
			is->audio_tgt.channel_layout, is->audio_tgt.fmt, is->audio_tgt.freq,
			dec_channel_layout, (AVSampleFormat)af->frame->format, af->frame->sample_rate,
			0, NULL);
		if (!is->swr_ctx || swr_init(is->swr_ctx) < 0) {
			av_log(NULL, AV_LOG_ERROR,
				"Cannot create sample rate converter for conversion of %d Hz %s %d channels to %d Hz %s %d channels!\n",
				af->frame->sample_rate, av_get_sample_fmt_name((AVSampleFormat)af->frame->format), av_frame_get_channels(af->frame),
				is->audio_tgt.freq, av_get_sample_fmt_name(is->audio_tgt.fmt), is->audio_tgt.channels);
			swr_free(&is->swr_ctx);
			return -1;
		}
		is->audio_src.channel_layout = dec_channel_layout;
		is->audio_src.channels = av_frame_get_channels(af->frame);
		is->audio_src.freq = af->frame->sample_rate;
		is->audio_src.fmt = (AVSampleFormat)af->frame->format;
	}

	if (is->swr_ctx) {
		const uint8_t **in = (const uint8_t **)af->frame->extended_data;
		uint8_t **out = &is->audio_buf1;
		int out_count = (int64_t)wanted_nb_samples * is->audio_tgt.freq / af->frame->sample_rate + 256;
		int out_size = av_samples_get_buffer_size(NULL, is->audio_tgt.channels, out_count, is->audio_tgt.fmt, 0);
		int len2;
		if (out_size < 0) {
			av_log(NULL, AV_LOG_ERROR, "av_samples_get_buffer_size() failed\n");
			return -1;
		}
		if (wanted_nb_samples != af->frame->nb_samples) {
			if (swr_set_compensation(is->swr_ctx, (wanted_nb_samples - af->frame->nb_samples) * is->audio_tgt.freq / af->frame->sample_rate,
				wanted_nb_samples * is->audio_tgt.freq / af->frame->sample_rate) < 0) {
				av_log(NULL, AV_LOG_ERROR, "swr_set_compensation() failed\n");
				return -1;
			}
		}
		av_fast_malloc(&is->audio_buf1, &is->audio_buf1_size, out_size);
		if (!is->audio_buf1)
			return AVERROR(ENOMEM);
		len2 = swr_convert(is->swr_ctx, out, out_count, in, af->frame->nb_samples);
		if (len2 < 0) {
			av_log(NULL, AV_LOG_ERROR, "swr_convert() failed\n");
			return -1;
		}
		if (len2 == out_count) {
			av_log(NULL, AV_LOG_WARNING, "audio buffer is probably too small\n");
			if (swr_init(is->swr_ctx) < 0)
				swr_free(&is->swr_ctx);
		}
		is->audio_buf = is->audio_buf1;
		resampled_data_size = len2 * is->audio_tgt.channels * av_get_bytes_per_sample(is->audio_tgt.fmt);
	}
	else {
		is->audio_buf = af->frame->data[0];
		resampled_data_size = data_size;
	}

	audio_clock0 = is->audio_clock;
	/* update the audio clock with the pts */
	if (!isnan(af->pts))
		is->audio_clock = af->pts + (double)af->frame->nb_samples / af->frame->sample_rate;
	else
		is->audio_clock = NAN;
	is->audio_clock_serial = af->serial;
#ifdef DEBUG
	{
		static double last_clock;
		printf("audio: delay=%0.3f clock=%0.3f clock0=%0.3f\n",
			is->audio_clock - last_clock,
			is->audio_clock, audio_clock0);
		last_clock = is->audio_clock;
	}
#endif
	return resampled_data_size;
}

void CorePlayer::sdl_audio_callback(Uint8 *stream, int len)
{
	VideoState *is = m_is;
	int audio_size, len1;
	audio_callback_time = av_gettime_relative();

	while (len > 0) {
		if (is->audio_buf_index >= is->audio_buf_size) {
			audio_size = audio_decode_frame(is);
			if (audio_size < 0) {
				/* if error, just output silence */
				is->audio_buf = NULL;
				is->audio_buf_size = SDL_AUDIO_MIN_BUFFER_SIZE / is->audio_tgt.frame_size * is->audio_tgt.frame_size;
			}
			else {
				if (is->show_mode != SHOW_MODE_VIDEO)
					update_sample_display(is, (int16_t *)is->audio_buf, audio_size);
				is->audio_buf_size = audio_size;
			}
			is->audio_buf_index = 0;
		}
		len1 = is->audio_buf_size - is->audio_buf_index;
		if (len1 > len)
			len1 = len;
		if (!is->muted && is->audio_buf && is->audio_volume == SDL_MIX_MAXVOLUME)
			memcpy(stream, (uint8_t *)is->audio_buf + is->audio_buf_index, len1);
		else {
			memset(stream, 0, len1);
			if (!is->muted && is->audio_buf)
				SDL_MixAudio(stream, (uint8_t *)is->audio_buf + is->audio_buf_index, len1, is->audio_volume);
		}
		len -= len1;
		stream += len1;
		is->audio_buf_index += len1;
	}
	is->audio_write_buf_size = is->audio_buf_size - is->audio_buf_index;
	/* Let's assume the audio driver that is used by SDL has two periods. */
	if (!isnan(is->audio_clock)) {
		set_clock_at(&is->audclk, is->audio_clock - (double)(2 * is->audio_hw_buf_size + is->audio_write_buf_size) / is->audio_tgt.bytes_per_sec, is->audio_clock_serial, audio_callback_time / 1000000.0);
		sync_clock_to_slave(&is->extclk, &is->audclk);
	}
}

int CorePlayer::audio_open(void *opaque, int64_t wanted_channel_layout, int wanted_nb_channels, int wanted_sample_rate, struct AudioParams *audio_hw_params)
{
	SDL_AudioSpec wanted_spec, spec;
	const char *env;
	static const int next_nb_channels[] = { 0, 0, 1, 6, 2, 6, 4, 6 };
	static const int next_sample_rates[] = { 0, 44100, 48000, 96000, 192000 };
	int next_sample_rate_idx = FF_ARRAY_ELEMS(next_sample_rates) - 1;

	env = SDL_getenv("SDL_AUDIO_CHANNELS");
	if (env) {
		wanted_nb_channels = atoi(env);
		wanted_channel_layout = av_get_default_channel_layout(wanted_nb_channels);
	}
	if (!wanted_channel_layout || wanted_nb_channels != av_get_channel_layout_nb_channels(wanted_channel_layout)) {
		wanted_channel_layout = av_get_default_channel_layout(wanted_nb_channels);
		wanted_channel_layout &= ~AV_CH_LAYOUT_STEREO_DOWNMIX;
	}
	wanted_nb_channels = av_get_channel_layout_nb_channels(wanted_channel_layout);
	wanted_spec.channels = wanted_nb_channels;
	wanted_spec.freq = wanted_sample_rate;
	if (wanted_spec.freq <= 0 || wanted_spec.channels <= 0) {
		av_log(NULL, AV_LOG_ERROR, "Invalid sample rate or channel count!\n");
		return -1;
	}
	while (next_sample_rate_idx && next_sample_rates[next_sample_rate_idx] >= wanted_spec.freq)
		next_sample_rate_idx--;
	wanted_spec.format = AUDIO_S16SYS;
	wanted_spec.silence = 0;
	wanted_spec.samples = FFMAX(SDL_AUDIO_MIN_BUFFER_SIZE, 2 << av_log2(wanted_spec.freq / SDL_AUDIO_MAX_CALLBACKS_PER_SEC));
	//TODO
	wanted_spec.callback = sdl_audio_callback_1;
	wanted_spec.userdata = this;
	while (SDL_OpenAudio(&wanted_spec, &spec) < 0) {
		av_log(NULL, AV_LOG_WARNING, "SDL_OpenAudio (%d channels, %d Hz): %s\n",
			wanted_spec.channels, wanted_spec.freq, SDL_GetError());
		wanted_spec.channels = next_nb_channels[FFMIN(7, wanted_spec.channels)];
		if (!wanted_spec.channels) {
			wanted_spec.freq = next_sample_rates[next_sample_rate_idx--];
			wanted_spec.channels = wanted_nb_channels;
			if (!wanted_spec.freq) {
				av_log(NULL, AV_LOG_ERROR,
					"No more combinations to try, audio open failed\n");
				return -1;
			}
		}
		wanted_channel_layout = av_get_default_channel_layout(wanted_spec.channels);
	}
	if (spec.format != AUDIO_S16SYS) {
		av_log(NULL, AV_LOG_ERROR,
			"SDL advised audio format %d is not supported!\n", spec.format);
		return -1;
	}
	if (spec.channels != wanted_spec.channels) {
		wanted_channel_layout = av_get_default_channel_layout(spec.channels);
		if (!wanted_channel_layout) {
			av_log(NULL, AV_LOG_ERROR,
				"SDL advised channel count %d is not supported!\n", spec.channels);
			return -1;
		}
	}

	audio_hw_params->fmt = AV_SAMPLE_FMT_S16;
	audio_hw_params->freq = spec.freq;
	audio_hw_params->channel_layout = wanted_channel_layout;
	audio_hw_params->channels = spec.channels;
	audio_hw_params->frame_size = av_samples_get_buffer_size(NULL, audio_hw_params->channels, 1, audio_hw_params->fmt, 1);
	audio_hw_params->bytes_per_sec = av_samples_get_buffer_size(NULL, audio_hw_params->channels, audio_hw_params->freq, audio_hw_params->fmt, 1);
	if (audio_hw_params->bytes_per_sec <= 0 || audio_hw_params->frame_size <= 0) {
		av_log(NULL, AV_LOG_ERROR, "av_samples_get_buffer_size failed\n");
		return -1;
	}
	return spec.size;
}

int CorePlayer::stream_component_open(VideoState *is, int stream_index)
{
	AVFormatContext *ic = is->ic;
	AVCodecContext *avctx;
	AVCodec *codec;
	const char *forced_codec_name = NULL;
	AVDictionary *opts = NULL;
	AVDictionaryEntry *t = NULL;
	int sample_rate, nb_channels;
	int64_t channel_layout;
	int ret = 0;
	int stream_lowres = lowres;

	if (stream_index < 0 || stream_index >= ic->nb_streams)
		return -1;

	avctx = avcodec_alloc_context3(NULL);
	if (!avctx)
		return AVERROR(ENOMEM);

	ret = avcodec_parameters_to_context(avctx, ic->streams[stream_index]->codecpar);
	if (ret < 0)
		goto fail;
	av_codec_set_pkt_timebase(avctx, ic->streams[stream_index]->time_base);

	codec = avcodec_find_decoder(avctx->codec_id);

	switch (avctx->codec_type){
	case AVMEDIA_TYPE_AUDIO: is->last_audio_stream = stream_index; forced_codec_name = audio_codec_name; break;
	case AVMEDIA_TYPE_SUBTITLE: is->last_subtitle_stream = stream_index; forced_codec_name = subtitle_codec_name; break;
	case AVMEDIA_TYPE_VIDEO: is->last_video_stream = stream_index; forced_codec_name = video_codec_name; break;
	}
	if (forced_codec_name)
		codec = avcodec_find_decoder_by_name(forced_codec_name);
	if (!codec) {
		if (forced_codec_name) av_log(NULL, AV_LOG_WARNING,
			"No codec could be found with name '%s'\n", forced_codec_name);
		else                   av_log(NULL, AV_LOG_WARNING,
			"No codec could be found with id %d\n", avctx->codec_id);
		ret = AVERROR(EINVAL);
		goto fail;
	}

	avctx->codec_id = codec->id;
	if (stream_lowres > av_codec_get_max_lowres(codec)){
		av_log(avctx, AV_LOG_WARNING, "The maximum value for lowres supported by the decoder is %d\n",
			av_codec_get_max_lowres(codec));
		stream_lowres = av_codec_get_max_lowres(codec);
	}
	av_codec_set_lowres(avctx, stream_lowres);

#if FF_API_EMU_EDGE
	if (stream_lowres) avctx->flags |= CODEC_FLAG_EMU_EDGE;
#endif
	if (fast)
		avctx->flags2 |= AV_CODEC_FLAG2_FAST;
#if FF_API_EMU_EDGE
	if (codec->capabilities & AV_CODEC_CAP_DR1)
		avctx->flags |= CODEC_FLAG_EMU_EDGE;
#endif

	opts = filter_codec_opts(codec_opts, avctx->codec_id, ic, ic->streams[stream_index], codec);
	if (!av_dict_get(opts, "threads", NULL, 0))
		av_dict_set(&opts, "threads", "auto", 0);
	if (stream_lowres)
		av_dict_set_int(&opts, "lowres", stream_lowres, 0);
	if (avctx->codec_type == AVMEDIA_TYPE_VIDEO || avctx->codec_type == AVMEDIA_TYPE_AUDIO)
		av_dict_set(&opts, "refcounted_frames", "1", 0);
	if ((ret = avcodec_open2(avctx, codec, &opts)) < 0) {
		goto fail;
	}
	if ((t = av_dict_get(opts, "", NULL, AV_DICT_IGNORE_SUFFIX))) {
		av_log(NULL, AV_LOG_ERROR, "Option %s not found.\n", t->key);
		ret = AVERROR_OPTION_NOT_FOUND;
		goto fail;
	}

	is->eof = 0;
	ic->streams[stream_index]->discard = AVDISCARD_DEFAULT;
	switch (avctx->codec_type) {
	case AVMEDIA_TYPE_AUDIO:
#if CONFIG_AVFILTER
	{
		AVFilterLink *link;

		is->audio_filter_src.freq = avctx->sample_rate;
		is->audio_filter_src.channels = avctx->channels;
		is->audio_filter_src.channel_layout = get_valid_channel_layout(avctx->channel_layout, avctx->channels);
		is->audio_filter_src.fmt = avctx->sample_fmt;
		if ((ret = configure_audio_filters(is, afilters, 0)) < 0)
			goto fail;
		link = is->out_audio_filter->inputs[0];
		sample_rate = link->sample_rate;
		nb_channels = avfilter_link_get_channels(link);
		channel_layout = link->channel_layout;
	}
#else
		sample_rate = avctx->sample_rate;
		nb_channels = avctx->channels;
		channel_layout = avctx->channel_layout;
#endif

		/* prepare audio output */
		if ((ret = audio_open(is, channel_layout, nb_channels, sample_rate, &is->audio_tgt)) < 0)
			goto fail;
		is->audio_hw_buf_size = ret;
		is->audio_src = is->audio_tgt;
		is->audio_buf_size = 0;
		is->audio_buf_index = 0;

		/* init averaging filter */
		is->audio_diff_avg_coef = exp(log(0.01) / AUDIO_DIFF_AVG_NB);
		is->audio_diff_avg_count = 0;
		/* since we do not have a precise anough audio FIFO fullness,
		we correct audio sync only if larger than this threshold */
		is->audio_diff_threshold = (double)(is->audio_hw_buf_size) / is->audio_tgt.bytes_per_sec;

		is->audio_stream = stream_index;
		is->audio_st = ic->streams[stream_index];

		decoder_init(&is->auddec, avctx, &is->audioq, is->continue_read_thread);
		if ((is->ic->iformat->flags & (AVFMT_NOBINSEARCH | AVFMT_NOGENSEARCH | AVFMT_NO_BYTE_SEEK)) && !is->ic->iformat->read_seek) {
			is->auddec.start_pts = is->audio_st->start_time;
			is->auddec.start_pts_tb = is->audio_st->time_base;
		}
		//TODO
		if ((ret = decoder_start(&is->auddec, audio_thread_1, this)) < 0)
			goto out;
		SDL_PauseAudio(0);
		break;
	case AVMEDIA_TYPE_VIDEO:
		is->video_stream = stream_index;
		is->video_st = ic->streams[stream_index];

		decoder_init(&is->viddec, avctx, &is->videoq, is->continue_read_thread);
		//TODO
		if ((ret = decoder_start(&is->viddec, video_thread_1, this)) < 0)
			goto out;
		is->queue_attachments_req = 1;
		break;
	case AVMEDIA_TYPE_SUBTITLE:
		is->subtitle_stream = stream_index;
		is->subtitle_st = ic->streams[stream_index];

		decoder_init(&is->subdec, avctx, &is->subtitleq, is->continue_read_thread);
		//TODO
		if ((ret = decoder_start(&is->subdec, subtitle_thread_1, this)) < 0)
			goto out;
		break;
	default:
		break;
	}
	goto out;

fail:
	avcodec_free_context(&avctx);
out:
	av_dict_free(&opts);

	return ret;
}

AVDictionary * CorePlayer::filter_codec_opts(AVDictionary *opts, enum AVCodecID codec_id, AVFormatContext *s, AVStream *st, AVCodec *codec)
{
	AVDictionary    *ret = NULL;
	AVDictionaryEntry *t = NULL;
	int            flags = s->oformat ? AV_OPT_FLAG_ENCODING_PARAM
		: AV_OPT_FLAG_DECODING_PARAM;
	char          prefix = 0;
	const AVClass    *cc = avcodec_get_class();

	if (!codec)
		codec = s->oformat ? avcodec_find_encoder(codec_id)
		: avcodec_find_decoder(codec_id);

	switch (st->codecpar->codec_type) {
	case AVMEDIA_TYPE_VIDEO:
		prefix = 'v';
		flags |= AV_OPT_FLAG_VIDEO_PARAM;
		break;
	case AVMEDIA_TYPE_AUDIO:
		prefix = 'a';
		flags |= AV_OPT_FLAG_AUDIO_PARAM;
		break;
	case AVMEDIA_TYPE_SUBTITLE:
		prefix = 's';
		flags |= AV_OPT_FLAG_SUBTITLE_PARAM;
		break;
	}

	while (t = av_dict_get(opts, "", t, AV_DICT_IGNORE_SUFFIX)) {
		char *p = strchr(t->key, ':');

		/* check stream specification in opt name */
		if (p)
			switch (check_stream_specifier(s, st, p + 1)) {
			case  1: *p = 0; break;
			case  0:         continue;
			default:         exit_program(1);
		}

		if (av_opt_find(&cc, t->key, NULL, flags, AV_OPT_SEARCH_FAKE_OBJ) ||
			!codec ||
			(codec->priv_class &&
			av_opt_find(&codec->priv_class, t->key, NULL, flags,
			AV_OPT_SEARCH_FAKE_OBJ)))
			av_dict_set(&ret, t->key, t->value, 0);
		else if (t->key[0] == prefix &&
			av_opt_find(&cc, t->key + 1, NULL, flags,
			AV_OPT_SEARCH_FAKE_OBJ))
			av_dict_set(&ret, t->key + 1, t->value, 0);

		if (p)
			*p = ':';
	}
	return ret;
}

int CorePlayer::check_stream_specifier(AVFormatContext *s, AVStream *st, const char *spec)
{
	int ret = avformat_match_stream_specifier(s, st, spec);
	if (ret < 0)
		av_log(s, AV_LOG_ERROR, "Invalid stream specifier: %s.\n", spec);
	return ret;
}

void CorePlayer::exit_program(int ret)
{
	/*if (program_exit)
		program_exit(ret);*/

	exit(ret);
}

int CorePlayer::decode_interrupt_cb()
{
	//VideoState *is = (VideoState*)ctx;
	return m_is->abort_request;
}

int CorePlayer::stream_has_enough_packets(AVStream *st, int stream_id, PacketQueue *queue)
{
	return stream_id < 0 ||
		queue->abort_request ||
		(st->disposition & AV_DISPOSITION_ATTACHED_PIC) ||
		queue->nb_packets > MIN_FRAMES && (!queue->duration || av_q2d(st->time_base) * queue->duration > 1.0);
}

int CorePlayer::is_realtime(AVFormatContext *s)
{
	if (!strcmp(s->iformat->name, "rtp")
		|| !strcmp(s->iformat->name, "rtsp")
		|| !strcmp(s->iformat->name, "sdp")
		)
		return 1;

	if (s->pb && (!strncmp(s->filename, "rtp:", 4)
		|| !strncmp(s->filename, "udp:", 4)
		)
		)
		return 1;
	return 0;
}

int CorePlayer::read_thread()
{
	CoInitialize(NULL);
	VideoState *is = m_is;
	AVFormatContext *ic = NULL;
	int err, i, ret;
	int st_index[AVMEDIA_TYPE_NB];
	AVPacket pkt1, *pkt = &pkt1;
	int64_t stream_start_time;
	int pkt_in_play_range = 0;
	AVDictionaryEntry *t;
	AVDictionary **opts;
	int orig_nb_streams;
	SDL_mutex *wait_mutex = SDL_CreateMutex();
	int scan_all_pmts_set = 0;
	int64_t pkt_ts;

	if (!wait_mutex) {
		av_log(NULL, AV_LOG_FATAL, "SDL_CreateMutex(): %s\n", SDL_GetError());
		ret = AVERROR(ENOMEM);
		goto fail;
	}

	memset(st_index, -1, sizeof(st_index));
	is->last_video_stream = is->video_stream = -1;
	is->last_audio_stream = is->audio_stream = -1;
	is->last_subtitle_stream = is->subtitle_stream = -1;
	is->eof = 0;

	ic = avformat_alloc_context();
	if (!ic) {
		av_log(NULL, AV_LOG_FATAL, "Could not allocate context.\n");
		ret = AVERROR(ENOMEM);
		goto fail;
	}
	//TODO
	ic->interrupt_callback.callback = decode_interrupt_cb_1;
	ic->interrupt_callback.opaque = this;
	if (!av_dict_get(format_opts, "scan_all_pmts", NULL, AV_DICT_MATCH_CASE)) {
		av_dict_set(&format_opts, "scan_all_pmts", "1", AV_DICT_DONT_OVERWRITE);
		scan_all_pmts_set = 1;
	}
	err = avformat_open_input(&ic, is->filename, is->iformat, &format_opts);
	if (err < 0) {
		//print_error(is->filename, err);
		ret = -1;
		goto fail;
	}
	if (scan_all_pmts_set)
		av_dict_set(&format_opts, "scan_all_pmts", NULL, AV_DICT_MATCH_CASE);

	if ((t = av_dict_get(format_opts, "", NULL, AV_DICT_IGNORE_SUFFIX))) {
		av_log(NULL, AV_LOG_ERROR, "Option %s not found.\n", t->key);
		ret = AVERROR_OPTION_NOT_FOUND;
		goto fail;
	}
	is->ic = ic;

	if (genpts)
		ic->flags |= AVFMT_FLAG_GENPTS;

	av_format_inject_global_side_data(ic);

	opts = setup_find_stream_info_opts(ic, codec_opts);
	orig_nb_streams = ic->nb_streams;

	err = avformat_find_stream_info(ic, opts);

	for (i = 0; i < orig_nb_streams; i++)
		av_dict_free(&opts[i]);
	av_freep(&opts);

	if (err < 0) {
		av_log(NULL, AV_LOG_WARNING,
			"%s: could not find codec parameters\n", is->filename);
		ret = -1;
		goto fail;
	}

	if (ic->pb)
		ic->pb->eof_reached = 0; // FIXME hack, ffplay maybe should not use avio_feof() to test for the end

	if (seek_by_bytes < 0)
		seek_by_bytes = !!(ic->iformat->flags & AVFMT_TS_DISCONT) && strcmp("ogg", ic->iformat->name);

	is->max_frame_duration = (ic->iformat->flags & AVFMT_TS_DISCONT) ? 10.0 : 3600.0;

	if (!window_title && (t = av_dict_get(ic->metadata, "title", NULL, 0)))
		window_title = av_asprintf("%s - %s", t->value, input_filename);

	/* if seeking requested, we execute it */
	if (start_time != AV_NOPTS_VALUE) {
		int64_t timestamp;

		timestamp = start_time;
		/* add the stream start time */
		if (ic->start_time != AV_NOPTS_VALUE)
			timestamp += ic->start_time;
		ret = avformat_seek_file(ic, -1, INT64_MIN, timestamp, INT64_MAX, 0);
		if (ret < 0) {
			av_log(NULL, AV_LOG_WARNING, "%s: could not seek to position %0.3f\n",
				is->filename, (double)timestamp / AV_TIME_BASE);
		}
	}

	is->realtime = is_realtime(ic);

	if (show_status)
		av_dump_format(ic, 0, is->filename, 0);

	for (i = 0; i < ic->nb_streams; i++) {
		AVStream *st = ic->streams[i];
		enum AVMediaType type = st->codecpar->codec_type;
		st->discard = AVDISCARD_ALL;
		if (type >= 0 && wanted_stream_spec[type] && st_index[type] == -1)
			if (avformat_match_stream_specifier(ic, st, wanted_stream_spec[type]) > 0)
				st_index[type] = i;
	}
	for (i = 0; i < AVMEDIA_TYPE_NB; i++) {
		if (wanted_stream_spec[i] && st_index[i] == -1) {
			av_log(NULL, AV_LOG_ERROR,
				"Stream specifier %s does not match any %s stream\n",
				wanted_stream_spec[i], av_get_media_type_string((AVMediaType)i));
			st_index[i] = INT_MAX;
		}
	}

	if (!video_disable)
		st_index[AVMEDIA_TYPE_VIDEO] =
		av_find_best_stream(ic, AVMEDIA_TYPE_VIDEO,
		st_index[AVMEDIA_TYPE_VIDEO], -1, NULL, 0);
	if (!audio_disable)
		st_index[AVMEDIA_TYPE_AUDIO] =
		av_find_best_stream(ic, AVMEDIA_TYPE_AUDIO,
		st_index[AVMEDIA_TYPE_AUDIO],
		st_index[AVMEDIA_TYPE_VIDEO],
		NULL, 0);
	if (!video_disable && !subtitle_disable)
		st_index[AVMEDIA_TYPE_SUBTITLE] =
		av_find_best_stream(ic, AVMEDIA_TYPE_SUBTITLE,
		st_index[AVMEDIA_TYPE_SUBTITLE],
		(st_index[AVMEDIA_TYPE_AUDIO] >= 0 ?
		st_index[AVMEDIA_TYPE_AUDIO] :
		st_index[AVMEDIA_TYPE_VIDEO]),
		NULL, 0);

	is->show_mode = show_mode;
	if (st_index[AVMEDIA_TYPE_VIDEO] >= 0) {
		AVStream *st = ic->streams[st_index[AVMEDIA_TYPE_VIDEO]];
		AVCodecParameters *codecpar = st->codecpar;
		AVRational sar = av_guess_sample_aspect_ratio(ic, st, NULL);
		if (codecpar->width)
			set_default_window_size(codecpar->width, codecpar->height, sar);
	}

	/* open the streams */
	if (st_index[AVMEDIA_TYPE_AUDIO] >= 0) {
		stream_component_open(is, st_index[AVMEDIA_TYPE_AUDIO]);
	}

	ret = -1;
	if (st_index[AVMEDIA_TYPE_VIDEO] >= 0) {
		ret = stream_component_open(is, st_index[AVMEDIA_TYPE_VIDEO]);
	}
	if (is->show_mode == SHOW_MODE_NONE)
		is->show_mode = ret >= 0 ? SHOW_MODE_VIDEO : SHOW_MODE_RDFT;

	if (st_index[AVMEDIA_TYPE_SUBTITLE] >= 0) {
		stream_component_open(is, st_index[AVMEDIA_TYPE_SUBTITLE]);
	}

	if (is->video_stream < 0 && is->audio_stream < 0) {
		av_log(NULL, AV_LOG_FATAL, "Failed to open file '%s' or configure filtergraph\n",
			is->filename);
		ret = -1;
		goto fail;
	}

	if (infinite_buffer < 0 && is->realtime)
		infinite_buffer = 1;

	//获取一些系统信息
	init_params(is);

	for (;;) {
		if (is->abort_request)
			break;
		if (is->paused != is->last_paused) {
			is->last_paused = is->paused;
			if (is->paused)
				is->read_pause_return = av_read_pause(ic);
			else
				av_read_play(ic);
		}
#if CONFIG_RTSP_DEMUXER || CONFIG_MMSH_PROTOCOL
		if (is->paused &&
			(!strcmp(ic->iformat->name, "rtsp") ||
			(ic->pb && !strncmp(input_filename, "mmsh:", 5)))) {
			/* wait 10 ms to avoid trying to get another packet */
			/* XXX: horrible */
			SDL_Delay(10);
			continue;
		}
#endif
		if (is->seek_req) {
			int64_t seek_target = is->seek_pos;
			int64_t seek_min = is->seek_rel > 0 ? seek_target - is->seek_rel + 2 : INT64_MIN;
			int64_t seek_max = is->seek_rel < 0 ? seek_target - is->seek_rel - 2 : INT64_MAX;
			// FIXME the +-2 is due to rounding being not done in the correct direction in generation
			//      of the seek_pos/seek_rel variables

			ret = avformat_seek_file(is->ic, -1, seek_min, seek_target, seek_max, is->seek_flags);
			if (ret < 0) {
				av_log(NULL, AV_LOG_ERROR,
					"%s: error while seeking\n", is->ic->filename);
			}
			else {
				if (is->audio_stream >= 0) {
					packet_queue_flush(&is->audioq);
					packet_queue_put(&is->audioq, &flush_pkt);
				}
				if (is->subtitle_stream >= 0) {
					packet_queue_flush(&is->subtitleq);
					packet_queue_put(&is->subtitleq, &flush_pkt);
				}
				if (is->video_stream >= 0) {
					packet_queue_flush(&is->videoq);
					packet_queue_put(&is->videoq, &flush_pkt);
				}
				if (is->seek_flags & AVSEEK_FLAG_BYTE) {
					set_clock(&is->extclk, NAN, 0);
				}
				else {
					set_clock(&is->extclk, seek_target / (double)AV_TIME_BASE, 0);
				}
			}
			is->seek_req = 0;
			is->queue_attachments_req = 1;
			is->eof = 0;
			if (is->paused)
				step_to_next_frame(is);
		}
		if (is->queue_attachments_req) {
			if (is->video_st && is->video_st->disposition & AV_DISPOSITION_ATTACHED_PIC) {
				AVPacket copy;
				if ((ret = av_copy_packet(&copy, &is->video_st->attached_pic)) < 0)
					goto fail;
				packet_queue_put(&is->videoq, &copy);
				packet_queue_put_nullpacket(&is->videoq, is->video_stream);
			}
			is->queue_attachments_req = 0;
		}

		/* if the queue are full, no need to read more */
		if (infinite_buffer<1 &&
			(is->audioq.size + is->videoq.size + is->subtitleq.size > MAX_QUEUE_SIZE
			|| (stream_has_enough_packets(is->audio_st, is->audio_stream, &is->audioq) &&
			stream_has_enough_packets(is->video_st, is->video_stream, &is->videoq) &&
			stream_has_enough_packets(is->subtitle_st, is->subtitle_stream, &is->subtitleq)))) {
			/* wait 10 ms */
			SDL_LockMutex(wait_mutex);
			SDL_CondWaitTimeout(is->continue_read_thread, wait_mutex, 10);
			SDL_UnlockMutex(wait_mutex);
			continue;
		}
		if (!is->paused &&
			(!is->audio_st || (is->auddec.finished == is->audioq.serial && frame_queue_nb_remaining(&is->sampq) == 0)) &&
			(!is->video_st || (is->viddec.finished == is->videoq.serial && frame_queue_nb_remaining(&is->pictq) == 0))) {
			if (loop != 1 && (!loop || --loop)) {
				stream_seek(is, start_time != AV_NOPTS_VALUE ? start_time : 0, 0, 0);
			}
			else if (autoexit) {
				ret = AVERROR_EOF;
				goto fail;
			}
		}
		ret = av_read_frame(ic, pkt);
		if (ret < 0) {
			if ((ret == AVERROR_EOF || avio_feof(ic->pb)) && !is->eof) {
				if (is->video_stream >= 0)
					packet_queue_put_nullpacket(&is->videoq, is->video_stream);
				if (is->audio_stream >= 0)
					packet_queue_put_nullpacket(&is->audioq, is->audio_stream);
				if (is->subtitle_stream >= 0)
					packet_queue_put_nullpacket(&is->subtitleq, is->subtitle_stream);
				is->eof = 1;
			}
			if (ic->pb && ic->pb->error)
				break;
			SDL_LockMutex(wait_mutex);
			SDL_CondWaitTimeout(is->continue_read_thread, wait_mutex, 10);
			SDL_UnlockMutex(wait_mutex);
			continue;
		}
		else {
			is->eof = 0;
		}
		/* check if packet is in play range specified by user, then queue, otherwise discard */
		stream_start_time = ic->streams[pkt->stream_index]->start_time;
		pkt_ts = pkt->pts == AV_NOPTS_VALUE ? pkt->dts : pkt->pts;
		pkt_in_play_range = duration == AV_NOPTS_VALUE ||
			(pkt_ts - (stream_start_time != AV_NOPTS_VALUE ? stream_start_time : 0)) *
			av_q2d(ic->streams[pkt->stream_index]->time_base) -
			(double)(start_time != AV_NOPTS_VALUE ? start_time : 0) / 1000000
			<= ((double)duration / 1000000);
		if (pkt->stream_index == is->audio_stream && pkt_in_play_range) {
			packet_queue_put(&is->audioq, pkt);
		}
		else if (pkt->stream_index == is->video_stream && pkt_in_play_range
			&& !(is->video_st->disposition & AV_DISPOSITION_ATTACHED_PIC)) {
			packet_queue_put(&is->videoq, pkt);
		}
		else if (pkt->stream_index == is->subtitle_stream && pkt_in_play_range) {
			packet_queue_put(&is->subtitleq, pkt);
		}
		else {
			av_packet_unref(pkt);
		}
	}

	ret = 0;
fail:
	if (ic && !is->ic)
		avformat_close_input(&ic);

	if (ret != 0) {
		SDL_Event event;

		event.type = FF_QUIT_EVENT;
		event.user.data1 = is;
		SDL_PushEvent(&event);
	}
	SDL_DestroyMutex(wait_mutex);
	return 0;
}

AVDictionary ** CorePlayer::setup_find_stream_info_opts(AVFormatContext *s, AVDictionary *codec_opts)
{
	int i;
	AVDictionary **opts;

	if (!s->nb_streams)
		return NULL;
	opts = (AVDictionary**)av_mallocz_array(s->nb_streams, sizeof(*opts));
	if (!opts) {
		av_log(NULL, AV_LOG_ERROR,
			"Could not alloc memory for stream options.\n");
		return NULL;
	}
	for (i = 0; i < s->nb_streams; i++)
		opts[i] = filter_codec_opts(codec_opts, s->streams[i]->codecpar->codec_id,
		s, s->streams[i], NULL);
	return opts;
}

VideoState * CorePlayer::stream_open(const char *filename, AVInputFormat *iformat)
{
	VideoState *is;

	is = (VideoState*)av_mallocz(sizeof(VideoState));
	if (!is)
		return NULL;
	is->filename = av_strdup(filename);
	if (!is->filename)
	{
		stream_close(is);
		return NULL;
	}
	is->iformat = iformat;
	is->ytop = 0;
	is->xleft = 0;

	/* start video display */
	if (frame_queue_init(&is->pictq, &is->videoq, VIDEO_PICTURE_QUEUE_SIZE, 1) < 0)
	{
		stream_close(is);
		return NULL;
	}
	if (frame_queue_init(&is->subpq, &is->subtitleq, SUBPICTURE_QUEUE_SIZE, 0) < 0)
	{
		stream_close(is);
		return NULL;
	}
	if (frame_queue_init(&is->sampq, &is->audioq, SAMPLE_QUEUE_SIZE, 1) < 0)
	{
		stream_close(is);
		return NULL;
	}

	if (packet_queue_init(&is->videoq) < 0 ||
		packet_queue_init(&is->audioq) < 0 ||
		packet_queue_init(&is->subtitleq) < 0)
	{
		stream_close(is);
		return NULL;
	}

	if (!(is->continue_read_thread = SDL_CreateCond())) {
		av_log(NULL, AV_LOG_FATAL, "SDL_CreateCond(): %s\n", SDL_GetError());
		stream_close(is);
		return NULL;
	}

	init_clock(&is->vidclk, &is->videoq.serial);
	init_clock(&is->audclk, &is->audioq.serial);
	init_clock(&is->extclk, &is->extclk.serial);
	is->audio_clock_serial = -1;
	is->audio_volume = SDL_MIX_MAXVOLUME;
	is->muted = 0;
	is->av_sync_type = av_sync_type;
	
	return is;
}

void CorePlayer::stream_cycle_channel(VideoState *is, int codec_type)
{
	AVFormatContext *ic = is->ic;
	int start_index, stream_index;
	int old_index;
	AVStream *st;
	AVProgram *p = NULL;
	int nb_streams = is->ic->nb_streams;

	if (codec_type == AVMEDIA_TYPE_VIDEO) {
		start_index = is->last_video_stream;
		old_index = is->video_stream;
	}
	else if (codec_type == AVMEDIA_TYPE_AUDIO) {
		start_index = is->last_audio_stream;
		old_index = is->audio_stream;
	}
	else {
		start_index = is->last_subtitle_stream;
		old_index = is->subtitle_stream;
	}
	stream_index = start_index;

	if (codec_type != AVMEDIA_TYPE_VIDEO && is->video_stream != -1) {
		p = av_find_program_from_stream(ic, NULL, is->video_stream);
		if (p) {
			nb_streams = p->nb_stream_indexes;
			for (start_index = 0; start_index < nb_streams; start_index++)
				if (p->stream_index[start_index] == stream_index)
					break;
			if (start_index == nb_streams)
				start_index = -1;
			stream_index = start_index;
		}
	}

	for (;;) {
		if (++stream_index >= nb_streams)
		{
			if (codec_type == AVMEDIA_TYPE_SUBTITLE)
			{
				stream_index = -1;
				is->last_subtitle_stream = -1;
				goto the_end;
			}
			if (start_index == -1)
				return;
			stream_index = 0;
		}
		if (stream_index == start_index)
			return;
		st = is->ic->streams[p ? p->stream_index[stream_index] : stream_index];
		if (st->codecpar->codec_type == codec_type) {
			/* check that parameters are OK */
			switch (codec_type) {
			case AVMEDIA_TYPE_AUDIO:
				if (st->codecpar->sample_rate != 0 &&
					st->codecpar->channels != 0)
					goto the_end;
				break;
			case AVMEDIA_TYPE_VIDEO:
			case AVMEDIA_TYPE_SUBTITLE:
				goto the_end;
			default:
				break;
			}
		}
	}
the_end:
	if (p && stream_index != -1)
		stream_index = p->stream_index[stream_index];
	av_log(NULL, AV_LOG_INFO, "Switch %s stream from #%d to #%d\n",
		av_get_media_type_string((AVMediaType)codec_type),
		old_index,
		stream_index);

	stream_component_close(is, old_index);
	stream_component_open(is, stream_index);
}

void CorePlayer::toggle_full_screen(VideoState *is)
{
	is_full_screen = !is_full_screen;
	SDL_SetWindowFullscreen(window, is_full_screen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
}

void CorePlayer::toggle_audio_display(VideoState *is)
{
	int next = is->show_mode;
	do {
		next = (next + 1) % SHOW_MODE_NB;
	} while (next != is->show_mode && (next == SHOW_MODE_VIDEO && !is->video_st || next != SHOW_MODE_VIDEO && !is->audio_st));
	if (is->show_mode != next) {
		is->force_refresh = 1;
		is->show_mode = (ShowMode)next;
	}
}

void CorePlayer::refresh_loop_wait_event(VideoState *is, SDL_Event *event)
{
	double remaining_time = 0.0;
	SDL_PumpEvents();
	while (!SDL_PeepEvents(event, 1, SDL_GETEVENT, SDL_FIRSTEVENT, SDL_LASTEVENT)) {
		if (!cursor_hidden && av_gettime_relative() - cursor_last_shown > CURSOR_HIDE_DELAY) {
			SDL_ShowCursor(0);
			cursor_hidden = 1;
		}
		if (remaining_time > 0.0)
			av_usleep((int64_t)(remaining_time * 1000000.0));
		remaining_time = REFRESH_RATE;
		if (is->show_mode != SHOW_MODE_NONE && (!is->paused || is->force_refresh))
			video_refresh(is, &remaining_time);
		SDL_PumpEvents();
	}
}

void CorePlayer::seek_chapter(VideoState *is, int incr)
{
	int64_t pos = get_master_clock(is) * AV_TIME_BASE;
	int i;

	if (!is->ic->nb_chapters)
		return;

	/* find the current chapter */
	for (i = 0; i < is->ic->nb_chapters; i++) {
		AVChapter *ch = is->ic->chapters[i];
		if (av_compare_ts(pos, AV_TIME_BASE_Q, ch->start, ch->time_base) < 0) {
			i--;
			break;
		}
	}

	i += incr;
	i = FFMAX(i, 0);
	if (i >= is->ic->nb_chapters)
		return;

	av_log(NULL, AV_LOG_VERBOSE, "Seeking to chapter %d.\n", i);
	stream_seek(is, av_rescale_q(is->ic->chapters[i]->start, is->ic->chapters[i]->time_base,
		AV_TIME_BASE_Q), 0, 0);
}

void CorePlayer::event_loop(VideoState *cur_stream)
{
	SDL_Event event;
	double incr, pos, frac;

	for (;;) {
		double x;
		refresh_loop_wait_event(cur_stream, &event);
		switch (event.type) {
		case SDL_KEYDOWN:
			if (exit_on_keydown) {
				do_exit(cur_stream);
				break;
			}
			switch (event.key.keysym.sym) {
			case SDLK_ESCAPE:
			case SDLK_q:
				do_exit(cur_stream);
				break;
			case SDLK_f:
				toggle_full_screen(cur_stream);
				cur_stream->force_refresh = 1;
				break;
			case SDLK_p:
			case SDLK_SPACE:
				toggle_pause(cur_stream);
				break;
			case SDLK_m:
				toggle_mute(cur_stream);
				break;
			case SDLK_KP_MULTIPLY:
			case SDLK_0:
				update_volume(cur_stream, 1, SDL_VOLUME_STEP);
				break;
			case SDLK_KP_DIVIDE:
			case SDLK_9:
				update_volume(cur_stream, -1, SDL_VOLUME_STEP);
				break;
			case SDLK_s: // S: Step to next frame
				step_to_next_frame(cur_stream);
				break;
			case SDLK_a:
				stream_cycle_channel(cur_stream, AVMEDIA_TYPE_AUDIO);
				break;
			case SDLK_v:
				stream_cycle_channel(cur_stream, AVMEDIA_TYPE_VIDEO);
				break;
			case SDLK_c:
				stream_cycle_channel(cur_stream, AVMEDIA_TYPE_VIDEO);
				stream_cycle_channel(cur_stream, AVMEDIA_TYPE_AUDIO);
				stream_cycle_channel(cur_stream, AVMEDIA_TYPE_SUBTITLE);
				break;
			case SDLK_t:
				stream_cycle_channel(cur_stream, AVMEDIA_TYPE_SUBTITLE);
				break;
			case SDLK_w:
#if CONFIG_AVFILTER
				if (cur_stream->show_mode == SHOW_MODE_VIDEO && cur_stream->vfilter_idx < nb_vfilters - 1) {
					if (++cur_stream->vfilter_idx >= nb_vfilters)
						cur_stream->vfilter_idx = 0;
				}
				else {
					cur_stream->vfilter_idx = 0;
					toggle_audio_display(cur_stream);
				}
#else
				toggle_audio_display(cur_stream);
#endif
				break;
			case SDLK_PAGEUP:
				if (cur_stream->ic->nb_chapters <= 1) {
					incr = 600.0;
					goto do_seek;
				}
				seek_chapter(cur_stream, 1);
				break;
			case SDLK_PAGEDOWN:
				if (cur_stream->ic->nb_chapters <= 1) {
					incr = -600.0;
					goto do_seek;
				}
				seek_chapter(cur_stream, -1);
				break;
			case SDLK_LEFT:
				incr = -10.0;
				goto do_seek;
			case SDLK_RIGHT:
				incr = 10.0;
				goto do_seek;
			case SDLK_UP:
				incr = 60.0;
				goto do_seek;
			case SDLK_DOWN:
				incr = -60.0;
			do_seek:
				if (seek_by_bytes) {
					pos = -1;
					if (pos < 0 && cur_stream->video_stream >= 0)
						pos = frame_queue_last_pos(&cur_stream->pictq);
					if (pos < 0 && cur_stream->audio_stream >= 0)
						pos = frame_queue_last_pos(&cur_stream->sampq);
					if (pos < 0)
						pos = avio_tell(cur_stream->ic->pb);
					if (cur_stream->ic->bit_rate)
						incr *= cur_stream->ic->bit_rate / 8.0;
					else
						incr *= 180000.0;
					pos += incr;
					stream_seek(cur_stream, pos, incr, 1);
				}
				else {
					pos = get_master_clock(cur_stream);
					if (isnan(pos))
						pos = (double)cur_stream->seek_pos / AV_TIME_BASE;
					pos += incr;
					if (cur_stream->ic->start_time != AV_NOPTS_VALUE && pos < cur_stream->ic->start_time / (double)AV_TIME_BASE)
						pos = cur_stream->ic->start_time / (double)AV_TIME_BASE;
					stream_seek(cur_stream, (int64_t)(pos * AV_TIME_BASE), (int64_t)(incr * AV_TIME_BASE), 0);
				}
				break;
			default:
				break;
			}
			break;
		case SDL_MOUSEBUTTONDOWN:
			if (exit_on_mousedown) {
				do_exit(cur_stream);
				break;
			}
			if (event.button.button == SDL_BUTTON_LEFT) {
				static int64_t last_mouse_left_click = 0;
				if (av_gettime_relative() - last_mouse_left_click <= 500000) {
					toggle_full_screen(cur_stream);
					cur_stream->force_refresh = 1;
					last_mouse_left_click = 0;
				}
				else {
					last_mouse_left_click = av_gettime_relative();
				}
			}
		case SDL_MOUSEMOTION:
			if (cursor_hidden) {
				SDL_ShowCursor(1);
				cursor_hidden = 0;
			}
			cursor_last_shown = av_gettime_relative();
			if (event.type == SDL_MOUSEBUTTONDOWN) {
				if (event.button.button != SDL_BUTTON_RIGHT)
					break;
				x = event.button.x;
			}
			else {
				if (!(event.motion.state & SDL_BUTTON_RMASK))
					break;
				x = event.motion.x;
			}
			if (seek_by_bytes || cur_stream->ic->duration <= 0) {
				uint64_t size = avio_size(cur_stream->ic->pb);
				stream_seek(cur_stream, size*x / cur_stream->width, 0, 1);
			}
			else {
				int64_t ts;
				int ns, hh, mm, ss;
				int tns, thh, tmm, tss;
				tns = cur_stream->ic->duration / 1000000LL;
				thh = tns / 3600;
				tmm = (tns % 3600) / 60;
				tss = (tns % 60);
				frac = x / cur_stream->width;
				ns = frac * tns;
				hh = ns / 3600;
				mm = (ns % 3600) / 60;
				ss = (ns % 60);
				av_log(NULL, AV_LOG_INFO,
					"Seek to %2.0f%% (%2d:%02d:%02d) of total duration (%2d:%02d:%02d)       \n", frac * 100,
					hh, mm, ss, thh, tmm, tss);
				ts = frac * cur_stream->ic->duration;
				if (cur_stream->ic->start_time != AV_NOPTS_VALUE)
					ts += cur_stream->ic->start_time;
				stream_seek(cur_stream, ts, 0, 0);
			}
			break;
		case SDL_WINDOWEVENT:
			switch (event.window.event) {
			case SDL_WINDOWEVENT_RESIZED:
				screen_width = cur_stream->width = event.window.data1;
				screen_height = cur_stream->height = event.window.data2;
				if (cur_stream->vis_texture) {
					SDL_DestroyTexture(cur_stream->vis_texture);
					cur_stream->vis_texture = NULL;
				}
			case SDL_WINDOWEVENT_EXPOSED:
				cur_stream->force_refresh = 1;
			}
			break;
		case SDL_QUIT:
		case FF_QUIT_EVENT:
			do_exit(cur_stream);
			break;
		case FF_ALLOC_EVENT:
			alloc_picture((VideoState*)event.user.data1);
			break;
		case FF_SEEK_BAR_EVENT:
			{
				if (cur_stream->ic){
						if (seek_by_bytes || cur_stream->ic->duration <= 0) {
						uint64_t size = avio_size(cur_stream->ic->pb);
						stream_seek(cur_stream, size*seek_bar_pos / 1000, 0, 1);
						}
						else {
						int64_t ts;
						frac = (double)seek_bar_pos / 1000;
						ts = frac * cur_stream->ic->duration;
						if (cur_stream->ic->start_time != AV_NOPTS_VALUE)
						ts += cur_stream->ic->start_time;
						stream_seek(cur_stream, ts, 0, 0);
						}
					}
				break;
			}
		default:
			break;
		}
	}
}

int CorePlayer::opt_frame_size(void *optctx, const char *opt, const char *arg)
{
	av_log(NULL, AV_LOG_WARNING, "Option -s is deprecated, use -video_size.\n");
	return opt_default(NULL, "video_size", arg);
}

int CorePlayer::opt_default(void *optctx, const char *opt, const char *arg)
{
	const AVOption *o;
	int consumed = 0;
	char opt_stripped[128];
	const char *p;
	const AVClass *cc = avcodec_get_class(), *fc = avformat_get_class();
#if CONFIG_AVRESAMPLE
	const AVClass *rc = avresample_get_class();
#endif
#if CONFIG_SWSCALE
	const AVClass *sc = sws_get_class();
#endif
#if CONFIG_SWRESAMPLE
	const AVClass *swr_class = swr_get_class();
#endif

	if (!strcmp(opt, "debug") || !strcmp(opt, "fdebug"))
		av_log_set_level(AV_LOG_DEBUG);

	if (!(p = strchr(opt, ':')))
		p = opt + strlen(opt);
	av_strlcpy(opt_stripped, opt, FFMIN(sizeof(opt_stripped), p - opt + 1));

	if ((o = opt_find(&cc, opt_stripped, NULL, 0,
		AV_OPT_SEARCH_CHILDREN | AV_OPT_SEARCH_FAKE_OBJ)) ||
		((opt[0] == 'v' || opt[0] == 'a' || opt[0] == 's') &&
		(o = opt_find(&cc, opt + 1, NULL, 0, AV_OPT_SEARCH_FAKE_OBJ)))) {
		av_dict_set(&codec_opts, opt, arg, FLAGS);
		consumed = 1;
	}
	if ((o = opt_find(&fc, opt, NULL, 0,
		AV_OPT_SEARCH_CHILDREN | AV_OPT_SEARCH_FAKE_OBJ))) {
		av_dict_set(&format_opts, opt, arg, FLAGS);
		if (consumed)
			av_log(NULL, AV_LOG_VERBOSE, "Routing option %s to both codec and muxer layer\n", opt);
		consumed = 1;
	}
#if CONFIG_SWSCALE
	if (!consumed && (o = opt_find(&sc, opt, NULL, 0,
		AV_OPT_SEARCH_CHILDREN | AV_OPT_SEARCH_FAKE_OBJ))) {
		struct SwsContext *sws = sws_alloc_context();
		int ret = av_opt_set(sws, opt, arg, 0);
		sws_freeContext(sws);
		if (!strcmp(opt, "srcw") || !strcmp(opt, "srch") ||
			!strcmp(opt, "dstw") || !strcmp(opt, "dsth") ||
			!strcmp(opt, "src_format") || !strcmp(opt, "dst_format")) {
			av_log(NULL, AV_LOG_ERROR, "Directly using swscale dimensions/format options is not supported, please use the -s or -pix_fmt options\n");
			return AVERROR(EINVAL);
		}
		if (ret < 0) {
			av_log(NULL, AV_LOG_ERROR, "Error setting option %s.\n", opt);
			return ret;
		}

		av_dict_set(&sws_dict, opt, arg, FLAGS);

		consumed = 1;
	}
#else
	if (!consumed && !strcmp(opt, "sws_flags")) {
		av_log(NULL, AV_LOG_WARNING, "Ignoring %s %s, due to disabled swscale\n", opt, arg);
		consumed = 1;
	}
#endif
#if CONFIG_SWRESAMPLE
	if (!consumed && (o = opt_find(&swr_class, opt, NULL, 0,
		AV_OPT_SEARCH_CHILDREN | AV_OPT_SEARCH_FAKE_OBJ))) {
		struct SwrContext *swr = swr_alloc();
		int ret = av_opt_set(swr, opt, arg, 0);
		swr_free(&swr);
		if (ret < 0) {
			av_log(NULL, AV_LOG_ERROR, "Error setting option %s.\n", opt);
			return ret;
		}
		av_dict_set(&swr_opts, opt, arg, FLAGS);
		consumed = 1;
	}
#endif
#if CONFIG_AVRESAMPLE
	if ((o = opt_find(&rc, opt, NULL, 0,
		AV_OPT_SEARCH_CHILDREN | AV_OPT_SEARCH_FAKE_OBJ))) {
		av_dict_set(&resample_opts, opt, arg, FLAGS);
		consumed = 1;
	}
#endif

	if (consumed)
		return 0;
	return AVERROR_OPTION_NOT_FOUND;
}

const AVOption * CorePlayer::opt_find(void *obj, const char *name, const char *unit, int opt_flags, int search_flags)
{
	const AVOption *o = av_opt_find(obj, name, unit, opt_flags, search_flags);
	if (o && !o->flags)
		return NULL;
	return o;
}

int CorePlayer::opt_width(void *optctx, const char *opt, const char *arg)
{
	screen_width = parse_number_or_die(opt, arg, OPT_INT64, 1, INT_MAX);
	return 0;
}

double CorePlayer::parse_number_or_die(const char *context, const char *numstr, int type, double min, double max)
{
	char *tail;
	const char *error;
	double d = av_strtod(numstr, &tail);
	if (*tail)
		error = "Expected number for %s but found: %s\n";
	else if (d < min || d > max)
		error = "The value for %s was %s which is not within %f - %f\n";
	else if (type == OPT_INT64 && (int64_t)d != d)
		error = "Expected int64 for %s but found %s\n";
	else if (type == OPT_INT && (int)d != d)
		error = "Expected int for %s but found %s\n";
	else
		return d;
	av_log(NULL, AV_LOG_FATAL, error, context, numstr, min, max);
	exit_program(1);
	return 0;
}

int CorePlayer::opt_height(void *optctx, const char *opt, const char *arg)
{
	screen_height = parse_number_or_die(opt, arg, OPT_INT64, 1, INT_MAX);
	return 0;
}

int CorePlayer::opt_format(void *optctx, const char *opt, const char *arg)
{
	file_iformat = av_find_input_format(arg);
	if (!file_iformat) {
		av_log(NULL, AV_LOG_FATAL, "Unknown input format: %s\n", arg);
		return AVERROR(EINVAL);
	}
	return 0;
}

int CorePlayer::opt_frame_pix_fmt(void *optctx, const char *opt, const char *arg)
{
	av_log(NULL, AV_LOG_WARNING, "Option -pix_fmt is deprecated, use -pixel_format.\n");
	return opt_default(NULL, "pixel_format", arg);
}

int CorePlayer::opt_sync(void *optctx, const char *opt, const char *arg)
{
	if (!strcmp(arg, "audio"))
		av_sync_type = AV_SYNC_AUDIO_MASTER;
	else if (!strcmp(arg, "video"))
		av_sync_type = AV_SYNC_VIDEO_MASTER;
	else if (!strcmp(arg, "ext"))
		av_sync_type = AV_SYNC_EXTERNAL_CLOCK;
	else {
		av_log(NULL, AV_LOG_ERROR, "Unknown value for %s: %s\n", opt, arg);
		exit(1);
	}
	return 0;
}

int CorePlayer::opt_seek(void *optctx, const char *opt, const char *arg)
{
	start_time = parse_time_or_die(opt, arg, 1);
	return 0;
}

int64_t CorePlayer::parse_time_or_die(const char *context, const char *timestr, int is_duration)
{
	int64_t us;
	if (av_parse_time(&us, timestr, is_duration) < 0) {
		av_log(NULL, AV_LOG_FATAL, "Invalid %s specification for %s: %s\n",
			is_duration ? "duration" : "date", context, timestr);
		exit_program(1);
	}
	return us;
}

int CorePlayer::opt_duration(void *optctx, const char *opt, const char *arg)
{
	duration = parse_time_or_die(opt, arg, 1);
	return 0;
}

int CorePlayer::opt_show_mode(void *optctx, const char *opt, const char *arg)
{
	show_mode = (ShowMode)(!strcmp(arg, "video") ? SHOW_MODE_VIDEO :
		!strcmp(arg, "waves") ? SHOW_MODE_WAVES :
		!strcmp(arg, "rdft") ? SHOW_MODE_RDFT :
		(int)parse_number_or_die(opt, arg, OPT_INT, 0, SHOW_MODE_NB - 1));
	return 0;
}

void CorePlayer::opt_input_file(void *optctx, const char *filename)
{
	if (input_filename) {
		av_log(NULL, AV_LOG_FATAL,
			"Argument '%s' provided as input filename, but '%s' was already specified.\n",
			filename, input_filename);
		exit(1);
	}
	if (!strcmp(filename, "-"))
		filename = "pipe:";
	input_filename = filename;
}

int CorePlayer::opt_codec(void *optctx, const char *opt, const char *arg)
{
	const char *spec = strchr(opt, ':');
	if (!spec) {
		av_log(NULL, AV_LOG_ERROR,
			"No media specifier was specified in '%s' in option '%s'\n",
			arg, opt);
		return AVERROR(EINVAL);
	}
	spec++;
	switch (spec[0]) {
	case 'a':    audio_codec_name = arg; break;
	case 's': subtitle_codec_name = arg; break;
	case 'v':    video_codec_name = arg; break;
	default:
		av_log(NULL, AV_LOG_ERROR,
			"Invalid media specifier '%s' in option '%s'\n", spec, opt);
		return AVERROR(EINVAL);
	}
	return 0;
}

void CorePlayer::show_usage(void)
{
	av_log(NULL, AV_LOG_INFO, "Simple media player\n");
	av_log(NULL, AV_LOG_INFO, "usage: %s [options] input_file\n", program_name);
	av_log(NULL, AV_LOG_INFO, "\n");
}

void CorePlayer::init_dynload(void)
{
	//SetDllDirectory(L"");
}

void CorePlayer::init_opts(void)
{
	av_dict_set(&sws_dict, "flags", "bicubic", 0);
}

//int CorePlayer::lockmgr(enum AVLockOp op)
//{
//	switch (op) {
//	case AV_LOCK_CREATE:
//		wait_mutex = SDL_CreateMutex();
//		if (!wait_mutex) {
//			av_log(NULL, AV_LOG_FATAL, "SDL_CreateMutex(): %s\n", SDL_GetError());
//			return 1;
//		}
//		return 0;
//	case AV_LOCK_OBTAIN:
//		return !!SDL_LockMutex(wait_mutex);
//	case AV_LOCK_RELEASE:
//		return !!SDL_UnlockMutex(wait_mutex);
//	case AV_LOCK_DESTROY:
//		SDL_DestroyMutex(wait_mutex);
//		return 0;
//	}
//	return 1;
//}

int CorePlayer::kaka_play()
{
	int flags;
	init_dynload();

	init_opts();

	//TODO
	signal(SIGINT, sigterm_handler_1); /* Interrupt (ANSI).    */
	signal(SIGTERM, sigterm_handler_1); /* Termination (ANSI).  */

	//	show_banner(argc, argv, options);

	//	parse_options(NULL, argc, argv, options, opt_input_file);

	if (!input_filename) {
		show_usage();
		av_log(NULL, AV_LOG_FATAL, "An input file must be specified\n");
		av_log(NULL, AV_LOG_FATAL,
			"Use -h to get full help or, even better, run 'man %s'\n", program_name);
		exit(1);
	}

	if (display_disable) {
		video_disable = 1;
	}
	flags = SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER;
	if (audio_disable)
		flags &= ~SDL_INIT_AUDIO;
	else {
		/* Try to work around an occasional ALSA buffer underflow issue when the
		* period size is NPOT due to ALSA resampling by forcing the buffer size. */
		if (!SDL_getenv("SDL_AUDIO_ALSA_SET_BUFFER_SIZE"))
			SDL_setenv("SDL_AUDIO_ALSA_SET_BUFFER_SIZE", "1", 1);
	}
	if (display_disable)
		flags &= ~SDL_INIT_VIDEO;
	if (SDL_Init(flags)) {
		av_log(NULL, AV_LOG_FATAL, "Could not initialize SDL - %s\n", SDL_GetError());
		av_log(NULL, AV_LOG_FATAL, "(Did you set the DISPLAY variable?)\n");
		exit(1);
	}

	SDL_EventState(SDL_SYSWMEVENT, SDL_IGNORE);
	SDL_EventState(SDL_USEREVENT, SDL_IGNORE);

	//TODO
	if (av_lockmgr_register(lockmgr_1)) {
		av_log(NULL, AV_LOG_FATAL, "Could not initialize lock manager!\n");
		do_exit(NULL);
	}

	av_init_packet(&flush_pkt);
	flush_pkt.data = (uint8_t *)&flush_pkt;

	m_is = stream_open(input_filename, file_iformat);

	if (!m_is) {
		av_log(NULL, AV_LOG_FATAL, "Failed to initialize VideoState!\n");
		do_exit(NULL);
	}
	else
	{
		//TODO
		m_is->read_tid = SDL_CreateThread(read_thread_1, "read_thread", this);
		if (!m_is->read_tid) {
			av_log(NULL, AV_LOG_FATAL, "SDL_CreateThread(): %s\n", SDL_GetError());
			stream_close(m_is);
			do_exit(NULL);
		}
	}

	event_loop(m_is);

	/* never returns */

	return 0;
}

void CorePlayer::start_play(const char* filename, HWND hWnd, bool is_audio)
{
	au_or_vedio = is_audio;
	input_filename = filename;
	win_hwnd = hWnd;
	std::thread thd(std::bind(&CorePlayer::kaka_play,this));
	thd.detach();
}

void CorePlayer::pause()
{
	SDL_Event event;
	event.type = SDL_KEYDOWN;
	event.key.keysym.sym = SDLK_p;
	SDL_PushEvent(&event);
}

void CorePlayer::play()
{
	pause();
}

void CorePlayer::seek_pos(int pos)
{
	SDL_Event event;
	event.type = FF_SEEK_BAR_EVENT;
	seek_bar_pos = pos;
	SDL_PushEvent(&event);
}

void CorePlayer::set_st_tm_cb(TimeLabelCb s_cb)
{
	start_label_cb_ = s_cb;
}

void CorePlayer::set_ed_tm_cb(TimeLabelCb e_cb)
{
	end_label_cb_ = e_cb;
}

void CorePlayer::set_win_size(WinSizeCb cb)
{
	win_size_cb_ = cb;
}

void CorePlayer::init_params(VideoState *is)
{
	//屏幕尺寸自适应  后期调研

	AVFormatContext *pFormatCtx = is->ic;
	if (pFormatCtx!=NULL)
	{
		//转换成hh:mm:ss形式

		int tns, thh, tmm, tss;
		tns = (pFormatCtx->duration) / 1000000;
		thh = tns / 3600;
		tmm = (tns % 3600) / 60;
		tss = (tns % 60);
		//timelong.Format(_T("%02d:%02d:%02d"),thh,tmm,tss);
		std::wstring timelong = nbase::StringPrintf(L"%02d:%02d:%02d", thh, tmm, tss);

		if (end_label_cb_)
		{
			end_label_cb_(timelong);
		}
	}

}

void CorePlayer::set_pro_val_cb(ProgressValCb p_cb)
{
	progress_val_cb_ = p_cb;
}

