#include "pch.h"

#include <list>
#include <regex>
#include <filesystem>
#include <unordered_map>
#include <atomic>
#include <vector>
#include <cassert>
#include <cstring>

#include <QCryptographicHash>

#include "push-widget.h"
#include "plugin-support.h"
#include "helpers.h"

#include "output-config.h"
#include "obs-websocket-api.h"

#ifdef _WIN32
#include <Windows.h>
#endif

#define ConfigSection "obs-multi-rtmp"

static class GlobalServiceImpl : public GlobalService
{
public:
    bool RunInUIThread(std::function<void()> task) override {
        if (uiThread_ == nullptr)
            return false;
        QMetaObject::invokeMethod(uiThread_, [func = std::move(task)]() {
            func();
        });
        return true;
    }

    QThread* uiThread_ = nullptr;
} s_service;


GlobalService& GetGlobalService() {
    return s_service;
}


class OutputsListWidget : public QListWidget
{
public:
    using QListWidget::QListWidget;

    QSize sizeHint() const override
    {
        QSize hint = QListWidget::sizeHint();
        hint.setHeight(ContentHeight());
        return hint;
    }

    QSize minimumSizeHint() const override
    {
        return sizeHint();
    }

protected:
    bool event(QEvent *event) override
    {
        const bool handled = QListWidget::event(event);

        switch (event->type()) {
        case QEvent::FontChange:
        case QEvent::LayoutRequest:
        case QEvent::PolishRequest:
        case QEvent::Show:
        case QEvent::StyleChange:
            RefreshItemSizeHints();
            updateGeometry();
            break;
        default:
            break;
        }

        return handled;
    }

private:
    void RefreshItemSizeHints()
    {
        for (int i = 0; i < count(); ++i) {
            auto item = this->item(i);
            auto widget = itemWidget(item);
            if (item && widget)
                item->setSizeHint(widget->sizeHint());
        }
    }

    int ContentHeight() const
    {
        auto *widget = const_cast<OutputsListWidget *>(this);
        widget->doItemsLayout();

        int totalHeight = frameWidth() * 2;
        const int itemCount = count();
        for (int i = 0; i < itemCount; ++i) {
            totalHeight += sizeHintForRow(i);
        }

        if (itemCount > 1) {
            totalHeight += (itemCount - 1) * spacing();
        }

        return (std::max)(totalHeight, frameWidth() * 2);
    }
};


