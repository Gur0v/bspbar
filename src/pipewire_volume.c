#define _POSIX_C_SOURCE 200809L

#include "pipewire_volume.h"
#include "util.h"

#include <pipewire/pipewire.h>
#include <pipewire/extensions/metadata.h>
#include <spa/param/param.h>
#include <spa/param/props.h>
#include <spa/pod/parser.h>
#include <spa/pod/iter.h>

#include <spa/utils/dict.h>

#include <signal.h>
#include <stdint.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    uint32_t id;
    char name[MAX_NAME];
    char serial[32];
} PwNodeEntry;

typedef struct {
    App *app;
    struct pw_thread_loop *loop;
    struct pw_context *context;
    struct pw_core *core;
    struct pw_registry *registry;
    struct pw_metadata *metadata;
    struct pw_node *node;
    struct spa_hook registry_listener;
    struct spa_hook metadata_listener;
    struct spa_hook node_listener;
    uint32_t metadata_id;
    uint32_t node_id;
    char default_sink_name[MAX_NAME];
    char default_sink_serial[32];
    char last_volume[16];
    PwNodeEntry nodes[128];
    size_t node_count;
} PwVolume;

static bool pw_volume_from_array_prop(const struct spa_pod_prop *prop, float *out) {
    if (!prop || !out) {
        return false;
    }

    float values[16];
    uint32_t n_values = spa_pod_copy_array(&prop->value, SPA_TYPE_Float, values, 16);
    if (n_values == 0) {
        return false;
    }

    float sum = 0.0f;
    for (uint32_t i = 0; i < n_values; i++) {
        sum += values[i];
    }
    *out = sum / (float)n_values;
    return true;
}

static int pw_volume_to_percent(float volume) {
    if (volume < 0.0f) {
        volume = 0.0f;
    }

    /* PipeWire stores sink volumes on a cubic scale; convert back to the
       user-facing linear percentage that tools like wpctl display. */
    float linear = cbrtf(volume);
    int percent = (int)(linear * 100.0f + 0.5f);
    if (percent < 0) {
        percent = 0;
    }
    return percent;
}

static void pw_send_volume(PwVolume *pwv, const char *volume) {
    if (!volume || strcmp(volume, pwv->last_volume) == 0) {
        return;
    }

    WorkerMessage msg = {.type = WORKER_MSG_VOLUME};
    snprintf(msg.payload.volume, sizeof(msg.payload.volume), "%s", volume);
    if (write_full(pwv->app->worker_write_fd, &msg, sizeof(msg))) {
        snprintf(pwv->last_volume, sizeof(pwv->last_volume), "%s", volume);
    }
}

static bool pw_parse_json_string_field(const char *json, const char *field, char *out, size_t out_len) {
    if (!json || !field || !out || out_len == 0) {
        return false;
    }

    char needle[64];
    snprintf(needle, sizeof(needle), "\"%s\"", field);
    const char *name = strstr(json, needle);
    if (!name) {
        return false;
    }

    const char *colon = strchr(name, ':');
    if (!colon) {
        return false;
    }

    const char *start = strchr(colon, '"');
    if (!start) {
        return false;
    }
    start++;

    const char *end = strchr(start, '"');
    if (!end || end <= start) {
        return false;
    }

    size_t len = (size_t)(end - start);
    if (len >= out_len) {
        len = out_len - 1;
    }

    memcpy(out, start, len);
    out[len] = '\0';
    return true;
}

static bool pw_parse_default_sink_name(const char *value, char out[MAX_NAME]) {
    if (!value) {
        return false;
    }
    return pw_parse_json_string_field(value, "name", out, MAX_NAME);
}

static bool pw_parse_default_sink_serial(const char *value, char out[32]) {
    if (!value) {
        return false;
    }
    return pw_parse_json_string_field(value, "object.serial", out, 32);
}

static int pw_node_index_by_id(PwVolume *pwv, uint32_t id) {
    for (size_t i = 0; i < pwv->node_count; i++) {
        if (pwv->nodes[i].id == id) {
            return (int)i;
        }
    }
    return -1;
}

static int pw_node_index_by_name(PwVolume *pwv, const char *name) {
    if (!name || !*name) {
        return -1;
    }
    for (size_t i = 0; i < pwv->node_count; i++) {
        if (strcmp(pwv->nodes[i].name, name) == 0) {
            return (int)i;
        }
    }
    return -1;
}

static int pw_node_index_by_serial(PwVolume *pwv, const char *serial) {
    if (!serial || !*serial) {
        return -1;
    }
    for (size_t i = 0; i < pwv->node_count; i++) {
        if (strcmp(pwv->nodes[i].serial, serial) == 0) {
            return (int)i;
        }
    }
    return -1;
}

static void pw_destroy_node(PwVolume *pwv) {
    if (pwv->node) {
        pw_proxy_destroy((struct pw_proxy *)pwv->node);
        pwv->node = NULL;
        memset(&pwv->node_listener, 0, sizeof(pwv->node_listener));
        pwv->node_id = PW_ID_ANY;
    }
}

static void pw_bind_target_node(PwVolume *pwv);

