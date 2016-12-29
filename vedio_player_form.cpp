#include "vedio_player_form.h"
#include "resource.h"

using namespace ui;
const LPCTSTR VedioPlayerForm::kClassName = L"VedioPlayWinForm";

#define SLIDER_WIDTH_ADDITONAL   205
#define WIN_HEIGHT_ADDITONAL     113

VedioPlayerForm::VedioPlayerForm()
{

}

VedioPlayerForm::~VedioPlayerForm()
{
	
}

std::wstring VedioPlayerForm::GetSkinFolder()
{
	return L"video_player";
}

std::wstring VedioPlayerForm::GetSkinFile()
{
	return L"vedio_win.xml";
}

ui::UILIB_RESOURCETYPE VedioPlayerForm::GetResourceType() const
{
	return ui::UILIB_FILE;
}

std::wstring VedioPlayerForm::GetZIPFileName() const
{
	return L"VedioPlayWin.zip";
}

std::wstring VedioPlayerForm::GetWindowClassName() const
{
	return kClassName;
}

std::wstring VedioPlayerForm::GetWindowId() const
{
	return kClassName;
}

UINT VedioPlayerForm::GetClassStyle() const
{
	return (UI_CLASSSTYLE_FRAME | CS_DBLCLKS);
}

ui::Control* VedioPlayerForm::CreateControl(const std::wstring& pstrClass)
{
	return NULL;
}

void VedioPlayerForm::OnFinalMessage(HWND hWnd)
{
	__super::OnFinalMessage(hWnd);
}

void VedioPlayerForm::InitWindow()
{
	SetIcon(IDI_ICON);
	SetTaskbarTitle(L"播放视频");

	start_time_ = (ui::Label*)FindControl(L"start_time");
	end_time_ = (ui::Label*)FindControl(L"end_time");
	input_vedio_ = (ui::Slider*)FindControl(L"input_vedio");
	btn_play_ = (ui::Button*)FindControl(L"btn_play");
	btn_pause_ = (ui::Button*)FindControl(L"btn_pause");
	vedio_show_ = (ui::Control*)FindControl(L"vedio_show");
	item_list_ = (ui::ListBox*)FindControl(L"item_list");
	list_box_ = (ui::VBox*)FindControl(L"list_box");

	m_pRoot->AttachBubbledEvent(kEventClick, nbase::Bind(&VedioPlayerForm::OnEventClick, this, std::placeholders::_1));
	input_vedio_->AttachButtonDown(nbase::Bind(&VedioPlayerForm::OnSliderButtonDown, this, std::placeholders::_1));
	input_vedio_->AttachValueChange(nbase::Bind(&VedioPlayerForm::OnSliderEventChange, this, std::placeholders::_1));

	auto st_tm_cb = ToWeakCallback([=](int tns){
		current_ts_ = tns;
		int thh, tmm, tss;
		thh = tns / 3600;
		tmm = (tns % 3600) / 60;
		tss = (tns % 60);
		std::wstring timelong = nbase::StringPrintf(L"%02d:%02d:%02d", thh, tmm, tss);
		start_time_->SetText(timelong);
	});

	auto end_tm_cb = ToWeakCallback([=](int tns){
		end_ts_ = tns;
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
			if (val >=1000)
			{
				OnHandlePlayNext();
			}
		}
	});

	auto win_size_cb = ToWeakCallback([=](int width, int height){
		int win_height = height + WIN_HEIGHT_ADDITONAL;
		if (list_box_->IsVisible())
		{
			SetPos(UiRect(0, 0, width + list_box_->GetWidth(), win_height), SWP_NOMOVE);
		}
		else
		{
			SetPos(UiRect(0, 0, width, win_height), SWP_NOMOVE);
		}
		
		int vedio_with = width - SLIDER_WIDTH_ADDITONAL;
		input_vedio_->SetFixedWidth(vedio_with);
		std::wstring bk_url = L"file='slider_gray.png' dest='5,8," + nbase::StringPrintf(L"%d", vedio_with-5) + L",13'";
		input_vedio_->SetBkImage(bk_url);

		::SetWindowPos(hwnd_, NULL, 0, 48, width, height, 0);
	});

	vedio_player_.SetStartTimeCb(st_tm_cb);
	vedio_player_.SetEndTimeCb(end_tm_cb);
	vedio_player_.SetProgressCb(pr_val_cb);
	vedio_player_.SetWinSizeCb(win_size_cb);
}

bool VedioPlayerForm::OnEventClick(ui::EventArgs* args)
{
	std::wstring name = args->pSender->GetName();

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
	else if (name == L"btn_close")
	{
		vedio_player_.DoExit();
		OnDelayClose();
	}

	return true;
}

