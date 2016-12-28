#include "vedio_player_form.h"
#include "resource.h"

using namespace ui;
const LPCTSTR VedioPlayerForm::kClassName = L"VedioPlayWinForm";

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

	m_pRoot->AttachBubbledEvent(kEventClick, nbase::Bind(&VedioPlayerForm::OnEventClick, this, std::placeholders::_1));

	input_vedio_->AttachButtonDown(nbase::Bind(&VedioPlayerForm::OnSliderButtonDown, this, std::placeholders::_1));
	input_vedio_->AttachValueChange(nbase::Bind(&VedioPlayerForm::OnSliderEventChange, this, std::placeholders::_1));

	auto st_tm_cb = ToWeakCallback([=](std::wstring path){
		start_time_->SetText(path);
	});

	auto end_tm_cb = ToWeakCallback([=](std::wstring path){
		end_time_->SetText(path);
	});

	auto pr_val_cb = ToWeakCallback([=](int val){
		if (!slider_change_ && slider_change_pos_ <= val)
		{
			input_vedio_->SetValue(val);
		}
	});

	vedio_player_.SetStartTimeCb(st_tm_cb);
	vedio_player_.SetEndTimeCb(end_tm_cb);
	vedio_player_.SetProgressCb(pr_val_cb);
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
	HWND hwnd = CreateWin32(GetHWND());
	ui::UiRect margin_rect = vedio_show_->GetMargin();
	int width = vedio_show_->GetWidth();
	int height = vedio_show_->GetHeight();
	RECT rc;
	rc.left = margin_rect.left;
	rc.right = margin_rect.left + width;
	rc.top = margin_rect.top + 48;
	rc.bottom = margin_rect.top + height - 14;
	::SetWindowPos(hwnd, NULL, rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top, 0);

	vedio_player_.Play(play_url_.c_str(), hwnd);
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

