#include "common_utils.h"
#include "core_player.h"


void log_callback_help(void* ptr, int level, const char* fmt, va_list vl)
{
	vfprintf(stdout, fmt, vl);
}

void *grow_array(void *array, int elem_size, int *size, int new_size)
{
	if (new_size >= INT_MAX / elem_size) {
		av_log(NULL, AV_LOG_ERROR, "Array too big.\n");
	}
	if (*size < new_size) {
		uint8_t *tmp = (uint8_t*)av_realloc_array(array, new_size, elem_size);
		if (!tmp) {
			av_log(NULL, AV_LOG_ERROR, "Could not alloc buffer.\n");
		}
		memset(tmp + *size*elem_size, 0, (new_size - *size) * elem_size);
		*size = new_size;
		return tmp;
	}
	return array;
}

void sdl_audio_callback_1(void *opaque, Uint8 *stream, int len)
{
	CorePlayer* audio_player = (CorePlayer*)opaque;
	if (audio_player)
	{
		audio_player->sdl_audio_callback(stream, len);
	}
}

int decode_interrupt_cb_1(void *ctx)
{
	int ret = 0;
	CorePlayer* audio_player = (CorePlayer*)ctx;
	if (audio_player)
	{
		ret = audio_player->decode_interrupt_cb();
	}
	return ret;
}

int read_thread_1(void *arg)
{
	int ret = 0;
	CorePlayer* audio_player = (CorePlayer*)arg;
	if (audio_player)
	{
		ret = audio_player->read_thread();
	}
	return ret;
}

void sigterm_handler_1(int sig)
{
	exit(123);
}

int lockmgr_1(void **mtx, enum AVLockOp op)
{
	switch (op) {
	case AV_LOCK_CREATE:
		*mtx = SDL_CreateMutex();
		if (!*mtx) {
			av_log(NULL, AV_LOG_FATAL, "SDL_CreateMutex(): %s\n", SDL_GetError());
			return 1;
		}
		return 0;
	case AV_LOCK_OBTAIN:
		return !!SDL_LockMutex((SDL_mutex*)*mtx);
	case AV_LOCK_RELEASE:
		return !!SDL_UnlockMutex((SDL_mutex*)*mtx);
	case AV_LOCK_DESTROY:
		SDL_DestroyMutex((SDL_mutex*)*mtx);
		return 0;
	}
	return 1;
}

int audio_thread_1(void *arg)
{
	int ret = 0;
	CorePlayer* audio_player = (CorePlayer*)arg;
	if (audio_player)
	{
		ret = audio_player->audio_thread();
	}
	return ret;
}

int subtitle_thread_1(void *arg)
{
	int ret = 0;
	CorePlayer* audio_player = (CorePlayer*)arg;
	if (audio_player)
	{
		ret = audio_player->subtitle_thread();
	}
	return ret;
}

int video_thread_1(void *arg)
{
	int ret = 0;
	CorePlayer* audio_player = (CorePlayer*)arg;
	if (audio_player)
	{
		ret = audio_player->video_thread();
	}
	return ret;
}

void InitAudio()
{
	av_log_set_flags(AV_LOG_SKIP_REPEATED);
	//parse_loglevel(argc, argv, options);

	/* register all codecs, demux and protocols */
#if CONFIG_AVDEVICE
	avdevice_register_all();
#endif
#if CONFIG_AVFILTER
	avfilter_register_all();
#endif
	av_register_all();
	avformat_network_init();
}