class MultiOutputWidget : public QWidget
{
public:
    MultiOutputWidget(QWidget* parent = 0)
        : QWidget(parent)
    {
        setWindowTitle(obs_module_text("Title"));

        container_ = new QWidget(&scroll_);
        layout_ = new QVBoxLayout(container_);
        layout_->setAlignment(Qt::AlignmentFlag::AlignTop);
        layout_->setSizeConstraint(QLayout::SetMinAndMaxSize);

        // init widget
        auto addButton = new QPushButton(obs_module_text("Btn.NewTarget"), container_);
        QObject::connect(addButton, &QPushButton::clicked, [this]() {
            auto& global = GlobalMultiOutputConfig();
            auto newId = GenerateId(global);
            auto target = std::make_shared<OutputTargetConfig>();
            target->id = newId;
            global.targets.emplace_back(target);
            auto pushWidget = AddPushWidget(newId);
            if (pushWidget->ShowEditDlg()) {
                SaveConfig();
            } else {
                DeletePushWidget(newId);
            }
        });
        layout_->addWidget(addButton);

        // start all, stop all
        auto allBtnContainer = new QWidget(this);
        auto allBtnLayout = new QHBoxLayout();
        auto startAllButton = new QPushButton(obs_module_text("Btn.StartAll"), allBtnContainer);
        allBtnLayout->addWidget(startAllButton);
        auto stopAllButton = new QPushButton(obs_module_text("Btn.StopAll"), allBtnContainer);
        allBtnLayout->addWidget(stopAllButton);
        allBtnContainer->setLayout(allBtnLayout);
        layout_->addWidget(allBtnContainer);

        QObject::connect(startAllButton, &QPushButton::clicked, [this]() {
            for (auto x : GetAllPushWidgets())
                x->StartStreaming();
        });
        QObject::connect(stopAllButton, &QPushButton::clicked, [this]() {
            for (auto x : GetAllPushWidgets())
                x->StopStreaming();
        });
 
        // load and show outputs
        outputsContainer_ = new OutputsListWidget(container_);
        outputsContainer_->setDragDropMode(QAbstractItemView::InternalMove);
        outputsContainer_->setSelectionMode(QAbstractItemView::SingleSelection);
        outputsContainer_->setDropIndicatorShown(true);
        outputsContainer_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Maximum);
        outputsContainer_->setSizeAdjustPolicy(QAbstractScrollArea::AdjustToContents);
        outputsContainer_->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        outputsContainer_->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
        outputsContainer_->setStyleSheet(
            "QListWidget {"
            "   padding: 0px;"
            "   margin: 0px;"
            "   border: none;"
            "   background: transparent;"
            "}"
            "QListWidget::item:selected, QListWidget::item:active {"
            "   border: none;"
            "   background: transparent;"
            "}"
            "QListWidget::item:hover, QListWidget::item:hover:selected, QListWidget::item:hover:active {"
            "   border: none;"
            "   background: rgba(127, 127, 127, 0.1);"
            "   cursor: move;"
            "}"
        );
        LoadConfig();
        connect(
            outputsContainer_->model(),
            &QAbstractItemModel::rowsMoved,
            this,
            &MultiOutputWidget::OnOutputMoved
        );
        layout_->addWidget(outputsContainer_);

