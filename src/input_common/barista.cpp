// Copyright 2026 Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include "input_common/barista.h"
#include "common/param_package.h"
#include "core/frontend/barista/barista_app_hook.h"

#include <cmath>
#include <string>
#include <unordered_map>

namespace InputCommon {

namespace {

const std::unordered_map<std::string, BaristaAppHook::Button> s_button_names = {
    {"a", BaristaAppHook::Button::A},
    {"b", BaristaAppHook::Button::B},
    {"x", BaristaAppHook::Button::X},
    {"y", BaristaAppHook::Button::Y},
    {"up", BaristaAppHook::Button::Up},
    {"down", BaristaAppHook::Button::Down},
    {"left", BaristaAppHook::Button::Left},
    {"right", BaristaAppHook::Button::Right},
    {"l", BaristaAppHook::Button::L},
    {"r", BaristaAppHook::Button::R},
    {"zl", BaristaAppHook::Button::ZL},
    {"zr", BaristaAppHook::Button::ZR},
    {"start", BaristaAppHook::Button::Start},
    {"select", BaristaAppHook::Button::Select},
    {"home", BaristaAppHook::Button::Home},
    {"l3", BaristaAppHook::Button::L3},
    {"r3", BaristaAppHook::Button::R3},
    {"tv", BaristaAppHook::Button::TV},
    {"sync", BaristaAppHook::Button::Sync},
};

class BaristaButtonDevice : public Input::ButtonDevice {
public:
    explicit BaristaButtonDevice(BaristaAppHook::Button button_) : button(button_) {}
    bool GetStatus() const override {
        return BaristaAppHook::GetButton(button);
    }

private:
    BaristaAppHook::Button button;
};

class BaristaAnalogDevice : public Input::AnalogDevice {
public:
    explicit BaristaAnalogDevice(bool right_stick_) : right_stick(right_stick_) {}
    std::tuple<float, float> GetStatus() const override {
        auto [x, y] = BaristaAppHook::GetStick(right_stick);
        return {x, y};
    }

private:
    bool right_stick;
};

class BaristaTouchDevice : public Input::TouchDevice {
public:
    std::tuple<float, float, bool> GetStatus() const override {
        float x = 0.0f, y = 0.0f;
        bool pressed = BaristaAppHook::GetTouch(x, y);
        return {x, y, pressed};
    }
};

class BaristaMotionDevice : public Input::MotionDevice {
public:
    std::tuple<Common::Vec3<float>, Common::Vec3<float>> GetStatus() const override {
        auto [accel, gyro] = BaristaAppHook::GetMotion();
        return {accel, gyro};
    }
};

class BaristaPoller : public Polling::DevicePoller {
public:
    explicit BaristaPoller(Polling::DeviceType type_) : type(type_) {}
    void Start() override {}
    void Stop() override {}

    Common::ParamPackage GetNextInput() override {
        if (!BaristaAppHook::IsConnected())
            return {};

        if (type == Polling::DeviceType::Button) {
            for (const auto& [name, btn] : s_button_names) {
                if (BaristaAppHook::GetButton(btn)) {
                    return Common::ParamPackage{
                        {"engine", "barista"},
                        {"button", name},
                    };
                }
            }
        } else if (type == Polling::DeviceType::Analog) {
            auto [lx, ly] = BaristaAppHook::GetStick(false);
            if (std::sqrt(lx * lx + ly * ly) > 0.5f) {
                return Common::ParamPackage{
                    {"engine", "barista"},
                    {"axis", "circle_pad"},
                };
            }
            auto [rx, ry] = BaristaAppHook::GetStick(true);
            if (std::sqrt(rx * rx + ry * ry) > 0.5f) {
                return Common::ParamPackage{
                    {"engine", "barista"},
                    {"axis", "c_stick"},
                };
            }
        }
        return {};
    }

private:
    Polling::DeviceType type;
};

} // namespace

std::unique_ptr<Input::ButtonDevice> BaristaButtonFactory::Create(
    const Common::ParamPackage& params) {
    const auto button_str = params.Get("button", "a");
    auto it = s_button_names.find(button_str);
    if (it != s_button_names.end()) {
        return std::make_unique<BaristaButtonDevice>(it->second);
    }
    return std::make_unique<BaristaButtonDevice>(BaristaAppHook::Button::A);
}

std::unique_ptr<Input::AnalogDevice> BaristaAnalogFactory::Create(
    const Common::ParamPackage& params) {
    const auto axis = params.Get("axis", "circle_pad");
    const bool is_right = (axis == "c_stick" || axis == "right");
    return std::make_unique<BaristaAnalogDevice>(is_right);
}

std::unique_ptr<Input::TouchDevice> BaristaTouchFactory::Create(
    const Common::ParamPackage& /*params*/) {
    return std::make_unique<BaristaTouchDevice>();
}

std::unique_ptr<Input::MotionDevice> BaristaMotionFactory::Create(
    const Common::ParamPackage& /*params*/) {
    return std::make_unique<BaristaMotionDevice>();
}

std::vector<std::unique_ptr<Polling::DevicePoller>> GetBaristaPollers(Polling::DeviceType type) {
    std::vector<std::unique_ptr<Polling::DevicePoller>> pollers;
    pollers.push_back(std::make_unique<BaristaPoller>(type));
    return pollers;
}

} // namespace InputCommon
