// Copyright 2026 Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <memory>
#include <vector>
#include "core/frontend/input.h"
#include "input_common/main.h"

namespace InputCommon {

class BaristaButtonFactory : public Input::Factory<Input::ButtonDevice> {
public:
    std::unique_ptr<Input::ButtonDevice> Create(const Common::ParamPackage& params) override;
};

class BaristaAnalogFactory : public Input::Factory<Input::AnalogDevice> {
public:
    std::unique_ptr<Input::AnalogDevice> Create(const Common::ParamPackage& params) override;
};

class BaristaTouchFactory : public Input::Factory<Input::TouchDevice> {
public:
    std::unique_ptr<Input::TouchDevice> Create(const Common::ParamPackage& params) override;
};

class BaristaMotionFactory : public Input::Factory<Input::MotionDevice> {
public:
    std::unique_ptr<Input::MotionDevice> Create(const Common::ParamPackage& params) override;
};

std::vector<std::unique_ptr<Polling::DevicePoller>> GetBaristaPollers(Polling::DeviceType type);

} // namespace InputCommon
