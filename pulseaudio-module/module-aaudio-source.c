/***
  This file is part of PulseAudio.

  Copyright 2004-2008 Lennart Poettering

  PulseAudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License as published
  by the Free Software Foundation; either version 2.1 of the License,
  or (at your option) any later version.

  PulseAudio is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  General Public License for more details.

  You should have received a copy of the GNU Lesser General Public License
  along with PulseAudio; if not, see <http://www.gnu.org/licenses/>.
***/

/* AAudio capture source, the input counterpart to module-aaudio-sink.
 *
 * Without a real source, the only capture device PulseAudio offers is
 * AAudioSink.monitor. Wine enumerates monitors as Windows recording devices, so
 * games pick it up as a microphone and end up transmitting their own output -
 * in a voice chat that means rebroadcasting the whole lobby back into it.
 *
 * Structure deliberately mirrors module-aaudio-sink.c: an AAudio callback hands
 * buffers to the PulseAudio I/O thread over an asyncmsgq, the same recreate /
 * try-start / try-stop message pattern handles stream loss, and the state machine
 * lives in set_state_in_io_thread.
 *
 * Two deliberate differences from the sink:
 *
 *  - The AAudio stream is stopped whenever the source is not RUNNING, not just
 *    when it is suspended. A sink holding an idle output stream costs nothing,
 *    but a source holding an idle input stream keeps the microphone open, which
 *    lights the Android privacy indicator for the whole session and runs into
 *    the background-capture restrictions on Android 14+.
 *
 *  - pa__done stops the stream and joins the I/O thread before returning, so
 *    unloading the module actually releases the microphone.
 */

#include <config.h>

#include <pulse/timeval.h>

#include <pulsecore/i18n.h>
#include <pulsecore/module.h>
#include <pulsecore/source.h>
#include <pulsecore/thread.h>
#include <pulsecore/modargs.h>

#include <sys/system_properties.h>
#include <android/versioning.h>
#undef __INTRODUCED_IN
#define __INTRODUCED_IN(api_level)
#include <aaudio/AAudio.h>
#include <android/log.h>

#define LOG_TAG "GN-PulseAudioSource"
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN,  LOG_TAG, __VA_ARGS__)

PA_MODULE_AUTHOR("Tom Yan, BrunoSX, Joshua Tam");
PA_MODULE_DESCRIPTION("Winlator AAudio source");
PA_MODULE_VERSION(PACKAGE_VERSION);
PA_MODULE_LOAD_ONCE(false);
PA_MODULE_USAGE(
    "source_name=<name for the source> "
    "source_properties=<properties for the source> "
    "rate=<sampling rate> "
    "channels=<number of channels> "
    "performance_mode=<performance mode: 0 (NONE), 1 (Low Latency), 2 (Power Saving)> "
    "input_preset=<raw AAudio input preset, default 7 (VOICE_COMMUNICATION); "
                  "1 GENERIC, 5 CAMCORDER, 6 VOICE_RECOGNITION, 7 VOICE_COMMUNICATION, 9 UNPROCESSED> "
    "set_default=<make this the default source: true/false (default: true)>"
);

#define DEFAULT_SOURCE_NAME "AAudioSource"

/* AAUDIO_INPUT_PRESET_VOICE_COMMUNICATION - gives us the hardware echo canceller
 * and noise suppressor, which matters because the game's own output is usually
 * coming out of the same device's speaker. */
#define DEFAULT_INPUT_PRESET 7

enum {
    SOURCE_MESSAGE_CAPTURE = PA_SOURCE_MESSAGE_MAX,
    SOURCE_MESSAGE_TRY_START,
    SOURCE_MESSAGE_TRY_STOP,
    SOURCE_MESSAGE_RECREATE,
};

struct userdata {
    pa_core *core;
    pa_module *module;
    pa_source *source;

    pa_thread *thread;
    pa_thread_mq thread_mq;
    pa_rtpoll *rtpoll;
    pa_rtpoll_item *rtpoll_item;
    pa_asyncmsgq *aaudio_msgq;

    size_t frame_size;

    AAudioStreamBuilder *builder;
    AAudioStream *stream;
    pa_sample_spec ss;

    int performance_mode;
    int input_preset;
    bool set_default;

    int32_t frames_per_burst;
    int32_t device_sample_rate;
    int32_t device_channels;
    int32_t last_device_id;

    bool stream_started;
};

static const char* const valid_modargs[] = {
    "source_name",
    "source_properties",
    "rate",
    "channels",
    "performance_mode",
    "input_preset",
    "set_default",
    NULL
};