static void pw_node_event_info(void *data, const struct pw_node_info *info) {
    (void)data;
    (void)info;
}

static void pw_node_event_param(
    void *data,
    int seq,
    uint32_t id,
    uint32_t index,
    uint32_t next,
    const struct spa_pod *param) {
    (void)seq;
    (void)index;
    (void)next;

    PwVolume *pwv = data;
    if (id != SPA_PARAM_Props || !param) {
        return;
    }

    const struct spa_pod_prop *mute_prop = spa_pod_find_prop(param, NULL, SPA_PROP_mute);
    const struct spa_pod_prop *soft_mute_prop = spa_pod_find_prop(param, NULL, SPA_PROP_softMute);
    const struct spa_pod_prop *volume_prop = spa_pod_find_prop(param, NULL, SPA_PROP_volume);
    const struct spa_pod_prop *channels_prop = spa_pod_find_prop(param, NULL, SPA_PROP_channelVolumes);
    const struct spa_pod_prop *soft_channels_prop = spa_pod_find_prop(param, NULL, SPA_PROP_softVolumes);

    bool mute = false;
    bool have_mute = false;
    if (soft_mute_prop && spa_pod_get_bool(&soft_mute_prop->value, &mute) == 0) {
        have_mute = true;
    } else if (mute_prop && spa_pod_get_bool(&mute_prop->value, &mute) == 0) {
        have_mute = true;
    }

    float volume = 0.0f;
    bool have_volume = false;

    if (pw_volume_from_array_prop(channels_prop, &volume)) {
        have_volume = true;
    } else if (pw_volume_from_array_prop(soft_channels_prop, &volume)) {
        have_volume = true;
    } else if (volume_prop && spa_pod_get_float(&volume_prop->value, &volume) == 0) {
        have_volume = true;
    }

    if (!have_volume && !have_mute) {
        return;
    }

    int percent = (have_mute && mute) ? 0 : pw_volume_to_percent(volume);

    char out[16];
    snprintf(out, sizeof(out), "%d%%", percent);
    pw_send_volume(pwv, out);
}

static const struct pw_node_events pw_node_events = {
    .version = PW_VERSION_NODE_EVENTS,
    .info = pw_node_event_info,
    .param = pw_node_event_param,
};

static void pw_bind_target_node(PwVolume *pwv) {
    int index = pw_node_index_by_serial(pwv, pwv->default_sink_serial);
    if (index < 0) {
        index = pw_node_index_by_name(pwv, pwv->default_sink_name);
    }
    if (index < 0) {
        return;
    }

    uint32_t node_id = pwv->nodes[index].id;
    if (pwv->node && pwv->node_id == node_id) {
        return;
    }

    pw_destroy_node(pwv);

    pwv->node = pw_registry_bind(
        pwv->registry,
        node_id,
        PW_TYPE_INTERFACE_Node,
        PW_VERSION_NODE,
        0);
    if (!pwv->node) {
        return;
    }

    pwv->node_id = node_id;
    pw_node_add_listener(pwv->node, &pwv->node_listener, &pw_node_events, pwv);

    uint32_t ids[] = {SPA_PARAM_Props};
    pw_node_subscribe_params(pwv->node, ids, 1);
    pw_node_enum_params(pwv->node, 0, SPA_PARAM_Props, 0, UINT32_MAX, NULL);
}

static int pw_metadata_property(
    void *data,
    uint32_t subject,
    const char *key,
    const char *type,
    const char *value) {
    (void)subject;
    (void)type;

    PwVolume *pwv = data;
    if (!key || strcmp(key, "default.audio.sink") != 0) {
        return 0;
    }

    char name[MAX_NAME];
    char serial[32] = {0};
    if (!pw_parse_default_sink_name(value, name)) {
        return 0;
    }
    pw_parse_default_sink_serial(value, serial);

    if (strcmp(name, pwv->default_sink_name) != 0 ||
        strcmp(serial, pwv->default_sink_serial) != 0) {
        snprintf(pwv->default_sink_name, sizeof(pwv->default_sink_name), "%s", name);
        snprintf(pwv->default_sink_serial, sizeof(pwv->default_sink_serial), "%s", serial);
        pw_bind_target_node(pwv);
    }

    return 0;
}

static const struct pw_metadata_events pw_metadata_events = {
    .version = PW_VERSION_METADATA_EVENTS,
    .property = pw_metadata_property,
};

