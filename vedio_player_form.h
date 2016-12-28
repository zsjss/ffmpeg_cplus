#pragma once
#include "vedio_player.h"

class VedioPlayerForm : public nim_comp::WindowEx
{
public:
	VedioPlayerForm();
	~VedioPlayerForm();

	virtual std::wstring GetSkinFolder() override;
	virtual std::wstring GetSkinFile() override;
	virtual ui::UILIB_RESOURCETYPE GetResourceType() const;
	virtual std::wstring GetZIPFileName() const;

	virtual std::wstring GetWindowClassName() const override;
	virtual std::wstring GetWindowId() const override;
	virtual UINT GetClassStyle() const override;
	virtual ui::Control* CreateControl(const std::wstring& pstrClass) override;
	virtual void InitWindow() override;
	virtual void OnFinalMessage(HWND hWnd) override;
	static void ShowAudioPlay(std::string path);

public:
	static const LPCTSTR kClassName;
private:
	static LRESULT CALLBACK MyWindowProc(HWND hWnd, UINT nMsg, WPARAM wParam, LPARAM lParam);
	HWND CreateWin32(HWND parent_hwnd);

	bool OnEventClick(ui::EventArgs* args);
	bool OnSliderButtonDown(ui::EventArgs* arg);
	bool OnSliderEventChange(ui::EventArgs* arg);
	void OnPause();
	void OnPlay();
	void OnSetPlayUrl(std::string path);
	void OnDelayClose();
	void OnClose();

protected:
private:
	bool is_first_ = true;
	bool slider_change_ = false;
	int slider_change_pos_ = 0;
	ui::Label* start_time_;
	ui::Label* end_time_;
	ui::Slider* input_vedio_;
	ui::Button* btn_play_;
	ui::Button* btn_pause_;
	ui::Control* vedio_show_;
	std::string play_url_;
	VedioPlayer vedio_player_;
};