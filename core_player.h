#pragma once
#include "common_args.h"

using TimeLabelCb = std::function<void(int)>;
using WinSizeCb = std::function<void(int, int)>;
using ProgressValCb = std::function<void(int)>;

class CorePlayer
{
public:
	CorePlayer();
	~CorePlayer();

public:
	int kaka_play();
	void start_play(const char* filename, HWND hWnd,bool is_audio=false);
	void sdl_audio_callback(Uint8 *stream, int len);
	VideoState * GetVideoState(){ return m_is; }
	int decode_interrupt_cb();
	int read_thread();
	int audio_thread();
	int video_thread();
	int subtitle_thread();
	void pause();
	void play();
	void eixt();
	void seek_pos(int pos);
	void set_win_size(WinSizeCb cb);
	void set_st_tm_cb(TimeLabelCb s_cb);
	void set_ed_tm_cb(TimeLabelCb e_cb);
	void set_pro_val_cb(ProgressValCb p_cb);

private:
	void init_params(VideoState *is);  //屏幕尺寸大小和结束时间
	int opt_add_vfilter(void *optctx, const char *opt, const char *arg);
	inline int cmp_audio_fmts(enum AVSampleFormat fmt1, int64_t channel_count1, enum AVSampleFormat fmt2, int64_t channel_count2)
	{
		/* If channel count == 1, planar and non-planar formats are the same */
		if (channel_count1 == 1 && channel_count2 == 1)
			return av_get_packed_sample_fmt(fmt1) != av_get_packed_sample_fmt(fmt2);
		else
			return channel_count1 != channel_count2 || fmt1 != fmt2;
	}

	inline void fill_rectangle(int x, int y, int w, int h)
	{
		SDL_Rect rect;
		rect.x = x;
		rect.y = y;
		rect.w = w;
		rect.h = h;
		if (w && h)
			SDL_RenderFillRect(renderer, &rect);
	}

	inline int compute_mod(int a, int b)
	{
		return a < 0 ? a%b + b : a%b;
	}

	void free_picture(Frame *vp);
	int packet_queue_put_private(PacketQueue *q, AVPacket *pkt);
	int packet_queue_put(PacketQueue *q, AVPacket *pkt);
	int packet_queue_put_nullpacket(PacketQueue *q, int stream_index);
	int packet_queue_init(PacketQueue *q);
	void packet_queue_flush(PacketQueue *q);
	void packet_queue_destroy(PacketQueue *q);
	void packet_queue_abort(PacketQueue *q);
	void packet_queue_start(PacketQueue *q);
	int packet_queue_get(PacketQueue *q, AVPacket *pkt, int block, int *serial);
	void decoder_init(Decoder *d, AVCodecContext *avctx, PacketQueue *queue, SDL_cond *empty_queue_cond);
	int decoder_decode_frame(Decoder *d, AVFrame *frame, AVSubtitle *sub);
	void decoder_destroy(Decoder *d);
	void frame_queue_unref_item(Frame *vp);
	int frame_queue_init(FrameQueue *f, PacketQueue *pktq, int max_size, int keep_last);
	void frame_queue_destory(FrameQueue *f);
	void frame_queue_signal(FrameQueue *f);
	Frame *frame_queue_peek(FrameQueue *f);
	Frame *frame_queue_peek_next(FrameQueue *f);
	Frame *frame_queue_peek_last(FrameQueue *f);
	Frame *frame_queue_peek_writable(FrameQueue *f);
	Frame *frame_queue_peek_readable(FrameQueue *f);
	void frame_queue_push(FrameQueue *f);
	void frame_queue_next(FrameQueue *f);
	int frame_queue_nb_remaining(FrameQueue *f);
	int64_t frame_queue_last_pos(FrameQueue *f);
	void decoder_abort(Decoder *d, FrameQueue *fq);
	int realloc_texture(SDL_Texture **texture, Uint32 new_format, int new_width, int new_height, SDL_BlendMode blendmode, int init_texture);
	void calculate_display_rect(SDL_Rect *rect, int scr_xleft, int scr_ytop, int scr_width, int scr_height, int pic_width, int pic_height, AVRational pic_sar);
	int upload_texture(SDL_Texture *tex, AVFrame *frame, struct SwsContext **img_convert_ctx);
	void video_image_display(VideoState *is);
	void video_audio_display(VideoState *s);
	void stream_component_close(VideoState *is, int stream_index);
	void stream_close(VideoState *is);
	void do_exit(VideoState *is);
	void uninit_opts(void);
	void sigterm_handler(int sig);
	void set_default_window_size(int width, int height, AVRational sar);
	int video_open(VideoState *is, Frame *vp);
	void video_display(VideoState *is);
	double get_clock(Clock *c);
	void set_clock_at(Clock *c, double pts, int serial, double time);
	void set_clock(Clock *c, double pts, int serial);
	void set_clock_speed(Clock *c, double speed);
	void init_clock(Clock *c, int *queue_serial);
	void sync_clock_to_slave(Clock *c, Clock *slave);
	int get_master_sync_type(VideoState *is);
	double get_master_clock(VideoState *is);
	void check_external_clock_speed(VideoState *is);
	void stream_seek(VideoState *is, int64_t pos, int64_t rel, int seek_by_bytes);
	void stream_toggle_pause(VideoState *is);
	void toggle_pause(VideoState *is);
	void toggle_mute(VideoState *is);
	void update_volume(VideoState *is, int sign, int step);
	void step_to_next_frame(VideoState *is);
	double compute_target_delay(double delay, VideoState *is);
	double vp_duration(VideoState *is, Frame *vp, Frame *nextvp);
	void update_video_pts(VideoState *is, double pts, int64_t pos, int serial);
	void video_refresh(void *opaque, double *remaining_time);
	void alloc_picture(VideoState *is);
	int queue_picture(VideoState *is, AVFrame *src_frame, double pts, double duration, int64_t pos, int serial);
	int get_video_frame(VideoState *is, AVFrame *frame);
	int configure_filtergraph(AVFilterGraph *graph, const char *filtergraph, AVFilterContext *source_ctx, AVFilterContext *sink_ctx);
	int configure_video_filters(AVFilterGraph *graph, VideoState *is, const char *vfilters, AVFrame *frame);
	double get_rotation(AVStream *st);
	int configure_audio_filters(VideoState *is, const char *afilters, int force_output_format);
	
