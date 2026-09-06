// Copyright 2026 Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include "citra_qt/configuration/configure_barista.h"
#include "common/settings.h"
#include "core/frontend/barista/barista_app_hook.h"
#include "ui_configure_barista.h"

#include <QFileDialog>
#include <QFileInfo>
#include <QTimer>

ConfigureBarista::ConfigureBarista(QWidget* parent)
    : QWidget(parent), ui(std::make_unique<Ui::ConfigureBarista>()),
      status_timer(std::make_unique<QTimer>(this)) {
    ui->setupUi(this);

    connect(ui->button_browse_socket, &QPushButton::clicked, this,
            &ConfigureBarista::OnBrowseSocket);
    connect(ui->button_reset_socket, &QPushButton::clicked, this, &ConfigureBarista::OnResetSocket);
    connect(ui->button_reconnect, &QPushButton::clicked, this, &ConfigureBarista::OnReconnect);
    connect(ui->button_refresh, &QPushButton::clicked, this, &ConfigureBarista::OnRefresh);
    connect(ui->combo_screen_mode, qOverload<int>(&QComboBox::currentIndexChanged), this,
            &ConfigureBarista::OnScreenModeChanged);

    connect(status_timer.get(), &QTimer::timeout, this, &ConfigureBarista::OnUpdateStatus);
    status_timer->start(500);

    SetConfiguration();
}

ConfigureBarista::~ConfigureBarista() = default;

void ConfigureBarista::SetConfiguration() {
    ui->checkbox_enabled->setChecked(Settings::values.barista_enabled.GetValue());
    ui->checkbox_input->setChecked(Settings::values.barista_enable_input.GetValue());
    ui->edit_socket_path->setText(
        QString::fromStdString(Settings::values.barista_socket_path.GetValue()));
    ui->combo_screen_mode->setCurrentIndex(
        static_cast<int>(Settings::values.barista_screen_mode.GetValue()));

    UpdateModeDescription(ui->combo_screen_mode->currentIndex());
    UpdateStatus();
}

void ConfigureBarista::ApplyConfiguration() {
    Settings::values.barista_enabled.SetValue(ui->checkbox_enabled->isChecked());
    Settings::values.barista_enable_input.SetValue(ui->checkbox_input->isChecked());
    Settings::values.barista_socket_path.SetValue(
        ui->edit_socket_path->text().trimmed().toStdString());
    Settings::values.barista_screen_mode.SetValue(
        static_cast<Settings::BaristaScreenMode>(ui->combo_screen_mode->currentIndex()));

    BaristaAppHook::Reconfigure(Settings::values.barista_socket_path.GetValue(),
                                Settings::values.barista_enabled.GetValue());
}

void ConfigureBarista::RetranslateUI() {
    ui->retranslateUi(this);
    UpdateModeDescription(ui->combo_screen_mode->currentIndex());
    UpdateStatus();
}

void ConfigureBarista::OnBrowseSocket() {
    QString startDir = QStringLiteral("/run/barista");
    const QString currentPath = ui->edit_socket_path->text().trimmed();
    if (!currentPath.isEmpty()) {
        QFileInfo fi(currentPath);
        if (fi.dir().exists()) {
            startDir = fi.dir().path();
        }
    }

    const QString path = QFileDialog::getOpenFileName(
        this, tr("Select Barista Socket File"), startDir,
        tr("Socket Files (*.sock);;All Files (*.*)"));
    if (!path.isEmpty()) {
        ui->edit_socket_path->setText(path);
        UpdateStatus();
    }
}

void ConfigureBarista::OnResetSocket() {
    ui->edit_socket_path->clear();
    UpdateStatus();
}

void ConfigureBarista::OnReconnect() {
    ApplyConfiguration();
    BaristaAppHook::Reconnect();
    UpdateStatus();
}

void ConfigureBarista::OnRefresh() {
    UpdateStatus();
}

void ConfigureBarista::OnUpdateStatus() {
    if (isVisible()) {
        UpdateStatus();
    }
}

void ConfigureBarista::OnScreenModeChanged(int index) {
    UpdateModeDescription(index);
}

void ConfigureBarista::UpdateModeDescription(int index) {
    switch (static_cast<Settings::BaristaScreenMode>(index)) {
    case Settings::BaristaScreenMode::BottomScreen:
        ui->label_screen_mode_desc->setText(
            tr("Displays the 3DS bottom touchscreen on the Wii U GamePad display at 2x integer scale "
               "(640x480, centered on 854x480). Touches on the GamePad touchscreen map directly "
               "and accurately to 3DS touchscreen coordinates. Recommended for 3DS games."));
        break;
    case Settings::BaristaScreenMode::TopScreen:
        ui->label_screen_mode_desc->setText(
            tr("Displays the 3DS top screen on the Wii U GamePad display at 2x integer scale "
               "(800x480, centered on 854x480). Useful when focusing on the main 3D/gameplay display."));
        break;
    case Settings::BaristaScreenMode::SideBySide:
        ui->label_screen_mode_desc->setText(
            tr("Displays both top and bottom screens side-by-side on the GamePad display. "
               "Touches on the bottom screen region map to the 3DS touchscreen."));
        break;
    case Settings::BaristaScreenMode::TopBottom:
        ui->label_screen_mode_desc->setText(
            tr("Displays both screens stacked vertically on the GamePad display (top screen 400x240 "
               "above bottom screen 320x240, matching the GamePad's native 480 vertical resolution). "
               "Touches on the bottom screen region map to the 3DS touchscreen."));
        break;
    }
}