        // donate
        if (std::string("\xe5\xa4\x9a\xe8\xb7\xaf\xe6\x8e\xa8\xe6\xb5\x81") == obs_module_text("Title"))
        {
            auto cr = new QWidget(container_);
            auto innerLayout = new QGridLayout(cr);
            innerLayout->setAlignment(Qt::AlignmentFlag::AlignLeft);

            auto label = new QLabel(u8"该插件免费提供，\r\n如您是付费取得，可向商家申请退款\r\n免费领红包或投喂支持插件作者。", cr);
            innerLayout->addWidget(label, 0, 0, 1, 2);
            innerLayout->setColumnStretch(0, 4);
            auto label2 = new QLabel(u8"作者：雷鸣", cr);
            innerLayout->addWidget(label2, 1, 0, 1, 1);
            auto btnFeed = new QPushButton(u8"支持", cr);
            innerLayout->addWidget(btnFeed, 1, 1, 1, 1);
            
            QObject::connect(btnFeed, &QPushButton::clicked, [this]() {
                const char redbagpng[] = 
                    "iVBORw0KGgoAAAANSUhEUgAAAJgAAACXAQMAAADTWgC3AAAABlBMVEUAAAD///+l2Z/dAAAAAWJLR0Q"
                    "AiAUdSAAAAAlwSFlzAAAuIwAALiMBeKU/dgAAAWtJREFUSMe1lk2OgzAMhY1YZJkj5CbkYkggcTG4SY"
                    "6QZRaonmcHqs7PYtTaVVWSLxJu7JfnEP/+0H9ZIaKRA0aZz4QJJXuGQFsJO9HU104H1ihuTENl4IS12"
                    "YmVcFSa4unJuE2xZV69mOav5XrX6nRgMi6Ii+3Nr9p4k8m7w5OtmOVbzw8ZrOmbxs0Y/ktENMlfnQnx"
                    "NweG2vB1ZrZCPoyPfmbwXWSPiz1DvIrHwOHgFQsQtTkrmG6McRvqUu4aGbM28Cm1wRM62HtOP2DwkFF"
                    "ypKVoU/dEa8Y9rtaFJLg5EzscoSfBKMWgZ8aY9bj4EQ1jo9GDIR68kKukMCF/6pPWTPW0R9XulVNzpj"
                    "6Z5ZzMpOZrzvRElPC49Awx2LOi3k7aP+akhnL1AEMmPYphvtqeGD032TPt5zB2kQBq5Mgo9hrl7lceT"
                    "MQsEkD80YH1O9xRw9Vzn/cSQ6Y6EK1JH3nVxvss/GCf3L3/YF97Nxv6vuoIAwAAAABJRU5ErkJggg=="
                    ;
                const char alipaypng[] = 
                    "iVBORw0KGgoAAAANSUhEUgAAALsAAAC4AQMAAACByg+HAAAABlBMVEUAAAD///+l2Z/dAAAAAWJLR0Q"
                    "AiAUdSAAAAAlwSFlzAAAuIwAALiMBeKU/dgAAAVtJREFUWMPNmEGugzAMRM0qx+CmIbkpx8gK1zM2/U"
                    "L66w4RogqvC5dhxqbm/6/TfgEuwzr8PNwnjhP7pgYNxR1r97XPhUvxjUsPztj0hkJ7O7c4m/V3gCg36"
                    "r5Q68uA7SHtaC8BlBbP2T4awFNzEaANOtSt4+EPEciEiKJn2eCZJSIQ5XbU5yFt+HNUfIgBNqFo6Grh"
                    "BDxttIQcoL5ICrsTbdAYWgBPIltxK7uVRaeLQToTFRMbb+JoYoAHjsG63UWXtFIQm2w/mV9x9vodUnC"
                    "vCWfuiA+oqwaLH7Al8qL/aS4GA9I6zkz/bbFBSsHVKi++Dftg89YC53DDq8iLTJCSVgcq6DlMZGRkzm"
                    "pBtW2jRRH3flU3UIIacWBLduv0gxzwltGTaUtOFS4HVSJHnBp35jvAsbJnV/RbewWgtLVyAlODkjZSd"
                    "eMrkNfsIwX3awYfuLLEaGKg/OvlAz+wXVruSNSgAAAAAElFTkSuQmCC";
                const char wechatpng[] = 
                    "iVBORw0KGgoAAAANSUhEUgAAAK8AAACtAQMAAAD8lL09AAAABlBMVEUAAAD///+l2Z/dAAAAAWJLR0Q"
                    "AiAUdSAAAAAlwSFlzAAAuIwAALiMBeKU/dgAAAbpJREFUSMe9l0GugzAMRI26yDJHyE3Si1WiEhcrN8"
                    "kRWLJA8Z9xaNUv/eUfUIXgqQtn4pkY87+ubv+Cd8O1T3g2y4unzveqxXf3BniUeLKa3XcxrnZr+32zg"
                    "iXPDvy0S/C04WYo4jpsKCK97FH2K3DovfqzoJzAX9sgwtFVNS/tvH03mwYPs6zb7PjDrf22lAZT6ugq"
                    "wwtaCzLYWLwO13yUUQl3N70yDSTFWOqCJbORsdloLYguxrSNgxyWhl3Z0l2LjWaZUE541qaRFUqM342"
                    "5sDTYdY5EZE1KjHU/z25+0UV3yi/GE4LeubHe0V9QYFZjEOhN70QOmp0JocSdGT82Nh+D7nrcQoHkDO"
                    "GOcmLnhXil3vAsy5nRWhS9SjGkhmMiddHIMO6580oMu5a0xpAC0aOSOHd0mC+jodjNnhyVqDH1Tj3Ts"
                    "0hEm3CGv7dYhDkdQGr0cOJxxquMM02GI44g+tKiqwwZVd4HugjHfOKxeEYvQ9jVOKawzrRnQkQSf4Yz"
                    "EeasiWHhwfYNF8WkIscc985pKCaGSzA9S6eGAmpMvRlFzonBLZ6qFp8nyhxSM/IP+3x2arDwc/kHxnM"
                    "tm62qBBUAAAAASUVORK5CYII=";
                auto donateWnd = new QDialog();
                donateWnd->setWindowTitle(u8"赞助");
                QTabWidget* tab = new QTabWidget(donateWnd);
                auto redbagQr = new QLabel(donateWnd);
                auto redbagQrBmp = QPixmap::fromImage(QImage::fromData(QByteArray::fromBase64(QByteArray::fromRawData(redbagpng, sizeof(redbagpng) - 1)), "png"));
                redbagQr->setPixmap(redbagQrBmp);
                tab->addTab(redbagQr, u8"支付宝领红包");
                auto aliQr = new QLabel(donateWnd);
                auto aliQrBmp = QPixmap::fromImage(QImage::fromData(QByteArray::fromBase64(QByteArray::fromRawData(alipaypng, sizeof(alipaypng) - 1)), "png"));
                aliQr->setPixmap(aliQrBmp);
                tab->addTab(aliQr, u8"支付宝打赏");
                auto weQr = new QLabel(donateWnd);
                auto weQrBmp = QPixmap::fromImage(QImage::fromData(QByteArray::fromBase64(QByteArray::fromRawData(wechatpng, sizeof(wechatpng) - 1)), "png"));
                weQr->setPixmap(weQrBmp);
                tab->addTab(weQr, u8"微信打赏");

                auto layout = new QGridLayout();
                layout->setRowStretch(0, 1);
                layout->setColumnStretch(0, 1);
                layout->addWidget(new QLabel(u8"打赏并非购买，不提供退款。", donateWnd), 0, 0);
                layout->addWidget(tab, 1, 0);
                donateWnd->setLayout(layout);
                donateWnd->setMinimumWidth(360);
                donateWnd->exec();
            });

            layout_->addWidget(cr);
        }
        else
        {
            auto label = new QLabel(u8"<p>This plugin is provided for free. <br>Author: SoraYuki (<a href=\"https://paypal.me/sorayuki0\">donate</a>) </p>", container_);
            label->setTextFormat(Qt::RichText);
            label->setTextInteractionFlags(Qt::TextBrowserInteraction);
            label->setOpenExternalLinks(true);
            layout_->addWidget(label);
        }

