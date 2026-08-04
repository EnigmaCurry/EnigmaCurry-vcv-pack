/*
 * TrackerHost — headless, JS-programmed wrapper around Biset Tracker.
 * Copyright (C) 2026 EnigmaCurry — GPL-3.0-or-later
 *
 * Subclasses Biset's Tracker so that the Biset T-Synth / T-Drum / T-Clock /
 * T-Phase / T-Quant / T-State modules can bind to `g_timeline` / `g_editor`
 * / `g_module` exactly as they would with a real Biset Tracker in the patch.
 * The tracker's usual pattern-editor UI is dropped; playback is driven from
 * JavaScript via the `tracker_host_*` extern "C" API below.
 *
 * Attribution:
 *   Biset — Copyright (C) Pyer — GPL-3.0-or-later
 *   https://github.com/pyer/rack-Biset
 */

#include "plugin.hpp"
#include "../../Biset/src/Tracker/Tracker.hpp"
#include <atomic>
#include <cstring>

#ifdef __EMSCRIPTEN__
# include <emscripten.h>
# define TRACKER_HOST_EXPORT EMSCRIPTEN_KEEPALIVE
#else
# define TRACKER_HOST_EXPORT
#endif

// ---------------------------------------------------------------------------
// Shared state block published by the DSP thread, polled by JS.
//   playing:         0/1 (g_timeline->play != STOP)
//   play_mode:       TIMELINE_MODE_* enum value
//   current_beat:    g_timeline->clock.beat (integer beat since play start)
//   current_phase:   [0..1) fractional phase within the current beat
//   current_pattern: g_editor->pattern_id, or -1 in song mode
//   song_length:     length in beats of the whole timeline (compute_length())
//   pattern_count:   number of patterns currently loaded
//   synth_count:     number of synths currently loaded
// alignas(16) so a Float32Array/Uint32Array view lands on a natural boundary.
// ---------------------------------------------------------------------------
struct TrackerHostState {
    uint32_t playing;
    uint32_t play_mode;
    uint32_t current_beat;
    float    current_phase;
    int32_t  current_pattern;
    uint32_t song_length;
    uint32_t pattern_count;
    uint32_t synth_count;
};
alignas(16) TrackerHostState g_tracker_host_state = {};

// ---------------------------------------------------------------------------
// Module class.
//
// Inheriting Tracker gives us:
//   - all PARAM_* configured (PARAM_BPM in particular, used by process())
//   - dataToJson / dataFromJson patch persistence via Tracker's overrides
//   - the base constructor sets g_module = this and news up g_timeline /
//     g_editor if they don't exist yet
//   - the base destructor cleans up g_timeline / g_editor
//
// We override process() to skip Editor::process (UI-only work that touches
// APP->window) and instead run just the timeline advance, plus one-shot
// end-of-song / end-of-pattern detection and state-block publishing.
// ---------------------------------------------------------------------------
struct EnigmaCurryTrackerHost : Tracker {
    uint32_t last_beat = 0;
    bool     one_shot_active = false;   // set by play_song() / play_pattern(),
                                        // cleared on wrap or by loop_* / stop()

    EnigmaCurryTrackerHost() : Tracker() {}

