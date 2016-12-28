#include "vedio_player.h"
#include "common_utils.h"


VedioPlayer::VedioPlayer()
{

}

VedioPlayer::~VedioPlayer()
{

}

void VedioPlayer::Init()
{
	InitAudio();
}

void VedioPlayer::Pause()
{
	co_player_.pause();
}

void VedioPlayer::Play(const char* filename, HWND hWnd)
{
	Init();
	co_player_.start_play(filename, hWnd,true);
}

void VedioPlayer::SeekPos(int pos)
{
	co_player_.seek_pos(pos);
}

void VedioPlayer::SetStartTimeCb(TimeLabelCb start_cb)
{
	co_player_.set_st_tm_cb(start_cb);
}

void VedioPlayer::SetEndTimeCb(TimeLabelCb end_cb)
{
	co_player_.set_ed_tm_cb(end_cb);
}

void VedioPlayer::SetProgressCb(ProgressValCb pro_cb)
{
	co_player_.set_pro_val_cb(pro_cb);
}

void VedioPlayer::SetWinSizeCb(WinSizeCb winsize_cb)
{
	co_player_.set_win_size(winsize_cb);
}

void VedioPlayer::DoExit()
{
	co_player_.eixt();
}
