#include "pch.h"

class PushWidget : virtual public QWidget {
public:
    virtual ~PushWidget() {}
    virtual bool ShowEditDlg() = 0;
    virtual void StartStreaming() = 0;
    virtual void StopStreaming() = 0;
    virtual void OnOBSEvent(obs_frontend_event ev) = 0;
    virtual QPushButton* GetDeleteButton() = 0;
    // The output is active: data capture began.
    virtual bool IsRunning() = 0;
    // The output is active or a start is in flight (the connect phase, where
    // obs_output_active is still false): the config must not change under it.
    virtual bool IsBusy() = 0;
    // The edit dialog is open: it edits a private copy of the config and writes that copy
    // back on accept, so a change made in place meanwhile would be overwritten.
    virtual bool IsEditing() = 0;
    // The target's config object was changed in place (the vendor API): redraw what the widget shows of it.
    virtual void RefreshConfig() = 0;
};

PushWidget* createPushWidget(const std::string& targetId, QWidget* parent = 0);