    void process(const ProcessArgs& args) override {
        // Only one Tracker owns g_timeline at a time. If a real Biset Tracker
        // (or another host) grabbed it first, silently no-op — we're inert.
        if (g_module != this) return;
        if (g_timeline == NULL) return;

        const float bpm     = params[Tracker::PARAM_BPM].getValue();
        const float dt_sec  = args.sampleTime;
        const float dt_beat = (bpm * dt_sec) / 60.0f;

        g_timeline->process(args.frame, dt_sec, dt_beat);

        // One-shot end detection. SONG mode natively wraps the clock at the
        // end of the timeline (see Biset Timeline.cpp:137); PATTERN_SOLO
        // wraps at pattern->beat_count. In both cases the beat counter
        // decreases across the boundary — detect that and stop.
        if (one_shot_active && g_timeline->play != TIMELINE_MODE_STOP) {
            const uint32_t beat = g_timeline->clock.beat;
            if (beat < last_beat) {
                g_timeline->stop();
                g_timeline->play = TIMELINE_MODE_STOP;
                g_timeline->stop_trigger.trigger(0.01f);
                one_shot_active = false;
            }
            last_beat = beat;
        } else {
            last_beat = g_timeline->clock.beat;
        }

        // Publish state block. Single-writer here / single-reader from JS —
        // no atomic ops required.
        g_tracker_host_state.playing         = (g_timeline->play != TIMELINE_MODE_STOP) ? 1u : 0u;
        g_tracker_host_state.play_mode       = (uint32_t)g_timeline->play;
        g_tracker_host_state.current_beat    = g_timeline->clock.beat;
        g_tracker_host_state.current_phase   = g_timeline->clock.phase;
        g_tracker_host_state.current_pattern = g_editor ? g_editor->pattern_id : -1;
        g_tracker_host_state.song_length     = (uint32_t)g_timeline->timeline_length;
        g_tracker_host_state.pattern_count   = (uint32_t)g_timeline->pattern_count;
        g_tracker_host_state.synth_count     = (uint32_t)g_timeline->synth_count;
    }
    // dataToJson / dataFromJson are inherited from Tracker.
};

// Helpers so the C API can find our subclass instance without carrying a
// separate global — g_module is already a Tracker*, and our subclass is-a
// Tracker, so downcast is safe.
static EnigmaCurryTrackerHost* host() {
    return dynamic_cast<EnigmaCurryTrackerHost*>(g_module);
}

// Reset one_shot_active on any explicit playback command so a subsequent
// loop_* / stop() doesn't get surprise-terminated by a stale wrap detection.
static void begin_command_locked() {
    while (g_timeline->thread_flag.test_and_set()) {}
}
static void end_command_locked() {
    g_timeline->thread_flag.clear();
}

