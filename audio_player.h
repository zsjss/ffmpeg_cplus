#pragma once
#include "core_player.h"


class AudioPlayer
{
public:
	AudioPlayer();
	~AudioPlayer();

	void Init();
	void Pause();
	void Play(const char* filename, HWND hWnd);
	void SeekPos(int pos);
	void SetStartTimeCb(TimeLabelCb start_cb);
	void SetEndTimeCb(TimeLabelCb end_cb);
	void SetProgressCb(ProgressValCb pro_cb);
private:
	CorePlayer co_player_;
};