        scroll_.setWidgetResizable(true);
        scroll_.setWidget(container_);

        auto fullLayout = new QGridLayout(this);
        fullLayout->setContentsMargins(0, 0, 0, 0);
        fullLayout->setRowStretch(0, 1);
        fullLayout->setColumnStretch(0, 1);
        fullLayout->addWidget(&scroll_, 0, 0);
    }

    std::list<PushWidget*> GetAllPushWidgets()
    {
        std::list<PushWidget*> result;
        for (int row = 0; row < outputsContainer_->count(); ++row) {
            auto item = outputsContainer_->item(row);
            if (!item) {
                continue;
            }

            auto widget = outputsContainer_->itemWidget(item);
            auto pushWidget = dynamic_cast<PushWidget*>(widget);
            if (pushWidget) {
                result.push_back(pushWidget);
            }
        }
        return result;
    }

    bool SaveConfig()
    {
        return SaveMultiOutputConfig();
    }

    // ---- vendor API support (see the vendor section below the class) ----

    PushWidget* FindPushWidget(const std::string& targetId)
    {
        const QString id = QString::fromStdString(targetId);
        for (int row = 0; row < outputsContainer_->count(); ++row) {
            auto item = outputsContainer_->item(row);
            if (!item || item->data(Qt::UserRole).toString() != id)
                continue;
            return dynamic_cast<PushWidget*>(outputsContainer_->itemWidget(item));
        }
        return nullptr;
    }

    // Active or starting: in both states the output was built from the target's config.
    bool IsTargetRunning(const std::string& targetId)
    {
        auto widget = FindPushWidget(targetId);
        return widget != nullptr && widget->IsBusy();
    }

    struct TargetState {
        OutputTargetConfigPtr config;
        bool running;
    };

    std::vector<TargetState> ViewTargets()
    {
        std::vector<TargetState> result;
        for (auto& target : GlobalMultiOutputConfig().targets) {
            if (target)
                result.push_back({ target, IsTargetRunning(target->id) });
        }
        return result;
    }

    struct UpsertResult {
        bool applied = false;
        bool created = false;
        bool saved = false;
        std::string error;
        OutputTargetConfigPtr target;
    };

    // Writes the target in place (the push widget keeps the same shared object), or appends it.
    // Never rebuilds the dock: a full LoadConfig() destroys every widget and force-stops any
    // other target that is streaming. A target whose own output is running or starting is
    // refused: the output already carries the old server and key.
    UpsertResult UpsertTarget(const OutputTargetConfig& wanted)
    {
        UpsertResult result;
        auto& global = GlobalMultiOutputConfig();
        auto existing = FindById(global.targets, wanted.id);
        if (existing && IsTargetRunning(wanted.id)) {
            result.error = "target running";
            return result;
        }
        if (!existing) {
            existing = std::make_shared<OutputTargetConfig>();
            existing->id = wanted.id;
            global.targets.emplace_back(existing);
            result.created = true;
        }
        existing->name = wanted.name;
        existing->protocol = wanted.protocol;
        existing->serviceParam = wanted.serviceParam;
        existing->outputParam = wanted.outputParam;
        existing->syncStart = wanted.syncStart;
        existing->syncStop = wanted.syncStop;
        // Both explicitly absent: "Reuse the streaming encoder as OBS", rendering the program scene.
        existing->videoConfig = std::nullopt;
        existing->audioConfig = std::nullopt;
        result.applied = true;
        result.target = existing;
        if (result.created)
            AddPushWidget(existing->id);
        else if (auto widget = FindPushWidget(existing->id))
            widget->RefreshConfig();
        result.saved = SaveConfig();
        return result;
    }

    void OnOutputMoved(
        const QModelIndex &parent,
        int start,
        int end,
        const QModelIndex &destination,
        int row
    )
    {
        // QListWidget uses a single root parent for internal move operations.
        if (parent != destination) {
            return;
        }

        const int count = outputsContainer_->count();
        if (count <= 0 || start < 0 || end < start || row < 0 || row > count) {
            return;
        }

        auto &targets = GlobalMultiOutputConfig().targets;
        std::unordered_map<std::string, OutputTargetConfigPtr> targetById;
        targetById.reserve(targets.size());
        for (auto &target : targets) {
            if (target) {
                targetById.emplace(target->id, target);
            }
        }

        std::remove_reference_t<decltype(targets)> reordered;
        for (int i = 0; i < count; ++i) {
            auto item = outputsContainer_->item(i);
            if (!item) {
                continue;
            }

            auto id = item->data(Qt::UserRole).toString().toStdString();
            auto it = targetById.find(id);
            if (it == targetById.end()) {
                continue;
            }

            reordered.emplace_back(it->second);
            targetById.erase(it);
        }

        // Keep unmatched items in their previous order to avoid accidental loss.
        if (!targetById.empty()) {
            for (auto &target : targets) {
                if (!target) {
                    continue;
                }
                auto it = targetById.find(target->id);
                if (it == targetById.end()) {
                    continue;
                }
                reordered.emplace_back(it->second);
                targetById.erase(it);
            }
        }

        targets.swap(reordered);

        SaveConfig();
        outputsContainer_->clearSelection();
    }

    void LoadConfig()
    {
        outputsContainer_->clear();

        GlobalMultiOutputConfig() = {};
        if (!LoadMultiOutputConfig()) {
            return;
        }

        for(auto x: GlobalMultiOutputConfig().targets)
        {
            AddPushWidget(x->id);
        }
    }

