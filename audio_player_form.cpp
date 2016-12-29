#include "audio_player_form.h"
#include "resource.h"

using namespace ui;
const LPCTSTR AudioPlayerForm::kClassName = L"AudioPlayWinForm";


AudioPlayerForm::AudioPlayerForm()
{

}

AudioPlayerForm::~AudioPlayerForm()
{
}

std::wstring AudioPlayerForm::GetSkinFolder()
{
	return L"video_player";
}

std::wstring AudioPlayerForm::GetSkinFile()
{
	return L"audio_win.xml";
}

ui::UILIB_RESOURCETYPE AudioPlayerForm::GetResourceType() const
{
	return ui::UILIB_FILE;
}

std::wstring AudioPlayerForm::GetZIPFileName() const
{
	return L"VedioPlayWin.zip";
}

std::wstring AudioPlayerForm::GetWindowClassName() const
{
	return kClassName;
}

std::wstring AudioPlayerForm::GetWindowId() const
{
	return kClassName;
}

UINT AudioPlayerForm::GetClassStyle() const
{
	return (UI_CLASSSTYLE_FRAME | CS_DBLCLKS);
}

ui::Control* AudioPlayerForm::CreateControl(const std::wstring& pstrClass)
{
	return NULL;
}

void AudioPlayerForm::OnFinalMessage(HWND hWnd)
{
	audio_player_.DoExit();
	__super::OnFinalMessage(hWnd);
}

void AudioPlayerForm::InitWindow()
{
	SetIcon(IDI_ICON);
	SetTaskbarTitle(L"²¥·ÅÒôÆµ");

	start_time_ = (ui::Label*)FindControl(L"start_time");
	end_time_ = (ui::Label*)FindControl(L"end_time");
	input_vedio_ = (ui::Slider*)FindControl(L"input_vedio");
	btn_play_ = (ui::Button*)FindControl(L"btn_play");
	btn_pause_ = (ui::Button*)FindControl(L"btn_pause");

	m_pRoot->AttachBubbledEvent(kEventClick, nbase::Bind(&AudioPlayerForm::OnEventClick, this, std::placeholders::_1));

	input_vedio_->AttachButtonDown(nbase::Bind(&AudioPlayerForm::OnSliderButtonDown, this, std::placeholders::_1));
	input_vedio_->AttachValueChange(nbase::Bind(&AudioPlayerForm::OnSliderEventChange, this, std::placeholders::_1));

	auto st_tm_cb = ToWeakCallback([=](int tns){
		int thh, tmm, tss;
		thh = tns / 3600;
		tmm = (tns % 3600) / 60;
		tss = (tns % 60);
		std::wstring timelong = nbase::StringPrintf(L"%02d:%02d:%02d", thh, tmm, tss);
		start_time_->SetText(timelong);
	});

	auto end_tm_cb = ToWeakCallback([=](int tns){
		int thh, tmm, tss;
		thh = tns / 3600;
		tmm = (tns % 3600) / 60;
		tss = (tns % 60);
		std::wstring timelong = nbase::StringPrintf(L"%02d:%02d:%02d", thh, tmm, tss);
		end_time_->SetText(timelong);
	});

	auto pr_val_cb = ToWeakCallback([=](int val){
		if (!slider_change_ && slider_change_pos_ <= val)
		{
			input_vedio_->SetValue(val);
		}
	});

	audio_player_.SetStartTimeCb(st_tm_cb);
	audio_player_.SetEndTimeCb(end_tm_cb);
	audio_player_.SetProgressCb(pr_val_cb);
}

bool AudioPlayerForm::OnEventClick(ui::EventArgs* args)
{
	std::wstring name= args->pSender->GetName();

	if (name == L"btn_play")
	{
		if (is_first_)
		{
			is_first_ = false;
			OnPlay();
		}
		else
		{
			OnPause();
		}

		btn_play_->SetVisible(false);
		btn_pause_->SetVisible(true);

	}
	else if (name == L"btn_pause")
	{
		OnPause();
		btn_play_->SetVisible(true);
		btn_pause_->SetVisible(false);
	}

	return true;
}

void AudioPlayerForm::ShowAudioPlay(std::string path)
{
	AudioPlayerForm* window = (AudioPlayerForm*)nim_comp::WindowsManager::GetInstance()->GetWindow(AudioPlayerForm::kClassName, AudioPlayerForm::kClassName);

	if (!window)
	{
		window = new AudioPlayerForm;
		window->OnSetPlayUrl(path);
		window->Create(NULL, _T("ÒôÆµ²¥·Å"), UI_WNDSTYLE_DIALOG, 0, false);
		window->CenterWindow();
		window->ShowWindow();
	}
	else
	{
		window->ActiveWindow();
	}
}

void AudioPlayerForm::OnSetPlayUrl(std::string path)
{
	play_url_ = path;
}

void AudioPlayerForm::OnPause()
{
	audio_player_.Pause();
}

void AudioPlayerForm::OnPlay()
{
	audio_player_.Play(play_url_.c_str(), GetHWND());
}

bool AudioPlayerForm::OnSliderButtonDown(ui::EventArgs* arg)
{
	slider_change_ = true;
	return true;
}

bool AudioPlayerForm::OnSliderEventChange(ui::EventArgs* arg)
{
	std::wstring name = arg->pSender->GetName();
	if (name == L"input_vedio")
	{
		slider_change_pos_ = input_vedio_->GetValue();
		audio_player_.SeekPos(slider_change_pos_);
		slider_change_ = false;
	}
	return true;
}


