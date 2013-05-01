/*
 * Copyright © 2013 Canonical Ltd.
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * Authored by: Thomas Voss <thomas.voss@canonical.com>
 */

#ifndef MIR_TOOLKIT_EVENT_H_
#define MIR_TOOLKIT_EVENT_H_

#include <stddef.h>
#include <stdint.h>
#include "mir_toolkit/common.h"

#ifdef __cplusplus
/**
 * \addtogroup mir_toolkit
 * @{
 */
extern "C" {
#endif
/* TODO: To the moon. */
#define MIR_INPUT_EVENT_MAX_POINTER_COUNT 16

typedef int64_t nsecs_t;

typedef enum
{
    mir_event_type_key,
    mir_event_type_motion,
    mir_event_type_surface
} MirEventType;

typedef enum {
    mir_key_action_down     = 0,
    mir_key_action_up       = 1,
    mir_key_action_multiple = 2
} MirKeyAction;

typedef enum {
    mir_key_flag_woke_here           = 0x1,
    mir_key_flag_soft_keyboard       = 0x2,
    mir_key_flag_keep_touch_mode     = 0x4,
    mir_key_flag_from_system         = 0x8,
    mir_key_flag_editor_action       = 0x10,
    mir_key_flag_canceled            = 0x20,
    mir_key_flag_virtual_hard_key    = 0x40,
    mir_key_flag_long_press          = 0x80,
    mir_key_flag_canceled_long_press = 0x100,
    mir_key_flag_tracking            = 0x200,
    mir_key_flag_fallback            = 0x400
} MirKeyFlag;

typedef enum {
    mir_key_modifier_none        = 0,
    mir_key_modifier_alt         = 0x02,
    mir_key_modifier_alt_left    = 0x10,
    mir_key_modifier_alt_right   = 0x20,
    mir_key_modifier_shift       = 0x01,
    mir_key_modifier_shift_left  = 0x40,
    mir_key_modifier_shift_right = 0x80,
    mir_key_modifier_sym         = 0x04,
    mir_key_modifier_function    = 0x08,
    mir_key_modifier_ctrl        = 0x1000,
    mir_key_modifier_ctrl_left   = 0x2000,
    mir_key_modifier_ctrl_right  = 0x4000,
    mir_key_modifier_meta        = 0x10000,
    mir_key_modifier_meta_left   = 0x20000,
    mir_key_modifier_meta_right  = 0x40000,
    mir_key_modifier_caps_lock   = 0x100000,
    mir_key_modifier_num_lock    = 0x200000,
    mir_key_modifier_scroll_lock = 0x400000
} MirKeyModifier;

typedef enum {
    mir_motion_action_down         = 0,
    mir_motion_action_up           = 1,
    mir_motion_action_move         = 2,
    mir_motion_action_cancel       = 3,
    mir_motion_action_outside      = 4,
    mir_motion_action_pointer_down = 5,
    mir_motion_action_pointer_up   = 6,
    mir_motion_action_hover_move   = 7,
    mir_motion_action_scroll       = 8,
    mir_motion_action_hover_enter  = 9,
    mir_motion_action_hover_exit   = 10
} MirMotionAction;

typedef enum {
    mir_motion_flag_window_is_obscured = 0x1
} MirMotionFlag;

typedef enum {
    mir_motion_button_primary   = 1 << 0,
    mir_motion_button_secondary = 1 << 1,
    mir_motion_button_tertiary  = 1 << 2,
    mir_motion_button_back      = 1 << 3,
    mir_motion_button_forward   = 1 << 4
} MirMotionButton;

typedef struct
{
    MirEventType type;

    int32_t device_id;
    int32_t source_id;
    MirKeyAction action;
    MirKeyFlag flags;
    unsigned int modifiers;

    int32_t key_code;
    int32_t scan_code;
    int32_t repeat_count;
    nsecs_t down_time;
    nsecs_t event_time;
    int is_system_key;
} MirKeyEvent;

typedef struct
{
    MirEventType type;

    int32_t device_id;
    int32_t source_id;
    MirMotionAction action;
    MirMotionFlag flags;
    int32_t meta_state;

    int32_t edge_flags;
    MirMotionButton button_state;
    float x_offset;
    float y_offset;
    float x_precision;
    float y_precision;
    nsecs_t down_time;
    nsecs_t event_time;

    size_t pointer_count;
    struct
    {
        int id;
        float x, raw_x;
        float y, raw_y;
        float touch_major;
        float touch_minor;
        float size;
        float pressure;
        float orientation;
        float vscroll;
        float hscroll;
    } pointer_coordinates[MIR_INPUT_EVENT_MAX_POINTER_COUNT];
} MirMotionEvent;

typedef struct
{
    MirEventType type;

    int id;
    MirSurfaceAttrib attrib;
    int value;
} MirSurfaceEvent;

typedef union
{
    MirEventType    type;
    MirKeyEvent     key;
    MirMotionEvent  motion;
    MirSurfaceEvent surface;
} MirEvent;

#ifdef __cplusplus
}
/**@}*/
#endif

#endif /* MIR_TOOLKIT_EVENT_H_ */
