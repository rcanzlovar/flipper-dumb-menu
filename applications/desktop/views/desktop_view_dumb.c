#include <projdefs.h>
#include <stdint.h>
#include <furi.h>
#include <gui/elements.h>
#include <gui/icon.h>
#include <gui/view.h>
#include <portmacro.h>

#include "../desktop_settings/desktop_settings.h"
#include "../desktop_i.h"
#include "desktop_view_dumb.h"

#define DOOR_MOVING_INTERVAL_MS (1000 / 16)
#define LOCKED_HINT_TIMEOUT_MS (1000)
#define UNLOCKED_HINT_TIMEOUT_MS (2000)

#define DOOR_OFFSET_START -55
#define DOOR_OFFSET_END 0

#define DOOR_L_FINAL_POS 0
#define DOOR_R_FINAL_POS 60

#define UNLOCK_CNT 3
#define UNLOCK_RST_TIMEOUT 600

struct DesktopViewLocked {
    View* view;
    DesktopViewLockedCallback callback;
    void* context;

    TimerHandle_t timer;
    uint8_t lock_count;
    uint32_t lock_lastpress;
};

typedef enum {
    DesktopViewLockedStateUnlocked,
    DesktopViewLockedStateLocked,
    DesktopViewLockedStateDoorsClosing,
    DesktopViewLockedStateLockedHintShown,
    DesktopViewLockedStateUnlockedHintShown
} DesktopViewLockedState;

typedef struct {
    bool pin_locked;
    int8_t door_offset;
    DesktopViewLockedState view_state;
} DesktopViewLockedModel;

void desktop_view_dumb_set_callback(
    DesktopViewLocked* dumb_view,
    DesktopViewLockedCallback callback,
    void* context) {
    furi_assert(dumb_view);
    furi_assert(callback);
    dumb_view->callback = callback;
    dumb_view->context = context;
}

static void dumb_view_timer_callback(TimerHandle_t timer) {
    DesktopViewLocked* dumb_view = pvTimerGetTimerID(timer);
    dumb_view->callback(DesktopLockedEventUpdate, dumb_view->context);
}

static void desktop_view_dumb_doors_draw(Canvas* canvas, DesktopViewLockedModel* model) {
    int8_t offset = model->door_offset;
    uint8_t door_left_x = DOOR_L_FINAL_POS + offset;
    uint8_t door_right_x = DOOR_R_FINAL_POS - offset;
    uint8_t height = icon_get_height(&I_DoorLeft_70x55);
    canvas_draw_icon(canvas, door_left_x, canvas_height(canvas) - height, &I_DoorLeft_70x55);
    canvas_draw_icon(canvas, door_right_x, canvas_height(canvas) - height, &I_DoorRight_70x55);
}

static bool desktop_view_dumb_doors_move(DesktopViewLockedModel* model) {
    bool stop = false;
    if(model->door_offset < DOOR_OFFSET_END) {
        model->door_offset = CLAMP(model->door_offset + 5, DOOR_OFFSET_END, DOOR_OFFSET_START);
        stop = true;
    }

    return stop;
}

static void desktop_view_dumb_update_hint_icon_timeout(DesktopViewLocked* dumb_view) {
    DesktopViewLockedModel* model = view_get_model(dumb_view->view);
    const bool change_state = (model->view_state == DesktopViewLockedStateLocked) &&
                              !model->pin_locked;
    if(change_state) {
        model->view_state = DesktopViewLockedStateLockedHintShown;
    }
    view_commit_model(dumb_view->view, change_state);
    xTimerChangePeriod(dumb_view->timer, pdMS_TO_TICKS(LOCKED_HINT_TIMEOUT_MS), portMAX_DELAY);
}

void desktop_view_locked_update(DesktopViewLocked* dumb_view) {
    DesktopViewLockedModel* model = view_get_model(dumb_view->view);
    DesktopViewLockedState view_state = model->view_state;

    if(view_state == DesktopViewLockedStateDoorsClosing &&
       !desktop_view_dumb_doors_move(model)) {
        model->view_state = DesktopViewLockedStateLocked;
    } else if(view_state == DesktopViewLockedStateLockedHintShown) {
        model->view_state = DesktopViewLockedStateLocked;
    } else if(view_state == DesktopViewLockedStateUnlockedHintShown) {
        model->view_state = DesktopViewLockedStateUnlocked;
    }

    view_commit_model(dumb_view->view, true);

    if(view_state != DesktopViewLockedStateDoorsClosing) {
        xTimerStop(dumb_view->timer, portMAX_DELAY);
    }
}

static void desktop_view_locked_draw(Canvas* canvas, void* model) {
    DesktopViewLockedModel* m = model;
    DesktopViewLockedState view_state = m->view_state;
    canvas_set_color(canvas, ColorBlack);

    if(view_state == DesktopViewLockedStateDoorsClosing) {
        desktop_view_locked_doors_draw(canvas, m);
        canvas_set_font(canvas, FontPrimary);
        elements_multiline_text_framed(canvas, 42, 30 + STATUS_BAR_Y_SHIFT, "Locked");
    } else if(view_state == DesktopViewLockedStateLockedHintShown) {
        canvas_set_font(canvas, FontSecondary);
        elements_bold_rounded_frame(canvas, 14, 2 + STATUS_BAR_Y_SHIFT, 99, 48);
        elements_multiline_text(canvas, 65, 20 + STATUS_BAR_Y_SHIFT, "To unlock\npress:");
        canvas_draw_icon(canvas, 65, 36 + STATUS_BAR_Y_SHIFT, &I_Pin_back_arrow_10x8);
        canvas_draw_icon(canvas, 80, 36 + STATUS_BAR_Y_SHIFT, &I_Pin_back_arrow_10x8);
        canvas_draw_icon(canvas, 95, 36 + STATUS_BAR_Y_SHIFT, &I_Pin_back_arrow_10x8);
        canvas_draw_icon(canvas, 16, 7 + STATUS_BAR_Y_SHIFT, &I_WarningDolphin_45x42);
        canvas_draw_dot(canvas, 17, 61);
    } else if(view_state == DesktopViewLockedStateUnlockedHintShown) {
        canvas_set_font(canvas, FontPrimary);
        elements_multiline_text_framed(canvas, 42, 30 + STATUS_BAR_Y_SHIFT, "Unlocked");
    }
}

