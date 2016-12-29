#pragma once

using PlayItemCb = std::function<void(int)>;

class VedioListItem : public ui::ListContainerElement
{
public:
	VedioListItem();
	virtual ~VedioListItem();

	void initFromJson(Json::Value& j_value, int index, PlayItemCb cb);
	void OnSaveProgress(int current_ts, int end_ts, int num);
private:
	void UpdateVedioProgress(int current_ts,int end_ts,int num);
	void AddVedioProgress(int current_ts, int end_ts, int num);
	bool OnEventSelected(ui::EventArgs* msg);
	bool OnEventUnSelected(ui::EventArgs* msg);
	bool OnDoubleClick(ui::EventArgs* args);

private:
	bool is_play_ = false;
	int vedio_index_;
	uint64_t item_id_;
	ui::Label* course_name_;
	std::string m3u8_;
	double study_progress_;
	uint64_t record_id_;
	uint64_t course_id_;
	PlayItemCb play_cb_;
};