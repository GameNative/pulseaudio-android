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

#include <config.h>

#include <pulse/timeval.h>

#include <pulsecore/i18n.h>
#include <pulsecore/module.h>
#include <pulsecore/sink.h>
#include <pulsecore/thread.h>
#include <pulsecore/modargs.h>

#include <sys/system_properties.h>
#include <android/versioning.h>
#undef __INTRODUCED_IN
#define __INTRODUCED_IN(api_level)
#include <aaudio/AAudio.h>

PA_MODULE_AUTHOR("Tom Yan, BrunoSX, Joshua Tam");
PA_MODULE_DESCRIPTION("Winlator AAudio sink");
PA_MODULE_VERSION(PACKAGE_VERSION);
PA_MODULE_LOAD_ONCE(false);
PA_MODULE_USAGE(
    "sink_name=<name for the sink> "
    "sink_properties=<properties for the sink> "
    "rate=<sampling rate> "
    "volume=<output volume>"
    "performance_mode=<performance mode: 0 (NONE), 1 (Low Latency), 2 (Power Saving)>"
);

#define DEFAULT_SINK_NAME "AAudioSink"

enum {
    SINK_MESSAGE_RENDER = PA_SINK_MESSAGE_MAX,
    SINK_MESSAGE_RECONNECT,
};

struct userdata {
    pa_core *core;
    pa_module *module;
    pa_sink *sink;

    pa_thread *thread;
    pa_thread_mq thread_mq;
    pa_rtpoll *rtpoll;
	pa_rtpoll_item *rtpoll_item;
    pa_asyncmsgq *aaudio_msgq;

    pa_memchunk memchunk;
    size_t frame_size;

    AAudioStreamBuilder *builder;
    AAudioStream *stream;
	pa_sample_spec ss;
    
    float volume;
    int performance_mode;
    
    int32_t previous_underrun_count;
    int32_t frames_per_burst;
    
    int32_t device_sample_rate;
    int32_t device_channels;
};

static const char* const valid_modargs[] = {
    "sink_name",
    "sink_properties",
    "rate",
    "volume",
    "performance_mode",
    NULL
};

static int get_android_sdk_version(void) {
    char sdk_version_str[PROP_VALUE_MAX];
    if (__system_property_get("ro.build.version.sdk", sdk_version_str) > 0) {
        return atoi(sdk_version_str);
    }
    return 0;
}

static aaudio_data_callback_result_t aaudio_data_callback(AAudioStream *stream, void *userdata, void *audioData, int32_t numFrames) {
    struct userdata* u = userdata;
    return pa_asyncmsgq_send(u->aaudio_msgq, PA_MSGOBJECT(u->sink), SINK_MESSAGE_RENDER, audioData, numFrames, NULL);
}

static void aaudio_error_callback(AAudioStream *stream, void *userdata, aaudio_result_t error) {
    struct userdata* u = userdata;
    
    if (error == AAUDIO_ERROR_DISCONNECTED) {
        pa_log("AAudio device disconnected, attempting to reconnect...");
        pa_asyncmsgq_post(u->thread_mq.inq, PA_MSGOBJECT(u->sink), SINK_MESSAGE_RECONNECT, NULL, 0, NULL, NULL);
    } else {
        pa_log("AAudio error callback: %d", error);
    }
}

static pa_usec_t get_aaudio_latency(struct userdata *u) {
	int32_t bufferSize = AAudioStream_getBufferSizeInFrames(u->stream);
	int32_t framesPerBurst = u->frames_per_burst;
	int32_t totalLatencyFrames = bufferSize + framesPerBurst;
	return PA_USEC_PER_SEC * totalLatencyFrames / u->ss.rate;
}

static void update_pa_latency(struct userdata *u) {
	if (u->sink) {
		pa_usec_t latency = get_aaudio_latency(u);
		if (pa_thread_mq_get()) {
			pa_sink_set_fixed_latency_within_thread(u->sink, latency);
		} else {
			pa_sink_set_fixed_latency(u->sink, latency);
		}
	}
}