View* desktop_view_locked_get_view(DesktopViewLocked* dumb_view) {
    furi_assert(dumb_view);
    return dumb_view->view;
}

static bool desktop_view_locked_input(InputEvent* event, void* context) {
    furi_assert(event);
    furi_assert(context);

    bool is_changed = false;
    const uint32_t press_time = xTaskGetTickCount();
    DesktopViewLocked* dumb_view = context;
    DesktopViewLockedModel* model = view_get_model(dumb_view->view);
    if(model->view_state == DesktopViewLockedStateUnlockedHintShown &&
       event->type == InputTypePress) {
        model->view_state = DesktopViewLockedStateUnlocked;
        is_changed = true;
    }
    const DesktopViewLockedState view_state = model->view_state;
    const bool pin_locked = model->pin_locked;
    view_commit_model(dumb_view->view, is_changed);

    if(view_state == DesktopViewLockedStateUnlocked || event->type != InputTypeShort) {
        return view_state != DesktopViewLockedStateUnlocked;
    } else if(view_state == DesktopViewLockedStateLocked && pin_locked) {
        dumb_view->callback(DesktopLockedEventShowPinInput, dumb_view->context);
    } else if(
        view_state == DesktopViewLockedStateLocked ||
        view_state == DesktopViewLockedStateLockedHintShown) {
        if(press_time - dumb_view->lock_lastpress > UNLOCK_RST_TIMEOUT) {
            dumb_view->lock_lastpress = press_time;
            dumb_view->lock_count = 0;
        }

        desktop_view_dumb_update_hint_icon_timeout(dumb_view);

        if(event->key == InputKeyBack) {
            dumb_view->lock_lastpress = press_time;
            dumb_view->lock_count++;
            if(dumb_view->lock_count == UNLOCK_CNT) {
                dumb_view->callback(DesktopLockedEventUnlocked, dumb_view->context);
            }
        } else {
            dumb_view->lock_count = 0;
        }

        dumb_view->lock_lastpress = press_time;
    }

    return true;
}

DesktopViewLocked* desktop_view_locked_alloc() {
    DesktopViewLocked* dumb_view = malloc(sizeof(DesktopViewLocked));
    dumb_view->view = view_alloc();
    dumb_view->timer =
        xTimerCreate(NULL, 1000 / 16, pdTRUE, dumb_view, dumb_view_timer_callback);

    view_allocate_model(dumb_view->view, ViewModelTypeLocking, sizeof(DesktopViewLockedModel));
    view_set_context(dumb_view->view, dumb_view);
    view_set_draw_callback(dumb_view->view, desktop_view_locked_draw);
    view_set_input_callback(dumb_view->view, desktop_view_locked_input);

    return dumb_view;
}

void desktop_view_locked_free(DesktopViewLocked* dumb_view) {
    furi_assert(dumb_view);
    osTimerDelete(dumb_view->timer);
    view_free(dumb_view->view);
    free(dumb_view);
}

void desktop_view_locked_close_doors(DesktopViewLocked* dumb_view) {
    DesktopViewLockedModel* model = view_get_model(dumb_view->view);
    furi_assert(model->view_state == DesktopViewLockedStateLocked);
    model->view_state = DesktopViewLockedStateDoorsClosing;
    model->door_offset = DOOR_OFFSET_START;
    view_commit_model(dumb_view->view, true);
    xTimerChangePeriod(dumb_view->timer, pdMS_TO_TICKS(DOOR_MOVING_INTERVAL_MS), portMAX_DELAY);
}

void desktop_view_locked_lock(DesktopViewLocked* dumb_view, bool pin_locked) {
    DesktopViewLockedModel* model = view_get_model(dumb_view->view);
    furi_assert(model->view_state == DesktopViewLockedStateUnlocked);
    model->view_state = DesktopViewLockedStateLocked;
    model->pin_locked = pin_locked;
    view_commit_model(dumb_view->view, true);
}

void desktop_view_locked_unlock(DesktopViewLocked* dumb_view) {
    dumb_view->lock_count = 0;
    DesktopViewLockedModel* model = view_get_model(dumb_view->view);
    model->view_state = DesktopViewLockedStateUnlockedHintShown;
    model->pin_locked = false;
    view_commit_model(dumb_view->view, true);
    xTimerChangePeriod(dumb_view->timer, pdMS_TO_TICKS(UNLOCKED_HINT_TIMEOUT_MS), portMAX_DELAY);
}

bool desktop_view_locked_is_locked_hint_visible(DesktopViewLocked* dumb_view) {
    DesktopViewLockedModel* model = view_get_model(dumb_view->view);
    const DesktopViewLockedState view_state = model->view_state;
    view_commit_model(dumb_view->view, false);
    return view_state == DesktopViewLockedStateLockedHintShown;
}