private:
    // Main widget of this module's dock
    QWidget* container_ = 0;
    // The layout of the root widget
    QVBoxLayout* layout_ = 0;
    // Scrollable area in case of overflows of content
    QScrollArea scroll_;
    // Widget, that contains output source widgets
    QListWidget* outputsContainer_ = 0;

    void DeletePushWidget(const std::string& targetId)
    {
        // Delete from model
        auto outputTargets = &(GlobalMultiOutputConfig().targets);
        auto currentTarget = std::find_if(outputTargets->begin(), outputTargets->end(), [&targetId](auto& x) { return x->id == targetId; });
        if (currentTarget == outputTargets->end()) {
            return;
        }
        outputTargets->erase(currentTarget);

        // Delete from List View
        const QString id = QString::fromStdString(targetId);
        for(auto listItem: outputsContainer_->findItems("", Qt::MatchContains)) {
            if (listItem->data(Qt::UserRole).toString() != id) {
                continue;
            }
            int row = outputsContainer_->row(listItem);
            auto removedItem = outputsContainer_->takeItem(row);
            auto pushWidget = outputsContainer_->itemWidget(removedItem);
            delete removedItem;
            if (pushWidget) {
                pushWidget->deleteLater();
            }
        }
    }

    PushWidget* AddPushWidget(const std::string& targetId)
    {
        auto pushWidget = createPushWidget(targetId, outputsContainer_->viewport());

        QListWidgetItem* listItem = new QListWidgetItem();
        listItem->setData(Qt::UserRole, QString::fromStdString(targetId));
        listItem->setSizeHint(pushWidget->sizeHint());
        outputsContainer_->addItem(listItem);
        outputsContainer_->setItemWidget(listItem, pushWidget);

        QObject::connect(pushWidget->GetDeleteButton(), &QPushButton::clicked, [this, targetId]() {
            auto msgbox = new QMessageBox(
                QMessageBox::Icon::Question,
                obs_module_text("Question.Title"),
                obs_module_text("Question.Delete"),
                QMessageBox::Yes | QMessageBox::No,
                this
            );
            if (msgbox->exec() != QMessageBox::Yes) {
                return;
            }
            DeletePushWidget(targetId);
            SaveConfig();
        });

        return pushWidget;
    }
};