static int pa_create_aaudio_stream(struct userdata *u) {
	aaudio_result_t res;

    res = AAudio_createStreamBuilder(&u->builder);
	if (res != AAUDIO_OK) {
		pa_log("AAudio_createStreamBuilder() failed.");
		return -1;
	}

	if (get_android_sdk_version() >= 28) {
		AAudioStreamBuilder_setUsage(u->builder, AAUDIO_USAGE_GAME);
	}

	AAudioStreamBuilder_setSharingMode(u->builder, AAUDIO_SHARING_MODE_SHARED);
	AAudioStreamBuilder_setPerformanceMode(u->builder, u->performance_mode);
	AAudioStreamBuilder_setDataCallback(u->builder, aaudio_data_callback, u);
	AAudioStreamBuilder_setErrorCallback(u->builder, aaudio_error_callback, u);	
    AAudioStreamBuilder_setFormat(u->builder, u->ss.format == PA_SAMPLE_FLOAT32LE ? AAUDIO_FORMAT_PCM_FLOAT : AAUDIO_FORMAT_PCM_I16);
    AAudioStreamBuilder_setSampleRate(u->builder, AAUDIO_UNSPECIFIED);
    AAudioStreamBuilder_setChannelCount(u->builder, AAUDIO_UNSPECIFIED);

    res = AAudioStreamBuilder_openStream(u->builder, &u->stream);
	if (res != AAUDIO_OK) {
		pa_log("AAudioStreamBuilder_openStream() failed.");
		return -1;
	}
	
    res = AAudioStreamBuilder_delete(u->builder);
	if (res != AAUDIO_OK) {
		pa_log("AAudioStreamBuilder_delete() failed.");
		return -1;
	}

	u->device_sample_rate = AAudioStream_getSampleRate(u->stream);
	u->device_channels = AAudioStream_getChannelCount(u->stream);
	aaudio_format_t actual_format = AAudioStream_getFormat(u->stream);
	aaudio_sharing_mode_t actual_sharing_mode = AAudioStream_getSharingMode(u->stream);

	pa_log("AAudio stream opened: %d Hz, %d channels, format %d, sharing mode %s",
	       u->device_sample_rate, u->device_channels, actual_format,
	       actual_sharing_mode == AAUDIO_SHARING_MODE_EXCLUSIVE ? "EXCLUSIVE" : "SHARED");

	u->ss.rate = u->device_sample_rate;
	u->ss.channels = u->device_channels;

	if (actual_format == AAUDIO_FORMAT_PCM_FLOAT) {
		u->ss.format = PA_SAMPLE_FLOAT32LE;
	} else if (actual_format == AAUDIO_FORMAT_PCM_I16) {
		u->ss.format = PA_SAMPLE_S16LE;
	}

	u->frames_per_burst = AAudioStream_getFramesPerBurst(u->stream);
	int32_t bufferCapacity = AAudioStream_getBufferCapacityInFrames(u->stream);
	int32_t bufferSize = u->frames_per_burst * 2;
	
	if (bufferSize > bufferCapacity) {
		bufferSize = bufferCapacity;
		pa_log("AAudio: Requested buffer size exceeds capacity, clamped to %d frames", bufferSize);
	}
	
	res = AAudioStream_setBufferSizeInFrames(u->stream, bufferSize);
	if (res < 0) {
		pa_log("AAudioStream_setBufferSizeInFrames() failed: %d", res);
	} else {
		pa_log("AAudio buffer size set to %d frames (burst: %d, capacity: %d)", 
		       bufferSize, u->frames_per_burst, bufferCapacity);
	}
	
	u->previous_underrun_count = 0;
    u->frame_size = pa_frame_size(&u->ss);

	// Update pulseaudio latency based on current aaudio config
    update_pa_latency(u);

    return 0;
}

static int pa_recreate_aaudio_stream(struct userdata *u) {
    if (u->stream) {
        AAudioStream_requestStop(u->stream);
        AAudioStream_close(u->stream);
        u->stream = NULL;
    }
    
    return pa_create_aaudio_stream(u);
}

