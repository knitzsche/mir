/*
 * Copyright © 2012 Canonical Ltd.
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * Authored by: Robert Carr <robert.carr@canonical.com>
 */

#include "key_repeat_dispatcher.h"

#include "mir/input/device.h"
#include "mir/input/input_device_hub.h"
#include "mir/time/alarm_factory.h"
#include "mir/time/alarm.h"
#include "mir/events/event_private.h"
#include "mir/cookie/authority.h"

#include <boost/throw_exception.hpp>

#include <algorithm>
#include <stdexcept>
#include <string.h>

namespace mi = mir::input;

namespace
{
struct DeviceRemovalFilter : mi::InputDeviceObserver
{
    DeviceRemovalFilter(std::function<void(MirInputDeviceId)> const& on_removal)
        : on_removal(on_removal) {}

    void device_added(std::shared_ptr<mi::Device> const&) override
    {
    }

    void device_changed(std::shared_ptr<mi::Device> const&) override
    {
    }

    void device_removed(std::shared_ptr<mi::Device> const& device) override
    {
        on_removal(device->id());
    }

    void changes_complete() override
    {
    }
    std::function<void(MirInputDeviceId)> on_removal;
};

}

mi::KeyRepeatDispatcher::KeyRepeatDispatcher(
    std::shared_ptr<mi::InputDispatcher> const& next_dispatcher,
    std::shared_ptr<mir::time::AlarmFactory> const& factory,
    std::shared_ptr<mir::cookie::Authority> const& cookie_authority,
    bool repeat_enabled,
    std::chrono::milliseconds repeat_timeout,
    std::chrono::milliseconds repeat_delay)
    : next_dispatcher(next_dispatcher),
      alarm_factory(factory),
      cookie_authority(cookie_authority),
      repeat_enabled(repeat_enabled),
      repeat_timeout(repeat_timeout),
      repeat_delay(repeat_delay)
{
}

void mi::KeyRepeatDispatcher::set_input_device_hub(std::shared_ptr<InputDeviceHub> const& hub)
{
    hub->add_observer(std::make_shared<DeviceRemovalFilter>(
            [this](MirInputDeviceId id)
            {
                std::unique_lock<std::mutex> lock(repeat_state_mutex);
                repeat_state_by_device.erase(id); // destructor cancels alarms
            }
            ));
}

mi::KeyRepeatDispatcher::KeyboardState& mi::KeyRepeatDispatcher::ensure_state_for_device_locked(std::lock_guard<std::mutex> const&, MirInputDeviceId id)
{
    repeat_state_by_device.insert(std::make_pair(id, KeyboardState()));
    return repeat_state_by_device[id];
}

bool mi::KeyRepeatDispatcher::dispatch(MirEvent const& event)
{
    if (!repeat_enabled) // if we made this mutable we'd need a guard
    {
	return next_dispatcher->dispatch(event);
    }
    
    if (mir_event_get_type(&event) == mir_event_type_input)
    {
        auto iev = mir_event_get_input_event(&event);
        if (mir_input_event_get_type(iev) != mir_input_event_type_key)
            return next_dispatcher->dispatch(event);
        if (!handle_key_input(mir_input_event_get_device_id(iev), mir_input_event_get_keyboard_event(iev)))
            return next_dispatcher->dispatch(event);
        else
            return true;
    }
    return next_dispatcher->dispatch(event);
}

// Returns true if the original event has been handled, that is ::dispatch should not pass it on.
bool mi::KeyRepeatDispatcher::handle_key_input(MirInputDeviceId id, MirKeyboardEvent const* kev)
{
    std::lock_guard<std::mutex> lg(repeat_state_mutex);
    auto& device_state = ensure_state_for_device_locked(lg, id);

    auto scan_code = mir_keyboard_event_scan_code(kev);

    switch (mir_keyboard_event_action(kev))
    {
    case mir_keyboard_action_up:
    {
        auto it = device_state.repeat_alarms_by_scancode.find(scan_code);
        if (it == device_state.repeat_alarms_by_scancode.end())
        {
            return false;
        }
        device_state.repeat_alarms_by_scancode.erase(it);
        break;
    }
    case mir_keyboard_action_down:
    {
        MirKeyboardEvent new_kev = *kev;
        new_kev.set_action(mir_keyboard_action_repeat);

        auto it = device_state.repeat_alarms_by_scancode.find(scan_code);
        if (it != device_state.repeat_alarms_by_scancode.end())
        {
            // When we receive a duplicated down we just replace the action
            next_dispatcher->dispatch(new_kev);
            return true;
        }
        auto& capture_alarm = device_state.repeat_alarms_by_scancode[scan_code];
        std::shared_ptr<mir::time::Alarm> alarm = alarm_factory->create_alarm([this, &capture_alarm, new_kev]() mutable
            {
                std::lock_guard<std::mutex> lg(repeat_state_mutex);

                new_kev.set_event_time(std::chrono::steady_clock::now().time_since_epoch());
                auto const cookie = cookie_authority->make_cookie(new_kev.event_time().count());
                auto const serialized_cookie = cookie->serialize();
                new_kev.set_cookie(serialized_cookie);
                next_dispatcher->dispatch(new_kev);

                capture_alarm->reschedule_in(repeat_delay);
            });
        alarm->reschedule_in(repeat_timeout);
        device_state.repeat_alarms_by_scancode[scan_code] = {alarm};
    }
    case mir_keyboard_action_repeat:
        // Should we consume existing repeats?
        break;
    default:
        BOOST_THROW_EXCEPTION(std::logic_error("Unexpected key event action"));
    }
    return false;
}

void mi::KeyRepeatDispatcher::start()
{
    next_dispatcher->start();
}

void mi::KeyRepeatDispatcher::stop()
{
    std::lock_guard<std::mutex> lg(repeat_state_mutex);
    
    repeat_state_by_device.clear();
        
    next_dispatcher->stop();
}