// ======================= obs-websocket vendor API =======================
//
// Vendor "obs-multi-rtmp" with two requests, so an obs-websocket client (the
// Polemica Companion's YouTube setup) can read the targets and write one in place:
//   get_targets                       -> { success: true, targets: [TargetView] }
//   set_target {id, name, server, key, sync_start, sync_stop}
//                                     -> { success: true, created, target }
//                                        { success: false, error: "config not saved", applied: true, target }
//                                        { success: false, error: "target running" | "missing field: <name>" | "shutting down" }
// TargetView: id, name, protocol, server, key_sha256 (the key never leaves OBS in
// clear), sync_start, sync_stop, running, video_encoder, audio_encoder
// ("stream" | "recording" | "custom"), output_scene (a custom video config's scene).
// An unknown vendor or request is answered by obs-websocket itself (ResourceNotFound, 600).
//
// Handlers run on obs-websocket's thread and hop to the UI thread through a
// module-lifetime dispatcher object with a blocking call; the dock is touched
// only there, and after OBS_FRONTEND_EVENT_EXIT every request is refused.

static MultiOutputWidget* s_dock = nullptr;
static QObject* s_dispatcher = nullptr;
static std::atomic<bool> s_shuttingDown{ false };
static obs_websocket_vendor s_vendor = nullptr;

static void Refuse(obs_data_t* response, const std::string& error)
{
    obs_data_set_bool(response, "success", false);
    obs_data_set_string(response, "error", error.c_str());
}

static std::string EncoderKind(const std::optional<std::string>& configId)
{
    if (!configId.has_value() || configId->empty() || *configId == OBS_STREAMING_ENC_PLACEHOLDER)
        return "stream";
    if (*configId == OBS_RECORDING_ENC_PLACEHOLDER)
        return "recording";
    return "custom";
}

static std::string JsonString(const nlohmann::json& object, const char* key)
{
    if (!object.is_object())
        return "";
    auto it = object.find(key);
    if (it == object.end() || !it->is_string())
        return "";
    return it->get<std::string>();
}

