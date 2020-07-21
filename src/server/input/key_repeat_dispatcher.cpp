/*
 * Copyright © 2012-2020 Canonical Ltd.
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 or 3,
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
#include "mir/events/event_builders.h"
#include "mir/cookie/authority.h"

#include <xkbcommon/xkbcommon-keysyms.h>

#include <boost/throw_exception.hpp>

#include <algorithm>
#include <stdexcept>
#include <chrono>

namespace mi = mir::input;
namespace mev = mir::events;

namespace
{
struct DeviceRemovalFilter : mi::InputDeviceObserver
{
    DeviceRemovalFilter(mi::KeyRepeatDispatcher* dispatcher)
        : dispatcher{dispatcher} {}

    void device_added(std::shared_ptr<mi::Device> const& device) override
    {
        if (device->name() == "mtk-tpd")
        {
            dispatcher->set_touch_button_device(device->id());
        }

    }

    void device_changed(std::shared_ptr<mi::Device> const&) override
    {
    }

    void device_removed(std::shared_ptr<mi::Device> const& device) override
    {
        dispatcher->remove_device(device->id());
    }

    void changes_complete() override
    {
    }
    mi::KeyRepeatDispatcher* dispatcher;
};

auto is_meta_key(MirKeyboardEvent const* event) -> bool
{
    auto const key_code = mir_keyboard_event_key_code(event);
    switch(key_code)
    {
    case XKB_KEY_Shift_R:
    case XKB_KEY_Shift_L:
    case XKB_KEY_Alt_R:
    case XKB_KEY_Alt_L:
    case XKB_KEY_Control_R:
    case XKB_KEY_Control_L:
    case XKB_KEY_Super_L:
    case XKB_KEY_Super_R:
    case XKB_KEY_Caps_Lock:
    case XKB_KEY_Scroll_Lock:
    case XKB_KEY_Num_Lock: return true;
    default: return ((XKB_KEY_Shift_L <= key_code) && (key_code <= XKB_KEY_Hyper_R));
    }
}
}

mi::KeyRepeatDispatcher::KeyRepeatDispatcher(
    std::shared_ptr<mi::InputDispatcher> const& next_dispatcher,
    std::shared_ptr<mir::time::AlarmFactory> const& factory,
    std::shared_ptr<mir::cookie::Authority> const& cookie_authority,
    bool repeat_enabled,
    std::chrono::milliseconds repeat_timeout,
    std::chrono::milliseconds repeat_delay,
    bool disable_repeat_on_touchscreen)
    : next_dispatcher(next_dispatcher),
      alarm_factory(factory),
      cookie_authority(cookie_authority),
      repeat_enabled(repeat_enabled),
      repeat_timeout(repeat_timeout),
      repeat_delay(repeat_delay),
      disable_repeat_on_touchscreen(disable_repeat_on_touchscreen)
{
}

void mi::KeyRepeatDispatcher::set_input_device_hub(std::shared_ptr<InputDeviceHub> const& hub)
{
    hub->add_observer(std::make_shared<DeviceRemovalFilter>(this));
}

void mi::KeyRepeatDispatcher::set_touch_button_device(MirInputDeviceId id)
{
    std::lock_guard<std::mutex> lock(repeat_state_mutex);
    touch_button_device = id;
}

void mi::KeyRepeatDispatcher::remove_device(MirInputDeviceId id)
{
    std::lock_guard<std::mutex> lock(repeat_state_mutex);
    repeat_state_by_device.erase(id); // destructor cancels alarms
    if (touch_button_device.is_set() && touch_button_device.value() == id)
        touch_button_device.consume();
}

mi::KeyRepeatDispatcher::KeyboardState& mi::KeyRepeatDispatcher::ensure_state_for_device_locked(std::lock_guard<std::mutex> const&, MirInputDeviceId id)
{
    repeat_state_by_device.insert(std::make_pair(id, KeyboardState()));
    return repeat_state_by_device[id];
}

bool mi::KeyRepeatDispatcher::dispatch(std::shared_ptr<MirEvent const> const& event)
{
    if (!repeat_enabled) // if we made this mutable we'd need a guard
    {
	return next_dispatcher->dispatch(event);
    }

    if (mir_event_get_type(event.get()) == mir_event_type_input)
    {
        auto iev = mir_event_get_input_event(event.get());
        if (mir_input_event_get_type(iev) != mir_input_event_type_key)
            return next_dispatcher->dispatch(event);
        auto device_id = mir_input_event_get_device_id(iev);
        if (disable_repeat_on_touchscreen && touch_button_device.is_set() && device_id == touch_button_device.value())
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
        device_state.repeat_alarm.reset();
        break;
    }
    case mir_keyboard_action_down:
    {
        // We don't want to track and auto-repeat individual meta key presses
        // That leads, for example, to alternating Ctrl and Alt repeats when
        // both keys are pressed.
        if (is_meta_key(kev))
        {
            // Further, we don't want to repeat with the old modifier state.
            // So just cancel any existing repeats and carry on.
            device_state.repeat_alarm.reset();
            return false;
        }

        auto clone_event = [scan_code, id, this,
             key_code = mir_keyboard_event_key_code(kev),
             modifiers = mir_keyboard_event_modifiers(kev)]()
             {
                 auto const now = std::chrono::steady_clock::now().time_since_epoch();
                 auto const cookie = cookie_authority->make_cookie(now.count());
                 auto new_event = mev::make_event(
                     id,
                     now,
                     cookie->serialize(),
                     mir_keyboard_action_repeat,
                     key_code,
                     scan_code,
                     modifiers);
                 next_dispatcher->dispatch(std::move(new_event));
             };

        // We need to provide the alarm lambda with the alarm (which doesn't exist yet) so
        // that it can reschedule. This is a placeholder while we create the lambda & alarm.
        auto const shared_weak_alarm = std::make_shared<std::weak_ptr<time::Alarm>>();

        std::shared_ptr<mir::time::Alarm> alarm = alarm_factory->create_alarm(
            [clone_event, shared_weak_alarm, repeat_delay=repeat_delay]()
            {
                clone_event();

                if (auto const& repeat_alarm = shared_weak_alarm->lock())
                    repeat_alarm->reschedule_in(repeat_delay);
            });

        // Fulfill the placeholder before scheduling the alarm.
        *shared_weak_alarm = alarm;
        alarm->reschedule_in(repeat_timeout);
        device_state.repeat_alarm = alarm;
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
