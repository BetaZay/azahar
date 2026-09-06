// Copyright 2026 Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <memory>
#include <QWidget>

namespace Ui {
class ConfigureBarista;
}

class QTimer;

class ConfigureBarista : public QWidget {
    Q_OBJECT

public:
    explicit ConfigureBarista(QWidget* parent = nullptr);
    ~ConfigureBarista() override;

    void ApplyConfiguration();
    void RetranslateUI();
    void SetConfiguration();

private slots:
    void OnBrowseSocket();
    void OnResetSocket();
    void OnReconnect();
    void OnRefresh();
    void OnUpdateStatus();
    void OnScreenModeChanged(int index);

private:
    void UpdateStatus();
    void UpdateModeDescription(int index);

    std::unique_ptr<Ui::ConfigureBarista> ui;
    std::unique_ptr<QTimer> status_timer;
};
