// Copyright 2026 Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <array>
#include <span>
#include <string>
#include <vector>
#include "common/common_types.h"
#include "common/vector_math.h"
#include "core/frontend/framebuffer_layout.h"

namespace BaristaAppHook {

struct Status {
    bool enabled = false;
    bool connected = false;
    bool socketFileExists = false;
    bool gameActive = false;
    std::string effectiveSocketPath;
    std::string configuredSocketPath;
    u64 framesSent = 0;
    u64 audioChunksSent = 0;
    u64 inputReportsReceived = 0;
    s64 lastInputMsAgo = -1;
    std::string rejectionReason;
    std::string lockHolder;
};

enum class Button : u32 {
    A,
    B,
    X,
    Y,
    Up,
    Down,
    Left,
    Right,
    L,
    R,
    ZL,
    ZR,
    Start,
    Select,
    Home,
    L3,
    R3,
    TV,
    Sync,
    NumButtons
};

void Initialize(std::vector<u8> idleRgb, unsigned width, unsigned height,
                const std::string& customSocketPath = "", bool enabled = false);
void Shutdown();
void SetGameActive(bool active);
bool WantsFrame();
void SubmitFrame(std::vector<u8> rgb, unsigned width, unsigned height);
void SubmitAudio(std::span<const s16> samples, unsigned channels);
bool ReadInput(std::array<u8, 128>& report);

// Decoded inputs for direct polling / input engines
// Keep a report acquired with ReadInput for one emulated HID update and use these
// helpers to decode it. This prevents one update from combining different reports.
bool IsButtonPressed(const std::array<u8, 128>& report, Button button);
std::pair<float, float> DecodeStick(const std::array<u8, 128>& report, bool right_stick);
bool DecodeTouch(const std::array<u8, 128>& report, float& out_x, float& out_y);
std::pair<Common::Vec3<float>, Common::Vec3<float>>
DecodeMotion(const std::array<u8, 128>& report);

bool GetButton(Button button);
std::pair<float, float> GetStick(bool right_stick); // returns (x, y) in [-1.0, 1.0]
bool GetTouch(float& out_x, float& out_y);          // returns normalized (0..1) in bottom screen
std::pair<Common::Vec3<float>, Common::Vec3<float>> GetMotion(); // (accel, gyro)

// Layout for Barista based on current setting
Layout::FramebufferLayout GetLayout();

std::string GetDefaultSocketPath();
std::string GetEffectiveSocketPath();
bool IsConnected();
Status GetStatus();
void Reconfigure(const std::string& customSocketPath, bool enabled);
void Reconnect();

} // namespace BaristaAppHook