	int64_t get_valid_channel_layout(int64_t channel_layout, int channels);
	int decoder_start(Decoder *d, int(*fn)(void *), void *arg);
	
	
	void update_sample_display(VideoState *is, short *samples, int samples_size);
	int synchronize_audio(VideoState *is, int nb_samples);
	int audio_decode_frame(VideoState *is);
	
	int audio_open(void *opaque, int64_t wanted_channel_layout, int wanted_nb_channels, int wanted_sample_rate, struct AudioParams *audio_hw_params);
	int stream_component_open(VideoState *is, int stream_index);
	AVDictionary *filter_codec_opts(AVDictionary *opts, enum AVCodecID codec_id,AVFormatContext *s, AVStream *st, AVCodec *codec);
	int check_stream_specifier(AVFormatContext *s, AVStream *st, const char *spec);
	void exit_program(int ret);
	
	int stream_has_enough_packets(AVStream *st, int stream_id, PacketQueue *queue);
	int is_realtime(AVFormatContext *s);
	
	AVDictionary **setup_find_stream_info_opts(AVFormatContext *s,AVDictionary *codec_opts);
	VideoState *stream_open(const char *filename, AVInputFormat *iformat);
	void stream_cycle_channel(VideoState *is, int codec_type);
	void toggle_full_screen(VideoState *is);
	void toggle_audio_display(VideoState *is);
	void refresh_loop_wait_event(VideoState *is, SDL_Event *event);
	void seek_chapter(VideoState *is, int incr);
	void event_loop(VideoState *cur_stream);
	int opt_frame_size(void *optctx, const char *opt, const char *arg);
	int opt_default(void *optctx, const char *opt, const char *arg);
	const AVOption *opt_find(void *obj, const char *name, const char *unit, int opt_flags, int search_flags);
	int opt_width(void *optctx, const char *opt, const char *arg);
	double parse_number_or_die(const char *context, const char *numstr, int type,double min, double max);
	int opt_height(void *optctx, const char *opt, const char *arg);
	int opt_format(void *optctx, const char *opt, const char *arg);
	int opt_frame_pix_fmt(void *optctx, const char *opt, const char *arg);
	int opt_sync(void *optctx, const char *opt, const char *arg);
	int opt_seek(void *optctx, const char *opt, const char *arg);
	int64_t parse_time_or_die(const char *context, const char *timestr,int is_duration);
	int opt_duration(void *optctx, const char *opt, const char *arg);
	int opt_show_mode(void *optctx, const char *opt, const char *arg);
	void opt_input_file(void *optctx, const char *filename);
	int opt_codec(void *optctx, const char *opt, const char *arg);
	void show_usage(void);
	void init_dynload(void);
	void init_opts(void);
	
protected:
private:
	VideoState *m_is = NULL;
	AVInputFormat *file_iformat = NULL;
	const char *input_filename = NULL;
	const char *window_title = NULL;
	HWND win_hwnd;
	int default_width = 640;
	int default_height = 480;
	int screen_width = 0;
	int screen_height = 0;
	int audio_disable = 0;
	int video_disable = 0;
	int subtitle_disable = 0;
	char* wanted_stream_spec[AVMEDIA_TYPE_NB];
	int seek_by_bytes = -1;
	int display_disable = 0;
	int show_status = 1;
	int av_sync_type = AV_SYNC_AUDIO_MASTER;
	int64_t start_time = AV_NOPTS_VALUE;
	int64_t duration = AV_NOPTS_VALUE;
	int fast = 0;
	int genpts = 0;
	int lowres = 0;
	int decoder_reorder_pts = -1;
	int autoexit = 0;
	int exit_on_keydown = 0;
	int exit_on_mousedown = 0;
	int loop = 1;
	int framedrop = -1;
	int infinite_buffer = -1;
	enum ShowMode show_mode = SHOW_MODE_NONE;
	const char *audio_codec_name = NULL;
	const char *subtitle_codec_name = NULL;
	const char *video_codec_name = NULL;
	double rdftspeed = 0.02;
	int64_t cursor_last_shown = 0;
	int cursor_hidden = 0;
	int64_t last_time = 0;
	const char **vfilters_list = NULL;
	int nb_vfilters = 0;
	char *afilters = NULL;

	int autorotate = 1;

	/* current context */
	int is_full_screen = 0;
	int64_t audio_callback_time = 0;

	AVPacket flush_pkt;
	SDL_Window *window = NULL;
	SDL_Renderer *renderer = NULL;

	//from cmdutils
	AVDictionary *sws_dict = NULL;
	AVDictionary *swr_opts = NULL;
	AVDictionary *format_opts = NULL, *codec_opts = NULL, *resample_opts = NULL;
	
private:
	bool au_or_vedio = false;
	double pos_scale = 0.0;     //上一次播放记录
	int seek_bar_pos =0;		//进度条拖动
	TimeLabelCb start_label_cb_;
	TimeLabelCb end_label_cb_;
	WinSizeCb win_size_cb_;
	ProgressValCb progress_val_cb_;
};