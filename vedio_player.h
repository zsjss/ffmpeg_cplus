#pragma once
#include "core_player.h"

class VedioPlayer
{
public:
	VedioPlayer();
	~VedioPlayer();

	void Init();
	void Pause();
	void DoExit();
	void Play(const char* filename, HWND hWnd);
	void SeekPos(int pos);
	void SetStartTimeCb(TimeLabelCb start_cb);
	void SetEndTimeCb(TimeLabelCb end_cb);
	void SetProgressCb(ProgressValCb pro_cb);
	void SetWinSizeCb(WinSizeCb winsize_cb);

protected:
private:
	CorePlayer co_player_;
};