static void pw_registry_global(
    void *data,
    uint32_t id,
    uint32_t permissions,
    const char *type,
    uint32_t version,
    const struct spa_dict *props) {
    (void)permissions;
    (void)version;

    PwVolume *pwv = data;
    if (strcmp(type, PW_TYPE_INTERFACE_Metadata) == 0) {
        const char *metadata_name = props ? spa_dict_lookup(props, PW_KEY_METADATA_NAME) : NULL;
        if (!pwv->metadata && metadata_name && strcmp(metadata_name, "default") == 0) {
            pwv->metadata = pw_registry_bind(
                pwv->registry,
                id,
                PW_TYPE_INTERFACE_Metadata,
                PW_VERSION_METADATA,
                0);
            if (pwv->metadata) {
                pwv->metadata_id = id;
                pw_metadata_add_listener(
                    pwv->metadata,
                    &pwv->metadata_listener,
                    &pw_metadata_events,
                    pwv);
            }
        }
        return;
    }

    if (strcmp(type, PW_TYPE_INTERFACE_Node) != 0 || !props) {
        return;
    }

    const char *node_name = spa_dict_lookup(props, PW_KEY_NODE_NAME);
    const char *object_serial = spa_dict_lookup(props, PW_KEY_OBJECT_SERIAL);
    if (!node_name || !*node_name) {
        return;
    }

    int existing = pw_node_index_by_id(pwv, id);
    if (existing >= 0) {
        snprintf(pwv->nodes[existing].name, sizeof(pwv->nodes[existing].name), "%s", node_name);
        snprintf(
            pwv->nodes[existing].serial,
            sizeof(pwv->nodes[existing].serial),
            "%s",
            object_serial ? object_serial : "");
    } else if (pwv->node_count < sizeof(pwv->nodes) / sizeof(pwv->nodes[0])) {
        pwv->nodes[pwv->node_count].id = id;
        snprintf(pwv->nodes[pwv->node_count].name, sizeof(pwv->nodes[pwv->node_count].name), "%s", node_name);
        snprintf(
            pwv->nodes[pwv->node_count].serial,
            sizeof(pwv->nodes[pwv->node_count].serial),
            "%s",
            object_serial ? object_serial : "");
        pwv->node_count++;
    }

    if ((pwv->default_sink_serial[0] != '\0' && object_serial && strcmp(object_serial, pwv->default_sink_serial) == 0) ||
        (pwv->default_sink_name[0] != '\0' && strcmp(node_name, pwv->default_sink_name) == 0)) {
        pw_bind_target_node(pwv);
    }
}

static void pw_registry_global_remove(void *data, uint32_t id) {
    PwVolume *pwv = data;

    if (pwv->node && pwv->node_id == id) {
        pw_destroy_node(pwv);
    }

    int index = pw_node_index_by_id(pwv, id);
    if (index >= 0) {
        for (size_t i = (size_t)index; i + 1 < pwv->node_count; i++) {
            pwv->nodes[i] = pwv->nodes[i + 1];
        }
        pwv->node_count--;
    }
}

static const struct pw_registry_events pw_registry_events = {
    .version = PW_VERSION_REGISTRY_EVENTS,
    .global = pw_registry_global,
    .global_remove = pw_registry_global_remove,
};

bool pipewire_volume_start(App *app) {
    PwVolume *pwv = calloc(1, sizeof(*pwv));
    if (!pwv) {
        return false;
    }

    pwv->app = app;
    pwv->metadata_id = PW_ID_ANY;
    pwv->node_id = PW_ID_ANY;

    pw_init(NULL, NULL);

    pwv->loop = pw_thread_loop_new("bspbar-pipewire", NULL);
    if (!pwv->loop) {
        free(pwv);
        return false;
    }

    pwv->context = pw_context_new(pw_thread_loop_get_loop(pwv->loop), NULL, 0);
    if (!pwv->context) {
        pw_thread_loop_destroy(pwv->loop);
        free(pwv);
        return false;
    }

    pwv->core = pw_context_connect(pwv->context, NULL, 0);
    if (!pwv->core) {
        pw_context_destroy(pwv->context);
        pw_thread_loop_destroy(pwv->loop);
        free(pwv);
        return false;
    }

    pwv->registry = pw_core_get_registry(pwv->core, PW_VERSION_REGISTRY, 0);
    if (!pwv->registry) {
        pw_core_disconnect(pwv->core);
        pw_context_destroy(pwv->context);
        pw_thread_loop_destroy(pwv->loop);
        free(pwv);
        return false;
    }

    pw_registry_add_listener(pwv->registry, &pwv->registry_listener, &pw_registry_events, pwv);

    if (pw_thread_loop_start(pwv->loop) < 0) {
        pw_proxy_destroy((struct pw_proxy *)pwv->registry);
        pw_core_disconnect(pwv->core);
        pw_context_destroy(pwv->context);
        pw_thread_loop_destroy(pwv->loop);
        free(pwv);
        return false;
    }

    app->pipewire_volume = pwv;
    app->pipewire_started = true;
    return true;
}

void pipewire_volume_stop(App *app) {
    PwVolume *pwv = app->pipewire_volume;
    if (!pwv) {
        return;
    }

    pw_thread_loop_stop(pwv->loop);
    pw_destroy_node(pwv);
    if (pwv->metadata) {
        pw_proxy_destroy((struct pw_proxy *)pwv->metadata);
    }
    if (pwv->registry) {
        pw_proxy_destroy((struct pw_proxy *)pwv->registry);
    }
    if (pwv->core) {
        pw_core_disconnect(pwv->core);
    }
    if (pwv->context) {
        pw_context_destroy(pwv->context);
    }
    if (pwv->loop) {
        pw_thread_loop_destroy(pwv->loop);
    }

    app->pipewire_volume = NULL;
    app->pipewire_started = false;
    free(pwv);
    pw_deinit();
}
