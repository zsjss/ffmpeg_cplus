#pragma once
extern "C"
{
#include "SDL_stdinc.h"
}

void InitAudio();
int read_thread_1(void *arg);
int video_thread_1(void *arg);
int subtitle_thread_1(void *arg);
int audio_thread_1(void *arg);
int lockmgr_1(void **mtx, enum AVLockOp op);
void sigterm_handler_1(int sig);

void sdl_audio_callback_1(void *opaque, Uint8 *stream, int len);
int decode_interrupt_cb_1(void *ctx);
void log_callback_help(void* ptr, int level, const char* fmt, va_list vl);
void *grow_array(void *array, int elem_size, int *size, int new_size);
