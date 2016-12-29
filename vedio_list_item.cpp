#include "vedio_list_item.h"

using namespace ui;

VedioListItem::VedioListItem()
{

}

VedioListItem::~VedioListItem()
{

}

void VedioListItem::initFromJson(Json::Value& j_value, int index, PlayItemCb cb)
{
	play_cb_ = cb;
	if (index == 0)
	{
		is_play_ = true;
	}
	else
	{
		is_play_ = false;
	}
	vedio_index_ = index;
	item_id_ = j_value["itemId"].asUInt64();
	course_id_ = j_value["courseId"].asUInt64();
	m3u8_ = j_value["m3u8"].asString();
	std::string url = j_value["url"].asString();
	std::string video_name_ = j_value["videoName"].asString();
	study_progress_ = j_value["studyProgress"].asDouble();
	course_name_ = (ui::Label*)FindSubControl(L"course_name");
	course_name_->SetText(nbase::UTF8ToUTF16(video_name_));

	this->AttachMouseEnter(nbase::Bind(&VedioListItem::OnEventSelected, this, std::placeholders::_1));
	this->AttachMouseLeave(nbase::Bind(&VedioListItem::OnEventUnSelected, this, std::placeholders::_1));
	this->AttachDoubleClick(nbase::Bind(&VedioListItem::OnDoubleClick, this, std::placeholders::_1));
}

bool VedioListItem::OnDoubleClick(ui::EventArgs* args)
{
	is_play_ = true;
	course_name_->SetState(kControlStatePushed);
	if (play_cb_)
	{
		play_cb_(vedio_index_);
	}
	return true;
}

void VedioListItem::OnSaveProgress(int current_ts, int end_ts, int num)
{
	if (study_progress_ >= 1.0)
	{
		record_id_ = 0;
	}

	if (record_id_ != 0)
	{
		UpdateVedioProgress(current_ts, end_ts, num);
	}
	else
	{
		AddVedioProgress(current_ts, end_ts, num);
	}
}

bool VedioListItem::OnEventSelected(ui::EventArgs* msg)
{
	if (!is_play_)
	{
		course_name_->SetState(kControlStatePushed);
	}
	return true;
}

bool VedioListItem::OnEventUnSelected(ui::EventArgs* msg)
{
	if (!is_play_)
	{
		course_name_->SetState(kControlStateNormal);
	}
	return true;
}

void VedioListItem::UpdateVedioProgress(int current_ts, int end_ts, int num)
{
	//ApiCaller api_caller;
	//SS_MAP param_map_;
	//
	//bool is_finish = (current_ts >= end_ts) ? true : false;
	//if (is_finish)
	//{
	//	study_progress_ = 1.0;
	//}
	//else
	//{
	//	study_progress_ = (double)current_ts / (double)end_ts;
	//}
	//current_ts = current_ts <= end_ts ? current_ts : end_ts;

	//param_map_["progress"] = nbase::StringPrintf("%d", current_ts);
	//std::string url = nbase::StringPrintf("%s%llu", API_UPDATE_STUDY_PROGRESS.c_str(), record_id_);
	//api_caller.AsyncCallPost(url, param_map_,
	//	ToWeakCallback([this](ApiResult &api_result){
	//	if (api_result.Success()){
	//		
	//	}
	//	else{
	//		std::string msg = api_result.GetMsg();
	//		std::string othermsg = api_result.GetOtherMsg();
	//		MsgToastForm::ShowMsgToast(nbase::UTF8ToUTF16(msg));
	//	}
	//}));
}

void VedioListItem::AddVedioProgress(int current_ts, int end_ts, int num)
{
	//ApiCaller api_caller;
	//SS_MAP param_map_;

	//bool is_finish = (current_ts >= end_ts) ? true : false;
	//if (is_finish)
	//{
	//	study_progress_ = 1.0;
	//}
	//else
	//{
	//	study_progress_ = (double)current_ts / (double)end_ts;
	//}
	//current_ts = current_ts <= end_ts ? current_ts : end_ts;
	//param_map_["progress"] = nbase::StringPrintf("%d", current_ts);
	//param_map_["courseId"] = nbase::StringPrintf("%llu", course_id_);
	//param_map_["itemId"] = nbase::StringPrintf("%llu", item_id_);
	//param_map_["vindex"] = nbase::StringPrintf("%d", vedio_index_);
	//param_map_["videoNum"] = nbase::StringPrintf("%d", num);//视频数量
	//param_map_["duration"] = nbase::StringPrintf("%d", end_ts);
	//param_map_["status"] = is_finish ? "1" : "0";//1表示播放结束

	//api_caller.AsyncCallPost(API_ADD_STUDY_PROGRESS, param_map_,
	//	ToWeakCallback([this, need_call_back](ApiResult &api_result){
	//	if (api_result.Success()){
	//		Json::Value val = api_result.GetData();
	//		record_id_ = val["recordId"].asUInt64();
	//	}
	//	else{
	//		std::string msg = api_result.GetMsg();
	//		std::string othermsg = api_result.GetOtherMsg();
	//		MsgToastForm::ShowMsgToast(nbase::UTF8ToUTF16(msg));

	//	}
	//}));
}