static obs_data_t* TargetToData(const OutputTargetConfig& config, bool running)
{
    auto data = obs_data_create();
    obs_data_set_string(data, "id", config.id.c_str());
    obs_data_set_string(data, "name", config.name.c_str());
    obs_data_set_string(data, "protocol", config.protocol.c_str());
    obs_data_set_string(data, "server", JsonString(config.serviceParam, "server").c_str());
    auto key = JsonString(config.serviceParam, "key");
    std::string keyHash;
    if (!key.empty())
        keyHash = QCryptographicHash::hash(QByteArray::fromStdString(key), QCryptographicHash::Sha256).toHex().toStdString();
    obs_data_set_string(data, "key_sha256", keyHash.c_str());
    obs_data_set_bool(data, "sync_start", config.syncStart);
    obs_data_set_bool(data, "sync_stop", config.syncStop);
    obs_data_set_bool(data, "running", running);
    auto videoKind = EncoderKind(config.videoConfig);
    obs_data_set_string(data, "video_encoder", videoKind.c_str());
    obs_data_set_string(data, "audio_encoder", EncoderKind(config.audioConfig).c_str());
    std::string scene;
    if (videoKind == "custom") {
        auto videoConfig = FindById(GlobalMultiOutputConfig().videoConfig, *config.videoConfig);
        if (videoConfig && videoConfig->outputScene.has_value())
            scene = *videoConfig->outputScene;
    }
    obs_data_set_string(data, "output_scene", scene.c_str());
    return data;
}

// Runs task on the UI thread and waits for it; false once the module is shutting down.
static bool RunOnUiThread(std::function<void()> task)
{
    if (s_shuttingDown || s_dispatcher == nullptr)
        return false;
    if (QThread::currentThread() == s_dispatcher->thread()) {
        task();
        return true;
    }
    QMetaObject::invokeMethod(s_dispatcher, [task = std::move(task)]() { task(); }, Qt::BlockingQueuedConnection);
    return true;
}

static void OnGetTargets(obs_data_t*, obs_data_t* response, void*)
{
    bool ran = RunOnUiThread([response]() {
        if (s_shuttingDown || s_dock == nullptr) {
            Refuse(response, "shutting down");
            return;
        }
        auto targets = obs_data_array_create();
        for (auto& state : s_dock->ViewTargets()) {
            auto item = TargetToData(*state.config, state.running);
            obs_data_array_push_back(targets, item);
            obs_data_release(item);
        }
        obs_data_set_bool(response, "success", true);
        obs_data_set_array(response, "targets", targets);
        obs_data_array_release(targets);
    });
    if (!ran)
        Refuse(response, "shutting down");
}

static bool RequireField(obs_data_t* request, obs_data_t* response, const char* field, bool nonEmptyString)
{
    bool present = obs_data_has_user_value(request, field);
    if (present && nonEmptyString) {
        const char* value = obs_data_get_string(request, field);
        present = value != nullptr && *value != 0;
    }
    if (!present)
        Refuse(response, std::string("missing field: ") + field);
    return present;
}

