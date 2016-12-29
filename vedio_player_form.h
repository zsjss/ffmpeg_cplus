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
	void OnDelayPlayNext();
	void OnPlayNext();
	void OnHandlePlayNext();
	void OnHandleItemPlay();
	void OnSchedualStateCB();  //外部定时回调
	void DoStateCB();

protected:
private:
	HWND hwnd_;
	int vedio_list_index_ = 0;
	bool is_first_ = true;
	bool slider_change_ = false;
	int slider_change_pos_ = 0;
	int end_ts_ = 0;
	int current_ts_ = 0;
	ui::Label* start_time_;
	ui::Label* end_time_;
	ui::Slider* input_vedio_;
	ui::Button* btn_play_;
	ui::Button* btn_pause_;
	ui::Control* vedio_show_;
	std::string play_url_;
	ui::ListBox* item_list_;
	ui::VBox* list_box_;

	VedioPlayer vedio_player_;
	nbase::WeakCallbackFlag vedio_state_timer_;  //外部回调定时器
	StdClosure vedio_state_call_back_;   //外部回调使用
};