#include "audio_player.h"
#include "common_utils.h"

AudioPlayer::AudioPlayer()
{

}

AudioPlayer::~AudioPlayer()
{

}

void AudioPlayer::Init()
{
	InitAudio();
}

void AudioPlayer::Pause()
{
	co_player_.pause();
}

void AudioPlayer::Play(const char* filename, HWND hWnd)
{
	Init();
	co_player_.start_play(filename, hWnd);
}

void AudioPlayer::SeekPos(int pos)
{
	co_player_.seek_pos(pos);
}

void AudioPlayer::SetStartTimeCb(TimeLabelCb start_cb)
{
	co_player_.set_st_tm_cb(start_cb);
}

void AudioPlayer::SetEndTimeCb(TimeLabelCb end_cb)
{
	co_player_.set_ed_tm_cb(end_cb);
}

void AudioPlayer::SetProgressCb(ProgressValCb pro_cb)
{
	co_player_.set_pro_val_cb(pro_cb);
}