void VedioPlayerForm::ShowAudioPlay(std::string path)
{
	VedioPlayerForm* window = (VedioPlayerForm*)nim_comp::WindowsManager::GetInstance()->GetWindow(VedioPlayerForm::kClassName, VedioPlayerForm::kClassName);

	if (!window)
	{
		window = new VedioPlayerForm;
		window->OnSetPlayUrl(path);
		window->Create(NULL, _T("音频播放"), UI_WNDSTYLE_DIALOG, 0, false);
		window->CenterWindow();
		window->ShowWindow();
	}
	else
	{
		window->ActiveWindow();
	}
}

void VedioPlayerForm::OnSetPlayUrl(std::string path)
{
	play_url_ = path;
}

void VedioPlayerForm::OnPause()
{
	vedio_player_.Pause();
}

void VedioPlayerForm::OnPlay()
{
	hwnd_ = CreateWin32(GetHWND());

	::SetWindowPos(hwnd_, NULL, 0, 48, 10, 10, 0);
	
	vedio_player_.Play(play_url_.c_str(), hwnd_);
}

bool VedioPlayerForm::OnSliderButtonDown(ui::EventArgs* arg)
{
	slider_change_ = true;
	return true;
}

bool VedioPlayerForm::OnSliderEventChange(ui::EventArgs* arg)
{
	std::wstring name = arg->pSender->GetName();
	if (name == L"input_vedio")
	{
		slider_change_pos_ = input_vedio_->GetValue();
		vedio_player_.SeekPos(slider_change_pos_);
		slider_change_ = false;
	}
	return true;
}

LRESULT CALLBACK VedioPlayerForm::MyWindowProc(HWND hWnd, UINT nMsg, WPARAM wParam, LPARAM lParam)
{
	switch (nMsg)
	{
		case WM_QUIT:
			PostQuitMessage(0);
			break;
	}
	return DefWindowProc(hWnd, nMsg, wParam, lParam);
}

HWND VedioPlayerForm::CreateWin32(HWND parent_hwnd)
{
	HINSTANCE hInstance;
	hInstance = GetModuleHandle(NULL);
	WNDCLASS Draw;
	Draw.cbClsExtra = 0;
	Draw.cbWndExtra = 0;
	Draw.hCursor = LoadCursor(hInstance, IDC_ARROW);;
	Draw.hIcon = LoadIcon(hInstance, IDI_APPLICATION);;
	Draw.lpszMenuName = NULL;
	Draw.style = CS_HREDRAW | CS_VREDRAW;
	Draw.hbrBackground = (HBRUSH)COLOR_WINDOW;
	Draw.lpfnWndProc = MyWindowProc;
	Draw.lpszClassName = _T("DDraw");
	Draw.hInstance = hInstance;

	RegisterClass(&Draw);

	HWND hwnd = CreateWindow(
		_T("#32770"),           //上面注册的类名，要完全一致    
		L"play",  //窗口标题文字    
		WS_VISIBLE | WS_CHILD, //窗口外观样式    
		0,             //窗口相对于父级的X坐标    
		0,             //窗口相对于父级的Y坐标    
		0,                //窗口的宽度    
		0,                //窗口的高度    
		parent_hwnd,        //没有父窗口，为NULL    
		NULL,               //没有菜单，为NULL    
		hInstance,          //当前应用程序的实例句柄    
		NULL);              //没有附加数据，为NULL   

	return hwnd;
}

void VedioPlayerForm::OnDelayClose()
{
	StdClosure task = nbase::Bind(&VedioPlayerForm::OnClose, this);
	nbase::ThreadManager::PostDelayedTask(kThreadUI, task, nbase::TimeDelta::FromMilliseconds(300));
}

void VedioPlayerForm::OnClose()
{
	this->Close();
}

void VedioPlayerForm::OnHandlePlayNext()
{
	++vedio_list_index_;
	if (item_list_->GetCount()>vedio_list_index_)
	{
		vedio_player_.DoExit();
		DoStateCB();

		OnDelayPlayNext();
	}
}

void VedioPlayerForm::OnSchedualStateCB()
{
	vedio_state_timer_.Cancel();
	StdClosure task = nbase::Bind(&VedioPlayerForm::DoStateCB, this);
	auto cb = vedio_state_timer_.ToWeakCallback(task);
	nbase::ThreadManager::PostRepeatedTask(kThreadUI, cb, nbase::TimeDelta::FromSeconds(10));
}

void VedioPlayerForm::DoStateCB()
{
	if (vedio_state_call_back_)
	{
		vedio_state_call_back_();
	}

	//保存当前播放item的进度

}

void VedioPlayerForm::OnDelayPlayNext()
{
	StdClosure task = nbase::Bind(&VedioPlayerForm::OnPlayNext, this);
	nbase::ThreadManager::PostDelayedTask(kThreadUI, task, nbase::TimeDelta::FromMilliseconds(300));
}

void VedioPlayerForm::OnPlayNext()
{

}

void VedioPlayerForm::OnHandleItemPlay()
{

}

