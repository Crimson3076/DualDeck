#pragma once

#include <mutex>

#include "host/emulator_input_sink.h"

namespace melonds_remote::host {

// Default IEmulatorInputSink used until a real melonDS integration exists.
// Tracks the latest state and logs transitions so the prototype's behavior
// (including fail-safe release) can be observed and tested end-to-end
// without needing melonDS built.
class LoggingInputSink : public IEmulatorInputSink {
public:
    void applyControllerState(const ControllerState& state) override;
    void releaseAll() override;

    ControllerState lastState() const;

private:
    mutable std::mutex mutex_;
    ControllerState lastState_;
};

} // namespace melonds_remote::host
