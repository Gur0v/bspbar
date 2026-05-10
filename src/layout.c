#include "layout.h"

#include <X11/XKBlib.h>
#include <X11/Xlib-xcb.h>
#include <xkbcommon/xkbcommon-x11.h>

#include <ctype.h>
#include <stdio.h>
#include <string.h>

static const char *abbreviate_layout_name(const char *name, char out[16]) {
    if (!name || !*name) {
        return "??";
    }

    if (strstr(name, "English") || strstr(name, "english")) {
        return "us";
    }
    if (strstr(name, "Russian") || strstr(name, "russian")) {
        return "ru";
    }
    if (strstr(name, "Ukrainian") || strstr(name, "ukrainian")) {
        return "ua";
    }

    size_t n = 0;
    for (const unsigned char *p = (const unsigned char *)name; *p && n + 1 < 16; p++) {
        if (isalpha(*p)) {
            out[n++] = (char)tolower(*p);
        }
        if (n == 3) {
            break;
        }
    }

    if (n == 0) {
        return "??";
    }

    out[n] = '\0';
    return out;
}

static bool layout_refresh_state(App *app) {
    struct xkb_keymap *keymap = xkb_x11_keymap_new_from_device(
        app->xkb_context,
        app->xcb,
        app->xkb_device_id,
        XKB_KEYMAP_COMPILE_NO_FLAGS);
    if (!keymap) {
        return false;
    }

    struct xkb_state *state = xkb_x11_state_new_from_device(keymap, app->xcb, app->xkb_device_id);
    if (!state) {
        xkb_keymap_unref(keymap);
        return false;
    }

    if (app->xkb_state) {
        xkb_state_unref(app->xkb_state);
    }
    if (app->xkb_keymap) {
        xkb_keymap_unref(app->xkb_keymap);
    }

    app->xkb_keymap = keymap;
    app->xkb_state = state;
    return true;
}

bool layout_init(App *app) {
    app->xcb = XGetXCBConnection(app->dpy);
    if (!app->xcb) {
        return false;
    }

    uint16_t major = XKB_X11_MIN_MAJOR_XKB_VERSION;
    uint16_t minor = XKB_X11_MIN_MINOR_XKB_VERSION;
    uint8_t base_event = 0;
    uint8_t base_error = 0;

    if (!xkb_x11_setup_xkb_extension(
            app->xcb,
            major,
            minor,
            XKB_X11_SETUP_XKB_EXTENSION_NO_FLAGS,
            &major,
            &minor,
            &base_event,
            &base_error)) {
        return false;
    }

    app->xkb_context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    if (!app->xkb_context) {
        return false;
    }

    app->xkb_device_id = xkb_x11_get_core_keyboard_device_id(app->xcb);
    if (app->xkb_device_id < 0) {
        xkb_context_unref(app->xkb_context);
        app->xkb_context = NULL;
        return false;
    }

    app->xkb_event_base = base_event;
    int xkb_opcode = 0;
    int xkb_error = 0;
    int xkb_major = XkbMajorVersion;
    int xkb_minor = XkbMinorVersion;
    if (XkbQueryExtension(
            app->dpy,
            &xkb_opcode,
            &app->xkb_event_base,
            &xkb_error,
            &xkb_major,
            &xkb_minor)) {
        XkbSelectEventDetails(
            app->dpy,
            XkbUseCoreKbd,
            XkbStateNotify,
            XkbGroupStateMask,
            XkbGroupStateMask);
    }

    return layout_refresh_state(app);
}

void layout_destroy(App *app) {
    if (app->xkb_state) {
        xkb_state_unref(app->xkb_state);
        app->xkb_state = NULL;
    }
    if (app->xkb_keymap) {
        xkb_keymap_unref(app->xkb_keymap);
        app->xkb_keymap = NULL;
    }
    if (app->xkb_context) {
        xkb_context_unref(app->xkb_context);
        app->xkb_context = NULL;
    }
}

bool layout_update(App *app) {
    if (!app->xkb_context && !layout_init(app)) {
        snprintf(app->kbd_layout, sizeof(app->kbd_layout), "??");
        return false;
    }
    if (!layout_refresh_state(app)) {
        snprintf(app->kbd_layout, sizeof(app->kbd_layout), "??");
        return false;
    }

    xkb_layout_index_t index = xkb_state_serialize_layout(app->xkb_state, XKB_STATE_LAYOUT_EFFECTIVE);
    const char *name = xkb_keymap_layout_get_name(app->xkb_keymap, index);
    char short_name[16] = {0};
    const char *abbr = abbreviate_layout_name(name, short_name);
    snprintf(app->kbd_layout, sizeof(app->kbd_layout), "%s", abbr);
    return true;
}