static void OnSetTarget(obs_data_t* request, obs_data_t* response, void*)
{
    for (auto field : { "id", "server", "key" }) {
        if (!RequireField(request, response, field, true))
            return;
    }
    for (auto field : { "name", "sync_start", "sync_stop" }) {
        if (!RequireField(request, response, field, false))
            return;
    }
    OutputTargetConfig wanted;
    wanted.id = obs_data_get_string(request, "id");
    wanted.name = obs_data_get_string(request, "name");
    wanted.protocol = "RTMP";
    wanted.serviceParam = nlohmann::json{
        { "server", obs_data_get_string(request, "server") },
        { "key", obs_data_get_string(request, "key") },
        { "use_auth", false },
    };
    wanted.outputParam = nlohmann::json::object();
    wanted.syncStart = obs_data_get_bool(request, "sync_start");
    wanted.syncStop = obs_data_get_bool(request, "sync_stop");

    bool ran = RunOnUiThread([&wanted, response]() {
        if (s_shuttingDown || s_dock == nullptr) {
            Refuse(response, "shutting down");
            return;
        }
        auto result = s_dock->UpsertTarget(wanted);
        if (!result.applied) {
            Refuse(response, result.error);
            return;
        }
        auto target = TargetToData(*result.target, s_dock->IsTargetRunning(result.target->id));
        obs_data_set_obj(response, "target", target);
        obs_data_release(target);
        obs_data_set_bool(response, "created", result.created);
        if (result.saved) {
            obs_data_set_bool(response, "success", true);
        } else {
            // The dock shows the change and the next start uses it, but a restart loses it.
            Refuse(response, "config not saved");
            obs_data_set_bool(response, "applied", true);
        }
    });
    if (!ran)
        Refuse(response, "shutting down");
}

static void RegisterVendorApi()
{
    s_vendor = obs_websocket_register_vendor("obs-multi-rtmp");
    if (s_vendor == nullptr) {
        blog(LOG_INFO, TAG "obs-websocket not available: vendor API not registered");
        return;
    }
    obs_websocket_vendor_register_request(s_vendor, "get_targets", OnGetTargets, nullptr);
    obs_websocket_vendor_register_request(s_vendor, "set_target", OnSetTarget, nullptr);
    blog(LOG_INFO, TAG "vendor API registered: get_targets, set_target");
}

static void UnregisterVendorApi()
{
    if (s_vendor == nullptr)
        return;
    obs_websocket_vendor_unregister_request(s_vendor, "get_targets");
    obs_websocket_vendor_unregister_request(s_vendor, "set_target");
    s_vendor = nullptr;
}

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("obs-multi-rtmp", "en-US")
OBS_MODULE_AUTHOR("雷鳴 (@sorayukinoyume)")

bool obs_module_load()
{
    auto mainwin = (QMainWindow*)obs_frontend_get_main_window();
    if (mainwin == nullptr)
        return false;
    QMetaObject::invokeMethod(mainwin, []() {
        s_service.uiThread_ = QThread::currentThread();
    });

    auto dock = new MultiOutputWidget();
    dock->setObjectName("obs-multi-rtmp-dock");
    if (!obs_frontend_add_dock_by_id("obs-multi-rtmp-dock", obs_module_text("Title"), dock))
    {
        delete dock;
        return false;
    }
    // Module load runs on the UI thread (the dock above is built here): the dispatcher
    // the vendor handlers hop through lives on it for the module's lifetime.
    s_dock = dock;
    s_dispatcher = new QObject();

    blog(LOG_INFO, TAG "version: %s by SoraYuki https://github.com/sorayuki/obs-multi-rtmp/", PLUGIN_VERSION);

    obs_frontend_add_event_callback(
        [](enum obs_frontend_event event, void *private_data) {
            auto dock = static_cast<MultiOutputWidget*>(private_data);

            for(auto x: dock->GetAllPushWidgets())
                x->OnOBSEvent(event);

            if (event == obs_frontend_event::OBS_FRONTEND_EVENT_EXIT)
            {
                // From here on a vendor request is refused instead of touching a dock that is going away.
                s_shuttingDown = true;
                dock->SaveConfig();
            }
            else if (event == obs_frontend_event::OBS_FRONTEND_EVENT_PROFILE_CHANGED)
            {
                dock->LoadConfig();
            }
        }, dock
    );

    return true;
}

// obs-websocket asks for vendors to be registered here, after it has loaded itself.
void obs_module_post_load()
{
    RegisterVendorApi();
}

void obs_module_unload()
{
    s_shuttingDown = true;
    UnregisterVendorApi();
    s_dock = nullptr;
    delete s_dispatcher;
    s_dispatcher = nullptr;
}

const char *obs_module_description(void)
{
    return "Multiple RTMP Output Plugin";
}