static int sink_process_render(struct userdata *u, void *audioData, int64_t numFrames) {
    if (!PA_SINK_IS_LINKED(u->sink->thread_info.state)) return AAUDIO_CALLBACK_RESULT_STOP;

    if (u->sink->thread_info.state == PA_SINK_SUSPENDED) {
        pa_silence_memory(audioData, u->frame_size * numFrames, &u->ss);
        return AAUDIO_CALLBACK_RESULT_CONTINUE;
    }

    u->memchunk.memblock = pa_memblock_new_fixed(u->core->mempool, audioData, u->frame_size * numFrames, false);
    u->memchunk.length = pa_memblock_get_length(u->memchunk.memblock);
    pa_sink_render_into_full(u->sink, &u->memchunk);
    pa_memblock_unref_fixed(u->memchunk.memblock);
    
    int32_t bufferSize = AAudioStream_getBufferSizeInFrames(u->stream);
    int32_t bufferCapacity = AAudioStream_getBufferCapacityInFrames(u->stream);
    
    if (bufferSize < bufferCapacity) {
        int32_t underrunCount = AAudioStream_getXRunCount(u->stream);
        if (underrunCount > u->previous_underrun_count) {
            u->previous_underrun_count = underrunCount;
            bufferSize += u->frames_per_burst;
            if (bufferSize > bufferCapacity) {
                bufferSize = bufferCapacity;
            }
            AAudioStream_setBufferSizeInFrames(u->stream, bufferSize);

        	// Update pulseaudio latency when the aaudio config changed
            update_pa_latency(u);
        }
    }

    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

static int sink_process_msg(pa_msgobject *o, int code, void *data, int64_t offset, pa_memchunk *memchunk) {
    struct userdata* u = PA_SINK(o)->userdata;

	if (code == SINK_MESSAGE_RENDER) return sink_process_render(u, data, offset);
	
	if (code == SINK_MESSAGE_RECONNECT) {
		pa_log("AAudio reconnect requested, current state: %d", u->sink->thread_info.state);
		if (pa_recreate_aaudio_stream(u) == 0) {
			pa_log("AAudio stream recreated successfully");
			if (PA_SINK_IS_OPENED(u->sink->thread_info.state)) {
				aaudio_result_t res = AAudioStream_requestStart(u->stream);
				if (res == AAUDIO_OK) {
					pa_log("AAudio stream start requested after reconnect");
				} else {
					pa_log("AAudioStream_requestStart() failed after reconnect: %d", res);
				}
			} else {
				pa_log("Sink not in opened state, skipping stream start");
			}
		} else {
			pa_log("Failed to recreate AAudio stream during reconnect");
		}
		return 0;
	}
	
    return pa_sink_process_msg(o, code, data, offset, memchunk);
};

static int sink_set_state_io_thread(pa_sink *s, pa_sink_state_t state, pa_suspend_cause_t suspend_cause) {
    struct userdata *u = s->userdata;
    aaudio_result_t res;

    if (PA_SINK_IS_OPENED(s->thread_info.state) && state == PA_SINK_UNLINKED) {
		res = AAudioStream_requestStop(u->stream);
		if (res != AAUDIO_OK) {
			pa_log("AAudioStream_requestStop() failed: %d", res);
		} else {
			pa_log("AAudio stream stopped for unlink");
		}
    } 
	else if (s->thread_info.state == PA_SINK_SUSPENDED && PA_SINK_IS_OPENED(state)) {
		aaudio_stream_state_t stream_state = AAudioStream_getState(u->stream);
		int32_t current_device_id = AAudioStream_getDeviceId(u->stream);
		
		pa_log("AAudio stream resuming from suspended state, stream state: %d, device ID: %d", 
		       stream_state, current_device_id);
		
		if (stream_state == AAUDIO_STREAM_STATE_DISCONNECTED || 
		    stream_state == AAUDIO_STREAM_STATE_UNKNOWN ||
		    stream_state == AAUDIO_STREAM_STATE_UNINITIALIZED) {
			pa_log("AAudio stream disconnected/invalid (state: %d), recreating for device change", stream_state);
			if (pa_recreate_aaudio_stream(u) < 0) {
				pa_log("Failed to recreate AAudio stream during resume");
				return -1;
			}
			res = AAudioStream_requestStart(u->stream);
			if (res != AAUDIO_OK) {
				pa_log("AAudioStream_requestStart() failed after recreation: %d", res);
				return -1;
			}
			
			int32_t new_device_id = AAudioStream_getDeviceId(u->stream);
			pa_log("AAudio stream recreated and started (device ID: %d → %d)", 
			       current_device_id, new_device_id);
		} else if (stream_state != AAUDIO_STREAM_STATE_STARTED && 
		           stream_state != AAUDIO_STREAM_STATE_STARTING) {
			pa_log("AAudio stream in unexpected state %d, recreating", stream_state);
			if (pa_recreate_aaudio_stream(u) < 0) {
				pa_log("Failed to recreate AAudio stream");
				return -1;
			}
			res = AAudioStream_requestStart(u->stream);
			if (res != AAUDIO_OK) {
				pa_log("AAudioStream_requestStart() failed: %d", res);
				return -1;
			}
			pa_log("AAudio stream recreated and started");
		} else {
			pa_log("AAudio stream still valid, continuing with existing stream");
		}
    }
	else if (s->thread_info.state == PA_SINK_INIT && PA_SINK_IS_OPENED(state)) {
		aaudio_stream_state_t stream_state = AAudioStream_getState(u->stream);
		pa_log("AAudio stream initial start, state: %d", stream_state);
		
		if (stream_state == AAUDIO_STREAM_STATE_OPEN || 
		    stream_state == AAUDIO_STREAM_STATE_STOPPED) {
			res = AAudioStream_requestStart(u->stream);
			if (res != AAUDIO_OK) {
				pa_log("AAudioStream_requestStart() failed: %d", res);
				return -1;
			}
			pa_log("AAudio stream started");
		}
    }
	
    return 0;
}

static void thread_func(void *userdata) {
    struct userdata *u = userdata;
    pa_thread_mq_install(&u->thread_mq);

    for (;;) {
        if (PA_UNLIKELY(u->sink->thread_info.rewind_requested)) pa_sink_process_rewind(u->sink, 0);

		int res = pa_rtpoll_run(u->rtpoll);
        if (res < 0) {
			goto error;
		}
		else if (res == 0) break;
    }

error:
    pa_asyncmsgq_post(u->thread_mq.outq, PA_MSGOBJECT(u->core), PA_CORE_MESSAGE_UNLOAD_MODULE, u->module, 0, NULL, NULL);
    pa_asyncmsgq_wait_for(u->thread_mq.inq, PA_MESSAGE_SHUTDOWN);
}

void pa__done(pa_module* m) {
    struct userdata *u;
	
    if (!(u = m->userdata)) return;
	
    if (u->sink) {
		pa_sink_unlink(u->sink);
		pa_sink_unref(u->sink);
	}
	
	if (u->stream) AAudioStream_close(u->stream);
    if (u->rtpoll_item) pa_rtpoll_item_free(u->rtpoll_item);
    if (u->aaudio_msgq) pa_asyncmsgq_unref(u->aaudio_msgq);	
	
	pa_xfree(u);
}

int pa__init(pa_module* m) {
	struct userdata *u = NULL;
	pa_modargs *ma = NULL;
	
    if (!(ma = pa_modargs_new(m->argument, valid_modargs))) {
        pa_log("Failed to parse module arguments.");
		goto error;
    }		
	
	m->userdata = u = pa_xnew0(struct userdata, 1);
	
    u->core = m->core;
    u->module = m;	
    u->rtpoll = pa_rtpoll_new();

    if (pa_thread_mq_init(&u->thread_mq, m->core->mainloop, u->rtpoll) < 0) {
        pa_log("pa_thread_mq_init() failed.");
        goto error;
    }
	
    u->aaudio_msgq = pa_asyncmsgq_new(0);
    if (!u->aaudio_msgq) {
        pa_log("pa_asyncmsgq_new() failed.");
        goto error;
    }
	
	u->rtpoll_item = pa_rtpoll_item_new_asyncmsgq_read(u->rtpoll, PA_RTPOLL_EARLY-1, u->aaudio_msgq);
	
    u->ss = m->core->default_sample_spec;
    pa_channel_map map = m->core->default_channel_map;
	
    if (pa_modargs_get_sample_spec_and_channel_map(ma, &u->ss, &map, PA_CHANNEL_MAP_DEFAULT) < 0) {
        pa_log("pa_modargs_get_sample_spec_and_channel_map() failed.");
        goto error;
    }
	
	u->ss.channels = 2;
    u->ss.format = u->ss.format == PA_SAMPLE_FLOAT32LE || u->ss.format == PA_SAMPLE_FLOAT32BE ? PA_SAMPLE_FLOAT32LE : PA_SAMPLE_S16LE;
    
    u->volume = 1.0;
    u->performance_mode = AAUDIO_PERFORMANCE_MODE_LOW_LATENCY;
    
    double volume = 0.0;
    if (!pa_modargs_get_value_double(ma, "volume", &volume)) u->volume = volume;
    
    int performance_mode = 0;
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
	
    if (pa_create_aaudio_stream(u) < 0) goto error;
	
	pa_channel_map_init_stereo(&map);
	if (u->ss.channels != map.channels) {
		pa_channel_map_init_extend(&map, u->ss.channels, PA_CHANNEL_MAP_DEFAULT);
	}
	
	pa_sink_new_data data;
    pa_sink_new_data_init(&data);
    data.driver = __FILE__;
    data.module = m;
    pa_sink_new_data_set_name(&data, pa_modargs_get_value(ma, "sink_name", DEFAULT_SINK_NAME));
    pa_sink_new_data_set_sample_spec(&data, &u->ss);
    pa_sink_new_data_set_alternate_sample_rate(&data, u->ss.rate);
    pa_sink_new_data_set_channel_map(&data, &map);
    
    if (u->volume != 1.0) {
        pa_cvolume cvol;
        pa_cvolume_set(&cvol, u->ss.channels, u->volume * PA_VOLUME_NORM);
        pa_sink_new_data_set_volume(&data, &cvol);
    }
    
    pa_proplist_sets(data.proplist, PA_PROP_DEVICE_DESCRIPTION, _("AAudio Output"));
    pa_proplist_sets(data.proplist, PA_PROP_DEVICE_CLASS, "abstract");
	
    if (pa_modargs_get_proplist(ma, "sink_properties", data.proplist, PA_UPDATE_REPLACE) < 0) {
        pa_log("pa_modargs_get_proplist() failed.");
        pa_sink_new_data_done(&data);
        goto error;
    }
	
    u->sink = pa_sink_new(m->core, &data, PA_SINK_HARDWARE);
    pa_sink_new_data_done(&data);

    if (!u->sink) {
        pa_log("Failed to create sink object.");
        goto error;
    }
	
    u->sink->parent.process_msg = sink_process_msg;
    u->sink->set_state_in_io_thread = sink_set_state_io_thread;
    u->sink->userdata = u;

    pa_sink_set_asyncmsgq(u->sink, u->thread_mq.inq);
    pa_sink_set_rtpoll(u->sink, u->rtpoll);
    update_pa_latency(u);

    if (!(u->thread = pa_thread_new("aaudio-sink", thread_func, u))) {
        pa_log("Failed to create thread.");
        goto error;
    }	
	
    pa_sink_put(u->sink);
	pa_core_set_configured_default_sink(m->core, u->sink->name);
    pa_modargs_free(ma);	
	return 0;
error:
    if (ma) pa_modargs_free(ma);
	pa__done(m);
	return -1;	
}