static int get_android_sdk_version(void) {
    char sdk_version_str[PROP_VALUE_MAX];
    if (__system_property_get("ro.build.version.sdk", sdk_version_str) > 0) {
        return atoi(sdk_version_str);
    }
    return 0;
}

static void schedule_start(struct userdata *u) {
    pa_asyncmsgq_post(u->thread_mq.inq, PA_MSGOBJECT(u->source), SOURCE_MESSAGE_TRY_START, NULL, 0, NULL, NULL);
}

static void schedule_stop(struct userdata *u) {
    pa_asyncmsgq_post(u->thread_mq.inq, PA_MSGOBJECT(u->source), SOURCE_MESSAGE_TRY_STOP, NULL, 0, NULL, NULL);
}

static void schedule_recreate(struct userdata *u) {
    pa_asyncmsgq_post(u->thread_mq.inq, PA_MSGOBJECT(u->source), SOURCE_MESSAGE_RECREATE, NULL, 0, NULL, NULL);
}

/* Runs on the AAudio callback thread. Hand the buffer to the I/O thread and block
 * until it has been posted, so audioData stays valid for the whole call. */
static aaudio_data_callback_result_t aaudio_data_callback(AAudioStream *stream, void *userdata, void *audioData, int32_t numFrames) {
    struct userdata *u = userdata;
    return pa_asyncmsgq_send(u->aaudio_msgq, PA_MSGOBJECT(u->source), SOURCE_MESSAGE_CAPTURE, audioData, numFrames, NULL);
}

static void aaudio_error_callback(AAudioStream *stream, void *userdata, aaudio_result_t error) {
    struct userdata *u = userdata;

    if (error == AAUDIO_ERROR_DISCONNECTED ||
        error == AAUDIO_ERROR_INVALID_STATE ||
        error == AAUDIO_ERROR_INVALID_HANDLE ||
        error == AAUDIO_ERROR_TIMEOUT) {
        LOGW("AAudio stream error (%d), attempting to reconnect...", error);
        schedule_recreate(u);
    } else {
        LOGW("AAudio error callback: %d", error);
    }
}

static pa_usec_t get_aaudio_latency(struct userdata *u) {
    if (u->stream != NULL && u->ss.rate > 0) {
        int32_t buffer_size = AAudioStream_getBufferSizeInFrames(u->stream);
        if (buffer_size > 0) {
            return PA_USEC_PER_SEC * (int64_t) (buffer_size + u->frames_per_burst) / u->ss.rate;
        }
    }

    return 20 * PA_USEC_PER_MSEC;
}

static void update_pa_latency(struct userdata *u) {
    if (u->source) {
        pa_usec_t latency = get_aaudio_latency(u);
        if (pa_thread_mq_get()) {
            pa_source_set_fixed_latency_within_thread(u->source, latency);
        } else {
            pa_source_set_fixed_latency(u->source, latency);
        }
    }
}

static int pa_create_aaudio_stream(struct userdata *u) {
    aaudio_result_t res;

    res = AAudio_createStreamBuilder(&u->builder);
    if (res != AAUDIO_OK) {
        LOGW("AAudio_createStreamBuilder() failed.");
        return -1;
    }

    AAudioStreamBuilder_setDirection(u->builder, AAUDIO_DIRECTION_INPUT);
    AAudioStreamBuilder_setPerformanceMode(u->builder, u->performance_mode);
    AAudioStreamBuilder_setDataCallback(u->builder, aaudio_data_callback, u);
    AAudioStreamBuilder_setErrorCallback(u->builder, aaudio_error_callback, u);
    AAudioStreamBuilder_setFormat(u->builder, u->ss.format == PA_SAMPLE_FLOAT32LE ? AAUDIO_FORMAT_PCM_FLOAT : AAUDIO_FORMAT_PCM_I16);

    /* Let AAudio pick the device's native capture rate and channel count and adapt
     * the source to whatever we are given, rather than forcing a resample here. */
    AAudioStreamBuilder_setSampleRate(u->builder, AAUDIO_UNSPECIFIED);
    AAudioStreamBuilder_setChannelCount(u->builder, AAUDIO_UNSPECIFIED);

    /* setInputPreset is API 28. Guarded the same way the sink guards setUsage. */
    if (get_android_sdk_version() >= 28) {
        AAudioStreamBuilder_setInputPreset(u->builder, u->input_preset);
    }

    res = AAudioStreamBuilder_openStream(u->builder, &u->stream);
    if (res != AAUDIO_OK) {
        /* The most likely cause here is a missing or revoked RECORD_AUDIO grant. */
        LOGW("AAudioStreamBuilder_openStream() failed: %d (%s)", res, AAudio_convertResultToText(res));
        AAudioStreamBuilder_delete(u->builder);
        u->builder = NULL;
        return -1;
    }

    AAudioStreamBuilder_delete(u->builder);
    u->builder = NULL;

    u->device_sample_rate = AAudioStream_getSampleRate(u->stream);
    u->device_channels = AAudioStream_getChannelCount(u->stream);
    aaudio_format_t actual_format = AAudioStream_getFormat(u->stream);

    LOGW("AAudio input stream opened: %d Hz, %d channels, format %d, device %d",
         u->device_sample_rate, u->device_channels, actual_format,
         AAudioStream_getDeviceId(u->stream));

    u->ss.rate = u->device_sample_rate;
    u->ss.channels = u->device_channels;

    if (actual_format == AAUDIO_FORMAT_PCM_FLOAT) {
        u->ss.format = PA_SAMPLE_FLOAT32LE;
    } else if (actual_format == AAUDIO_FORMAT_PCM_I16) {
        u->ss.format = PA_SAMPLE_S16LE;
    }

    u->frames_per_burst = AAudioStream_getFramesPerBurst(u->stream);
    u->last_device_id = AAudioStream_getDeviceId(u->stream);
    u->frame_size = pa_frame_size(&u->ss);
    u->stream_started = false;

    update_pa_latency(u);

    return 0;
}