void ConfigureBarista::UpdateStatus() {
    const auto status = BaristaAppHook::GetStatus();
    const bool is_enabled = ui->checkbox_enabled->isChecked();
    const QString currentPathText = ui->edit_socket_path->text().trimmed();
    const QString effectivePath = currentPathText.isEmpty()
                                      ? QString::fromStdString(status.effectiveSocketPath)
                                      : currentPathText;

    if (!is_enabled) {
        ui->label_status_conn->setText(tr("Disabled"));
        ui->label_status_conn->setStyleSheet(QStringLiteral("color: gray; font-weight: bold;"));
        ui->label_status_sock->setText(effectivePath.isEmpty() ? tr("(Not configured)") : effectivePath);
        ui->label_status_stream->setText(tr("Inactive (disabled)"));
        ui->label_status_input->setText(tr("Disabled"));
        ui->label_status_stats->setText(QStringLiteral("-"));
    } else if (status.connected) {
        ui->label_status_conn->setText(tr("Connected to Barista daemon"));
        ui->label_status_conn->setStyleSheet(QStringLiteral("color: green; font-weight: bold;"));
        ui->label_status_sock->setText(tr("%1 (Connected)").arg(QString::fromStdString(status.effectiveSocketPath)));

        if (status.gameActive) {
            ui->label_status_stream->setText(
                tr("Active (streaming game video and audio to GamePad)"));
        } else {
            ui->label_status_stream->setText(tr("Idle (displaying logo on GamePad)"));
        }

        if (status.lastInputMsAgo >= 0 && status.lastInputMsAgo < 2000) {
            ui->label_status_input->setText(
                tr("Active (reports received: %1, last report: %2 ms ago)")
                    .arg(status.inputReportsReceived)
                    .arg(status.lastInputMsAgo));
        } else if (status.inputReportsReceived > 0) {
            ui->label_status_input->setText(
                tr("Idle (reports received: %1, last report: %2 ms ago)")
                    .arg(status.inputReportsReceived)
                    .arg(status.lastInputMsAgo));
        } else {
            ui->label_status_input->setText(tr("Waiting for GamePad reports..."));
        }

        ui->label_status_stats->setText(
            tr("Frames sent: %1 | Audio chunks: %2 | Input reports: %3")
                .arg(status.framesSent)
                .arg(status.audioChunksSent)
                .arg(status.inputReportsReceived));
    } else {
        if (!status.rejectionReason.empty()) {
            ui->label_status_conn->setText(
                tr("Connection rejected: %1").arg(QString::fromStdString(status.rejectionReason)));
            ui->label_status_conn->setStyleSheet(QStringLiteral("color: red; font-weight: bold;"));
        } else if (!status.lockHolder.empty()) {
            ui->label_status_conn->setText(
                tr("Socket busy (%1)").arg(QString::fromStdString(status.lockHolder)));
            ui->label_status_conn->setStyleSheet(QStringLiteral("color: orange; font-weight: bold;"));
        } else {
            ui->label_status_conn->setText(tr("Disconnected (waiting for Barista daemon)"));
            ui->label_status_conn->setStyleSheet(QStringLiteral("color: red; font-weight: bold;"));
        }

        const bool fileExists = !effectivePath.isEmpty() && QFileInfo::exists(effectivePath);
        if (effectivePath.isEmpty()) {
            ui->label_status_sock->setText(tr("(Not configured)"));
        } else if (fileExists) {
            ui->label_status_sock->setText(
                tr("%1 (Socket found, waiting for daemon)").arg(effectivePath));
        } else {
            ui->label_status_sock->setText(tr("%1 (Socket file not found)").arg(effectivePath));
        }

        if (!status.rejectionReason.empty() || !status.lockHolder.empty()) {
            const auto reason = !status.rejectionReason.empty()
                                    ? QString::fromStdString(status.rejectionReason)
                                    : QString::fromStdString(status.lockHolder);
            ui->label_status_stream->setText(tr("Blocked (%1)").arg(reason));
        } else {
            ui->label_status_stream->setText(tr("Inactive"));
        }

        ui->label_status_input->setText(tr("No GamePad connection"));
        ui->label_status_stats->setText(
            tr("Frames sent: %1 | Audio chunks: %2 | Input reports: %3")
                .arg(status.framesSent)
                .arg(status.audioChunksSent)
                .arg(status.inputReportsReceived));
    }
}