// ---------------------------------------------------------------------------
// extern "C" API — callable from JS via the CardinalWasmDSP export list.
// ---------------------------------------------------------------------------
extern "C" {

TRACKER_HOST_EXPORT void tracker_host_reset(void) {
    if (g_timeline == NULL) return;
    begin_command_locked();
    g_timeline->stop();
    g_timeline->play = TIMELINE_MODE_STOP;
    g_timeline->clear();
    EnigmaCurryTrackerHost* h = host();
    if (h) h->one_shot_active = false;
    end_command_locked();
}

TRACKER_HOST_EXPORT int tracker_host_load_track(const char* json, int len) {
    if (g_module == NULL || json == NULL || len <= 0) return -1;
    json_error_t err;
    json_t* root = json_loadb(json, (size_t)len, 0, &err);
    if (!root) return -2;
    // Tracker::dataFromJson does its own thread_flag acquire and calls
    // g_timeline->clear() before repopulating, so no extra lock needed here.
    g_module->dataFromJson(root);
    json_decref(root);
    return 0;
}

// Song playback — one-shot. Wrap detection in process() will stop it.
TRACKER_HOST_EXPORT void tracker_host_play_song(void) {
    if (g_timeline == NULL) return;
    begin_command_locked();
    if (g_timeline->play != TIMELINE_MODE_STOP)
        g_timeline->stop_trigger.trigger(0.01f);
    g_timeline->stop();
    g_timeline->clock.reset();
    g_timeline->compute_length();
    g_timeline->play_trigger.trigger(0.01f);
    g_timeline->play = TIMELINE_MODE_PLAY_SONG;
    EnigmaCurryTrackerHost* h = host();
    if (h) { h->one_shot_active = true; h->last_beat = 0; }
    end_command_locked();
}

// Song playback — loops natively (Timeline resets clock at end).
TRACKER_HOST_EXPORT void tracker_host_loop_song(void) {
    if (g_timeline == NULL) return;
    begin_command_locked();
    if (g_timeline->play != TIMELINE_MODE_STOP)
        g_timeline->stop_trigger.trigger(0.01f);
    g_timeline->stop();
    g_timeline->clock.reset();
    g_timeline->compute_length();
    g_timeline->play_trigger.trigger(0.01f);
    g_timeline->play = TIMELINE_MODE_PLAY_SONG;
    EnigmaCurryTrackerHost* h = host();
    if (h) h->one_shot_active = false;
    end_command_locked();
}

// Pattern playback — one-shot.
TRACKER_HOST_EXPORT void tracker_host_play_pattern(int idx) {
    if (g_timeline == NULL || g_editor == NULL) return;
    begin_command_locked();
    if (g_timeline->play != TIMELINE_MODE_STOP)
        g_timeline->stop_trigger.trigger(0.01f);
    g_timeline->stop();
    g_timeline->clock.reset();
    g_editor->set_pattern(idx);
    if (g_editor->pattern) {
        g_timeline->compute_length();
        g_timeline->play_trigger.trigger(0.01f);
        g_timeline->play = TIMELINE_MODE_PLAY_PATTERN_SOLO;
        EnigmaCurryTrackerHost* h = host();
        if (h) { h->one_shot_active = true; h->last_beat = 0; }
    }
    end_command_locked();
}

// Pattern playback — loops (PATTERN_SOLO mode natively resets clock at end).
TRACKER_HOST_EXPORT void tracker_host_loop_pattern(int idx) {
    if (g_timeline == NULL || g_editor == NULL) return;
    begin_command_locked();
    if (g_timeline->play != TIMELINE_MODE_STOP)
        g_timeline->stop_trigger.trigger(0.01f);
    g_timeline->stop();
    g_timeline->clock.reset();
    g_editor->set_pattern(idx);
    if (g_editor->pattern) {
        g_timeline->compute_length();
        g_timeline->play_trigger.trigger(0.01f);
        g_timeline->play = TIMELINE_MODE_PLAY_PATTERN_SOLO;
        EnigmaCurryTrackerHost* h = host();
        if (h) h->one_shot_active = false;
    }
    end_command_locked();
}

TRACKER_HOST_EXPORT void tracker_host_stop(void) {
    if (g_timeline == NULL) return;
    begin_command_locked();
    g_timeline->stop();
    g_timeline->play = TIMELINE_MODE_STOP;
    g_timeline->stop_trigger.trigger(0.01f);
    EnigmaCurryTrackerHost* h = host();
    if (h) h->one_shot_active = false;
    end_command_locked();
}

TRACKER_HOST_EXPORT void tracker_host_set_bpm(float bpm) {
    if (g_module == NULL) return;
    if (bpm < 30.f)  bpm = 30.f;
    if (bpm > 300.f) bpm = 300.f;
    g_module->params[Tracker::PARAM_BPM].setValue(bpm);
}

TRACKER_HOST_EXPORT float tracker_host_get_bpm(void) {
    if (g_module == NULL) return 0.f;
    return g_module->params[Tracker::PARAM_BPM].getValue();
}

TRACKER_HOST_EXPORT void tracker_host_live_note_on(int pitch, int velocity) {
    if (g_timeline == NULL || g_editor == NULL) return;
    if (pitch < 0 || pitch > 127) return;
    if (velocity < 0)   velocity = 0;
    if (velocity > 127) velocity = 127;
    begin_command_locked();
    g_editor->live_play(pitch, velocity);
    end_command_locked();
}

TRACKER_HOST_EXPORT void tracker_host_live_note_off(int pitch) {
    if (g_timeline == NULL || g_editor == NULL) return;
    if (pitch < 0 || pitch > 127) return;
    begin_command_locked();
    g_editor->live_stop(pitch);
    end_command_locked();
}

TRACKER_HOST_EXPORT void* tracker_host_get_state_ptr(void) {
    return (void*)&g_tracker_host_state;
}

TRACKER_HOST_EXPORT int tracker_host_state_size(void) {
    return (int)sizeof(TrackerHostState);
}

} // extern "C"

// ---------------------------------------------------------------------------
// Widget — deliberately blank. No ports; T-* modules bind via `g_timeline`
// globals, not cables.
// ---------------------------------------------------------------------------
struct EnigmaCurryTrackerHostWidget : ModuleWidget {
    EnigmaCurryTrackerHostWidget(EnigmaCurryTrackerHost* module) {
        setModule(module);
        setPanel(APP->window->loadSvg(asset::plugin(pluginInstance, "res/3hp.svg")));
    }
};

Model* modelEnigmaCurryTrackerHost =
    createModel<EnigmaCurryTrackerHost, EnigmaCurryTrackerHostWidget>("TrackerHost");