static void try_start_stream(struct userdata *u) {
    aaudio_result_t res;

    if (!u->stream || u->stream_started) return;

    res = AAudioStream_requestStart(u->stream);
    if (res == AAUDIO_OK) {
        u->stream_started = true;
        LOGW("AAudioStream_requestStart() succeeded");
    } else {
        LOGW("AAudioStream_requestStart() failed: %d, scheduling recreate", res);
        schedule_recreate(u);
    }
}

static void try_stop_stream(struct userdata *u) {
    aaudio_result_t res;

    if (!u->stream || !u->stream_started) return;

    res = AAudioStream_requestStop(u->stream);
    if (res != AAUDIO_OK) {
        LOGW("AAudioStream_requestStop() failed: %d", res);
    } else {
        LOGW("AAudio input stream stopped, microphone released");
    }
    u->stream_started = false;
}

static void recreate_aaudio_stream(struct userdata *u) {
    if (u->stream) {
        AAudioStream_requestStop(u->stream);
        AAudioStream_close(u->stream);
        u->stream = NULL;
        u->stream_started = false;
    }

    if (pa_create_aaudio_stream(u) < 0) {
        LOGW("Failed to create AAudio input stream");
        /* Deliberately not rescheduling: if the microphone permission is denied,
         * retrying forever would spin. The next state transition will retry. */
    } else {
        LOGW("AAudio input stream created, attempting to start");
        schedule_start(u);
    }
}

