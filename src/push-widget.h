#include "pch.h"

class PushWidget : virtual public QWidget {
public:
    virtual ~PushWidget() {}
    virtual bool ShowEditDlg() = 0;
    virtual void StartStreaming() = 0;
    virtual void StopStreaming() = 0;
    virtual void OnOBSEvent(obs_frontend_event ev) = 0;
    virtual QPushButton* GetDeleteButton() = 0;
    // The output is active (starting counts as running too, once the output exists).
    virtual bool IsRunning() = 0;
    // The target's config object was changed in place (the vendor API): redraw what the widget shows of it.
    virtual void RefreshConfig() = 0;
};

PushWidget* createPushWidget(const std::string& targetId, QWidget* parent = 0);