static int source_process_capture(struct userdata *u, void *audioData, int64_t numFrames) {
    pa_memchunk chunk;

    if (!PA_SOURCE_IS_LINKED(u->source->thread_info.state)) return AAUDIO_CALLBACK_RESULT_STOP;

    /* Drop the buffer rather than posting it while suspended or not running. */
    if (u->source->thread_info.state != PA_SOURCE_RUNNING) return AAUDIO_CALLBACK_RESULT_CONTINUE;

    if (numFrames <= 0) return AAUDIO_CALLBACK_RESULT_CONTINUE;

    /* audioData belongs to AAudio and is only valid for this call. pa_source_post
     * copies into the source outputs' buffers before returning, and the callback
     * thread is blocked in pa_asyncmsgq_send until we get here, so wrapping it
     * read-only without copying is safe. */
    chunk.memblock = pa_memblock_new_fixed(u->core->mempool, audioData, u->frame_size * numFrames, true);
    chunk.index = 0;
    chunk.length = u->frame_size * numFrames;

    pa_source_post(u->source, &chunk);

    pa_memblock_unref_fixed(chunk.memblock);

    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

static int source_process_msg(pa_msgobject *o, int code, void *data, int64_t offset, pa_memchunk *memchunk) {
    struct userdata *u = PA_SOURCE(o)->userdata;

    if (code == SOURCE_MESSAGE_CAPTURE) return source_process_capture(u, data, offset);

    if (code == SOURCE_MESSAGE_TRY_START) {
        LOGW("AAudio try start requested");
        if (u->source->thread_info.state == PA_SOURCE_RUNNING) {
            try_start_stream(u);
        } else {
            LOGW("Source no longer running, canceling try");
        }
        return 0;
    }

    if (code == SOURCE_MESSAGE_TRY_STOP) {
        LOGW("AAudio try stop requested");
        try_stop_stream(u);
        return 0;
    }

    if (code == SOURCE_MESSAGE_RECREATE) {
        LOGW("AAudio recreate requested");
        if (PA_SOURCE_IS_LINKED(u->source->thread_info.state)) {
            recreate_aaudio_stream(u);
        } else {
            LOGW("Source no longer linked, canceling recreate");
        }
        return 0;
    }

    return pa_source_process_msg(o, code, data, offset, memchunk);
}

static int source_set_state_io_thread(pa_source *s, pa_source_state_t state, pa_suspend_cause_t suspend_cause) {
    struct userdata *u = s->userdata;

    LOGW("AAudio source state transition: current=%d, target=%d, suspend_cause=%d",
         s->thread_info.state, state, suspend_cause);

    /* Hold the microphone open only while something is actually recording. */
    if (state == PA_SOURCE_RUNNING) {
        if (!u->stream) {
            LOGW("No AAudio stream, recreating before start");
            recreate_aaudio_stream(u);
        } else {
            aaudio_stream_state_t stream_state = AAudioStream_getState(u->stream);
            if (stream_state == AAUDIO_STREAM_STATE_STARTED ||
                stream_state == AAUDIO_STREAM_STATE_STARTING) {
                u->stream_started = true;
            } else if (stream_state == AAUDIO_STREAM_STATE_DISCONNECTED) {
                LOGW("AAudio stream disconnected, recreating");
                schedule_recreate(u);
            } else {
                schedule_start(u);
            }
        }
    } else {
        /* IDLE, SUSPENDED or UNLINKED - all mean nobody is recording right now. */
        if (u->stream_started) {
            schedule_stop(u);
        }
    }

    return 0;
}

static void thread_func(void *userdata) {
    struct userdata *u = userdata;
    pa_thread_mq_install(&u->thread_mq);

    for (;;) {
        int res = pa_rtpoll_run(u->rtpoll);
        if (res < 0) {
            goto error;
        } else if (res == 0) {
            break;
        }
    }

    return;

error:
    pa_asyncmsgq_post(u->thread_mq.outq, PA_MSGOBJECT(u->core), PA_CORE_MESSAGE_UNLOAD_MODULE, u->module, 0, NULL, NULL);
    pa_asyncmsgq_wait_for(u->thread_mq.inq, PA_MESSAGE_SHUTDOWN);
}

void pa__done(pa_module *m) {
    struct userdata *u;

    if (!(u = m->userdata)) return;

    if (u->source) pa_source_unlink(u->source);

    /* Join the I/O thread before touching the stream, so nothing is mid-post. */
    if (u->thread) {
        pa_asyncmsgq_send(u->thread_mq.inq, NULL, PA_MESSAGE_SHUTDOWN, NULL, 0, NULL);
        pa_thread_free(u->thread);
    }

    pa_thread_mq_done(&u->thread_mq);

    if (u->source) pa_source_unref(u->source);

    /* Stop before close so the microphone is released promptly and the Android
     * privacy indicator clears. */
    if (u->stream) {
        AAudioStream_requestStop(u->stream);
        AAudioStream_close(u->stream);
        u->stream = NULL;
    }

    if (u->builder) AAudioStreamBuilder_delete(u->builder);
    if (u->rtpoll_item) pa_rtpoll_item_free(u->rtpoll_item);
    if (u->aaudio_msgq) pa_asyncmsgq_unref(u->aaudio_msgq);
    if (u->rtpoll) pa_rtpoll_free(u->rtpoll);

    pa_xfree(u);
}

int pa__init(pa_module *m) {
    struct userdata *u = NULL;
    pa_modargs *ma = NULL;
    pa_channel_map map;
    pa_source_new_data data;
    int performance_mode = 0;

    if (!(ma = pa_modargs_new(m->argument, valid_modargs))) {
        LOGW("Failed to parse module arguments.");
        goto error;
    }

    m->userdata = u = pa_xnew0(struct userdata, 1);

    u->core = m->core;
    u->module = m;
    u->rtpoll = pa_rtpoll_new();

    if (pa_thread_mq_init(&u->thread_mq, m->core->mainloop, u->rtpoll) < 0) {
        LOGW("pa_thread_mq_init() failed.");
        goto error;
    }

    u->aaudio_msgq = pa_asyncmsgq_new(0);
    if (!u->aaudio_msgq) {
        LOGW("pa_asyncmsgq_new() failed.");
        goto error;
    }

    u->rtpoll_item = pa_rtpoll_item_new_asyncmsgq_read(u->rtpoll, PA_RTPOLL_EARLY-1, u->aaudio_msgq);

    u->ss = m->core->default_sample_spec;
    map = m->core->default_channel_map;

    if (pa_modargs_get_sample_spec_and_channel_map(ma, &u->ss, &map, PA_CHANNEL_MAP_DEFAULT) < 0) {
        LOGW("pa_modargs_get_sample_spec_and_channel_map() failed.");
        goto error;
    }

    /* Mono is what voice chat wants; overridden below by whatever AAudio opens. */
    u->ss.channels = 1;
    u->ss.format = u->ss.format == PA_SAMPLE_FLOAT32LE || u->ss.format == PA_SAMPLE_FLOAT32BE ? PA_SAMPLE_FLOAT32LE : PA_SAMPLE_S16LE;

    u->performance_mode = AAUDIO_PERFORMANCE_MODE_LOW_LATENCY;
    u->input_preset = DEFAULT_INPUT_PRESET;
    u->set_default = true;

    if (!pa_modargs_get_value_s32(ma, "performance_mode", &performance_mode)) {
        switch (performance_mode) {
            case 0:
                u->performance_mode = AAUDIO_PERFORMANCE_MODE_NONE;
                break;
            case 1:
                u->performance_mode = AAUDIO_PERFORMANCE_MODE_LOW_LATENCY;
                break;
            case 2:
                u->performance_mode = AAUDIO_PERFORMANCE_MODE_POWER_SAVING;
                break;
        }
    }

    if (pa_modargs_get_value_s32(ma, "input_preset", &u->input_preset) < 0) {
        LOGW("Failed to parse input_preset argument.");
        goto error;
    }

    if (pa_modargs_get_value_boolean(ma, "set_default", &u->set_default) < 0) {
        LOGW("Failed to parse set_default argument.");
        goto error;
    }

    if (pa_create_aaudio_stream(u) < 0) goto error;

    /* The stream is opened here only to discover the device's real format; nothing
     * is captured until a client connects and the source goes RUNNING. */
    pa_channel_map_init_extend(&map, u->ss.channels, PA_CHANNEL_MAP_DEFAULT);

    pa_source_new_data_init(&data);
    data.driver = __FILE__;
    data.module = m;
    pa_source_new_data_set_name(&data, pa_modargs_get_value(ma, "source_name", DEFAULT_SOURCE_NAME));
    pa_source_new_data_set_sample_spec(&data, &u->ss);
    pa_source_new_data_set_alternate_sample_rate(&data, u->ss.rate);
    pa_source_new_data_set_channel_map(&data, &map);

    pa_proplist_sets(data.proplist, PA_PROP_DEVICE_DESCRIPTION, _("AAudio Input"));
    pa_proplist_sets(data.proplist, PA_PROP_DEVICE_CLASS, "abstract");

    if (pa_modargs_get_proplist(ma, "source_properties", data.proplist, PA_UPDATE_REPLACE) < 0) {
        LOGW("pa_modargs_get_proplist() failed.");
        pa_source_new_data_done(&data);
        goto error;
    }

    u->source = pa_source_new(m->core, &data, PA_SOURCE_HARDWARE);
    pa_source_new_data_done(&data);

    if (!u->source) {
        LOGW("Failed to create source object.");
        goto error;
    }

    u->source->parent.process_msg = source_process_msg;
    u->source->set_state_in_io_thread = source_set_state_io_thread;
    u->source->userdata = u;

    pa_source_set_asyncmsgq(u->source, u->thread_mq.inq);
    pa_source_set_rtpoll(u->source, u->rtpoll);
    update_pa_latency(u);

    if (!(u->thread = pa_thread_new("aaudio-source", thread_func, u))) {
        LOGW("Failed to create thread.");
        goto error;
    }

    pa_source_put(u->source);

    /* Outrank AAudioSink.monitor, which PulseAudio creates for every sink and
     * which Wine would otherwise hand to games as their microphone. */
    if (u->set_default) {
        pa_core_set_configured_default_source(m->core, u->source->name);
    }

    pa_modargs_free(ma);
    return 0;

error:
    if (ma) pa_modargs_free(ma);
    pa__done(m);
    return -1;
}
