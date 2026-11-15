/* audio.c - single-file CPython extension.
 * Full miniaudio + minimp3 implementation.
 * 21 high-level APIs + ~1200 ma_* / mp3dec_* via PyCapsule (1:1).
 *
 * License: MIT-0.
 */
#define PY_SSIZE_T_CLEAN
#include <Python.h>

#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"

#define MINIMP3_IMPLEMENTATION
#include "minimp3.h"
#include "minimp3_ex.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ============================================================ */
/* 21 high-level APIs                                            */
/* ============================================================ */
#define AUDIO_MAX_SOUNDS 64

static void audio__sleep_ms(unsigned ms) {
#ifdef _WIN32
    Sleep(ms);
#else
    struct timespec ts; ts.tv_sec = ms/1000; ts.tv_nsec = (long)(ms%1000)*1000000L;
    nanosleep(&ts, NULL);
#endif
}

static int audio__decode_decoder(ma_decoder* pDec, void** ppPCM, uint64_t* pFrames,
                                 uint32_t* pChannels, uint32_t* pSampleRate) {
    const ma_uint64 chunk = 4096;
    const size_t bpf = 4;
    size_t cap = 0, used = 0;
    unsigned char* out = NULL;
    ma_int16* tmp = (ma_int16*)malloc((size_t)chunk * bpf);
    if (!tmp) return -1;
    for (;;) {
        ma_uint64 got = 0;
        ma_result r = ma_decoder_read_pcm_frames(pDec, tmp, chunk, &got);
        if (r != MA_SUCCESS || got == 0) break;
        size_t add = (size_t)got * bpf;
        if (used + add > cap) {
            size_t nc = cap ? cap*2 : 65536;
            while (nc < used+add) nc *= 2;
            unsigned char* nb = (unsigned char*)realloc(out, nc);
            if (!nb) { free(tmp); free(out); return -1; }
            out = nb; cap = nc;
        }
        memcpy(out+used, tmp, add); used += add;
    }
    free(tmp);
    {
        ma_format fmt = ma_format_s16; ma_uint32 ch = 2; ma_uint32 sr = 44100;
        if (ma_decoder_get_data_format(pDec,&fmt,&ch,&sr,NULL,0) == MA_SUCCESS) {
            if (pChannels) *pChannels = ch;
            if (pSampleRate) *pSampleRate = sr;
        } else {
            if (pChannels) *pChannels = 2;
            if (pSampleRate) *pSampleRate = 44100;
        }
    }
    *ppPCM = out; *pFrames = (uint64_t)(used / bpf);
    return 0;
}

static int audio_decode_file_w(const wchar_t* path, void** ppPCM, uint64_t* pFrames,
                               uint32_t* pChannels, uint32_t* pSampleRate) {
    if (!path || !ppPCM || !pFrames) return -1;
    *ppPCM = NULL; *pFrames = 0;
    if (pChannels) *pChannels = 0; if (pSampleRate) *pSampleRate = 0;
    ma_decoder_config cfg = ma_decoder_config_init_default();
    cfg.format = ma_format_s16; cfg.channels = 2; cfg.sampleRate = 0;
    ma_decoder dec;
    ma_result r = ma_decoder_init_file_w(path, &cfg, &dec);
    if (r != MA_SUCCESS) return (int)r;
    int rc = audio__decode_decoder(&dec, ppPCM, pFrames, pChannels, pSampleRate);
    ma_decoder_uninit(&dec);
    return rc;
}

static int audio_decode_memory(const void* data, size_t size, void** ppPCM, uint64_t* pFrames,
                               uint32_t* pChannels, uint32_t* pSampleRate) {
    if (!data || !size || !ppPCM || !pFrames) return -1;
    *ppPCM = NULL; *pFrames = 0;
    if (pChannels) *pChannels = 0; if (pSampleRate) *pSampleRate = 0;
    ma_decoder_config cfg = ma_decoder_config_init_default();
    cfg.format = ma_format_s16; cfg.channels = 2; cfg.sampleRate = 0;
    ma_decoder dec;
    ma_result r = ma_decoder_init_memory(data, size, &cfg, &dec);
    if (r != MA_SUCCESS) return (int)r;
    int rc = audio__decode_decoder(&dec, ppPCM, pFrames, pChannels, pSampleRate);
    ma_decoder_uninit(&dec);
    return rc;
}

static int audio_play_file_w(const wchar_t* path) {
    ma_engine engine; ma_result r = ma_engine_init(NULL, &engine);
    if (r != MA_SUCCESS) return (int)r;
    ma_sound s;
    r = ma_sound_init_from_file_w(&engine, path, 0, NULL, NULL, &s);
    if (r != MA_SUCCESS) { ma_engine_uninit(&engine); return (int)r; }
    r = ma_sound_start(&s);
    if (r != MA_SUCCESS) { ma_sound_uninit(&s); ma_engine_uninit(&engine); return (int)r; }
    while (!ma_sound_at_end(&s)) audio__sleep_ms(50);
    ma_sound_uninit(&s); ma_engine_uninit(&engine);
    return 0;
}

typedef struct { ma_sound sound; int inited; } audio__slot;
static ma_engine g_engine; static int g_engine_inited = 0;
static audio__slot g_slots[AUDIO_MAX_SOUNDS];

static int audio_engine_init(void) {
    if (g_engine_inited) return 0;
    ma_result r = ma_engine_init(NULL, &g_engine);
    if (r != MA_SUCCESS) return (int)r;
    g_engine_inited = 1; return 0;
}
static void audio_engine_uninit(void) {
    for (int i = 0; i < AUDIO_MAX_SOUNDS; i++) {
        if (g_slots[i].inited) {
            ma_sound_stop(&g_slots[i].sound);
            ma_sound_uninit(&g_slots[i].sound);
            g_slots[i].inited = 0;
        }
    }
    if (g_engine_inited) { ma_engine_uninit(&g_engine); g_engine_inited = 0; }
}
static int audio_sound_load_w(const wchar_t* path) {
    if (!g_engine_inited) return -1;
    for (int i = 0; i < AUDIO_MAX_SOUNDS; i++) {
        if (!g_slots[i].inited) {
            ma_result r = ma_sound_init_from_file_w(&g_engine, path, 0, NULL, NULL, &g_slots[i].sound);
            if (r != MA_SUCCESS) return -1;
            g_slots[i].inited = 1; return i;
        }
    }
    return -1;
}
static int audio_sound_play(int id) {
    if (id<0||id>=AUDIO_MAX_SOUNDS||!g_slots[id].inited) return -1;
    ma_result r = ma_sound_start(&g_slots[id].sound);
    return (r==MA_SUCCESS)?0:(int)r;
}
static int audio_sound_stop(int id) {
    if (id<0||id>=AUDIO_MAX_SOUNDS||!g_slots[id].inited) return -1;
    ma_sound_stop(&g_slots[id].sound); return 0;
}
static int audio_sound_pause(int id) { return audio_sound_stop(id); }
static int audio_sound_resume(int id) {
    if (id<0||id>=AUDIO_MAX_SOUNDS||!g_slots[id].inited) return -1;
    ma_result r = ma_sound_start(&g_slots[id].sound);
    return (r==MA_SUCCESS)?0:(int)r;
}
static void audio_sound_unload(int id) {
    if (id<0||id>=AUDIO_MAX_SOUNDS||!g_slots[id].inited) return;
    ma_sound_stop(&g_slots[id].sound);
    ma_sound_uninit(&g_slots[id].sound);
    g_slots[id].inited = 0;
}
static int audio_sound_set_volume(int id, float v) {
    if (id<0||id>=AUDIO_MAX_SOUNDS||!g_slots[id].inited) return -1;
    ma_sound_set_volume(&g_slots[id].sound, v); return 0;
}
static float audio_sound_get_volume(int id) {
    if (id<0||id>=AUDIO_MAX_SOUNDS||!g_slots[id].inited) return 0.0f;
    return ma_sound_get_volume(&g_slots[id].sound);
}
static int audio_sound_set_looping(int id, int lp) {
    if (id<0||id>=AUDIO_MAX_SOUNDS||!g_slots[id].inited) return -1;
    ma_sound_set_looping(&g_slots[id].sound, lp ? MA_TRUE : MA_FALSE); return 0;
}
static int audio_sound_is_playing(int id) {
    if (id<0||id>=AUDIO_MAX_SOUNDS||!g_slots[id].inited) return 0;
    return ma_sound_is_playing(&g_slots[id].sound) ? 1 : 0;
}
static int audio_sound_seek(int id, float sec) {
    if (id<0||id>=AUDIO_MAX_SOUNDS||!g_slots[id].inited) return -1;
    ma_result r = ma_sound_seek_to_second(&g_slots[id].sound, sec);
    return (r==MA_SUCCESS)?0:(int)r;
}
static float audio_sound_get_duration(int id) {
    float l = 0.0f;
    if (id<0||id>=AUDIO_MAX_SOUNDS||!g_slots[id].inited) return 0.0f;
    ma_sound_get_length_in_seconds(&g_slots[id].sound, &l);
    return l;
}
static float audio_sound_get_position(int id) {
    float p = 0.0f;
    if (id<0||id>=AUDIO_MAX_SOUNDS||!g_slots[id].inited) return 0.0f;
    ma_sound_get_cursor_in_seconds(&g_slots[id].sound, &p);
    return p;
}
static void audio_engine_set_volume(float v) { if (g_engine_inited) ma_engine_set_volume(&g_engine, v); }
static void audio_engine_stop_all(void) {
    for (int i = 0; i < AUDIO_MAX_SOUNDS; i++) if (g_slots[i].inited) ma_sound_stop(&g_slots[i].sound);
}

/* ============================================================ */
/* Python wrappers for 21 high-level APIs                       */
/* ============================================================ */
static wchar_t* path_to_wpath(PyObject* o) { return PyUnicode_AsWideCharString(o, NULL); }

static PyObject* py_version(PyObject* s, PyObject* a) { (void)s;(void)a;
    return PyUnicode_FromString(ma_version_string()); }

static PyObject* py_decode_file(PyObject* s, PyObject* a) { (void)s;
    PyObject* o; if (!PyArg_ParseTuple(a,"O",&o)) return NULL;
    wchar_t* w = path_to_wpath(o); if (!w) return NULL;
    void* pcm=NULL; uint64_t fr=0; uint32_t ch=0,sr=0;
    int rc = audio_decode_file_w(w,&pcm,&fr,&ch,&sr); PyMem_Free(w);
    if (rc) { PyErr_Format(PyExc_RuntimeError,"decode_file: %d",rc); return NULL; }
    PyObject* b = PyBytes_FromStringAndSize((const char*)pcm,(Py_ssize_t)(fr*ch*2));
    free(pcm); if (!b) return NULL;
    PyObject* r = Py_BuildValue("(OKKI)",b,(unsigned long long)fr,(unsigned)ch,(unsigned)sr);
    Py_DECREF(b); return r;
}

static PyObject* py_decode_memory(PyObject* s, PyObject* a) { (void)s;
    Py_buffer buf; if (!PyArg_ParseTuple(a,"y*",&buf)) return NULL;
    void* pcm=NULL; uint64_t fr=0; uint32_t ch=0,sr=0;
    int rc = audio_decode_memory(buf.buf,(size_t)buf.len,&pcm,&fr,&ch,&sr);
    PyBuffer_Release(&buf);
    if (rc) { PyErr_Format(PyExc_RuntimeError,"decode_memory: %d",rc); return NULL; }
    PyObject* b = PyBytes_FromStringAndSize((const char*)pcm,(Py_ssize_t)(fr*ch*2));
    free(pcm); if (!b) return NULL;
    PyObject* r = Py_BuildValue("(OKKI)",b,(unsigned long long)fr,(unsigned)ch,(unsigned)sr);
    Py_DECREF(b); return r;
}

static PyObject* py_play_file(PyObject* s, PyObject* a) { (void)s;
    PyObject* o; if (!PyArg_ParseTuple(a,"O",&o)) return NULL;
    wchar_t* w = path_to_wpath(o); if (!w) return NULL;
    int rc; Py_BEGIN_ALLOW_THREADS rc = audio_play_file_w(w); Py_END_ALLOW_THREADS
    PyMem_Free(w);
    if (rc) { PyErr_Format(PyExc_RuntimeError,"play_file: %d",rc); return NULL; }
    Py_RETURN_NONE;
}

static PyObject* py_engine_init(PyObject* s, PyObject* a) { (void)s;(void)a;
    int rc = audio_engine_init();
    if (rc) { PyErr_Format(PyExc_RuntimeError,"engine_init: %d",rc); return NULL; }
    Py_RETURN_NONE;
}
static PyObject* py_engine_uninit(PyObject* s, PyObject* a) { (void)s;(void)a;
    audio_engine_uninit(); Py_RETURN_NONE; }

static PyObject* py_sound_load(PyObject* s, PyObject* a) { (void)s;
    PyObject* o; if (!PyArg_ParseTuple(a,"O",&o)) return NULL;
    wchar_t* w = path_to_wpath(o); if (!w) return NULL;
    int id = audio_sound_load_w(w); PyMem_Free(w);
    if (id<0) { PyErr_SetString(PyExc_RuntimeError,"sound_load failed"); return NULL; }
    return PyLong_FromLong(id);
}
static PyObject* py_sound_play(PyObject* s, PyObject* a) { (void)s;
    int id; if (!PyArg_ParseTuple(a,"i",&id)) return NULL;
    if (audio_sound_play(id)) { PyErr_SetString(PyExc_RuntimeError,"play"); return NULL; }
    Py_RETURN_NONE; }
static PyObject* py_sound_stop(PyObject* s, PyObject* a) { (void)s;
    int id; if (!PyArg_ParseTuple(a,"i",&id)) return NULL;
    audio_sound_stop(id); Py_RETURN_NONE; }
static PyObject* py_sound_pause(PyObject* s, PyObject* a) { (void)s;
    int id; if (!PyArg_ParseTuple(a,"i",&id)) return NULL;
    audio_sound_pause(id); Py_RETURN_NONE; }
static PyObject* py_sound_resume(PyObject* s, PyObject* a) { (void)s;
    int id; if (!PyArg_ParseTuple(a,"i",&id)) return NULL;
    audio_sound_resume(id); Py_RETURN_NONE; }
static PyObject* py_sound_unload(PyObject* s, PyObject* a) { (void)s;
    int id; if (!PyArg_ParseTuple(a,"i",&id)) return NULL;
    audio_sound_unload(id); Py_RETURN_NONE; }
static PyObject* py_sound_set_volume(PyObject* s, PyObject* a) { (void)s;
    int id; float v; if (!PyArg_ParseTuple(a,"if",&id,&v)) return NULL;
    audio_sound_set_volume(id,v); Py_RETURN_NONE; }
static PyObject* py_sound_get_volume(PyObject* s, PyObject* a) { (void)s;
    int id; if (!PyArg_ParseTuple(a,"i",&id)) return NULL;
    return PyFloat_FromDouble(audio_sound_get_volume(id)); }
static PyObject* py_sound_set_looping(PyObject* s, PyObject* a) { (void)s;
    int id, lp; if (!PyArg_ParseTuple(a,"ip",&id,&lp)) return NULL;
    audio_sound_set_looping(id,lp); Py_RETURN_NONE; }
static PyObject* py_sound_is_playing(PyObject* s, PyObject* a) { (void)s;
    int id; if (!PyArg_ParseTuple(a,"i",&id)) return NULL;
    return PyBool_FromLong(audio_sound_is_playing(id)); }
static PyObject* py_sound_seek(PyObject* s, PyObject* a) { (void)s;
    int id; float sec; if (!PyArg_ParseTuple(a,"if",&id,&sec)) return NULL;
    audio_sound_seek(id,sec); Py_RETURN_NONE; }
static PyObject* py_sound_get_duration(PyObject* s, PyObject* a) { (void)s;
    int id; if (!PyArg_ParseTuple(a,"i",&id)) return NULL;
    return PyFloat_FromDouble(audio_sound_get_duration(id)); }
static PyObject* py_sound_get_position(PyObject* s, PyObject* a) { (void)s;
    int id; if (!PyArg_ParseTuple(a,"i",&id)) return NULL;
    return PyFloat_FromDouble(audio_sound_get_position(id)); }
static PyObject* py_engine_set_volume(PyObject* s, PyObject* a) { (void)s;
    float v; if (!PyArg_ParseTuple(a,"f",&v)) return NULL;
    audio_engine_set_volume(v); Py_RETURN_NONE; }
static PyObject* py_engine_stop_all(PyObject* s, PyObject* a) { (void)s;(void)a;
    audio_engine_stop_all(); Py_RETURN_NONE; }

/* ============================================================ */
/* 1:1 PyCapsule access for all ma_* / mp3dec_*                 */
/* ============================================================ */
/* X-Macro: every miniaudio public API + minimp3 API. */
#define AUDIO_FN_LIST(FN) \
    FN(ma_version_string) \
    FN(ma_version) \
    FN(ma_strcpy_s) \
    FN(ma_wcscpy_s) \
    FN(ma_strncpy_s) \
    FN(ma_strcat_s) \
    FN(ma_strncat_s) \
    FN(ma_itoa_s) \
    FN(ma_strcmp) \
    FN(ma_wcscmp) \
    FN(ma_strappend) \
    FN(ma_wcslen) \
    FN(ma_copy_string) \
    FN(ma_copy_string_w) \
    FN(ma_fopen) \
    FN(ma_wfopen) \
    FN(ma_log_level_to_string) \
    FN(ma_log_callback_init) \
    FN(ma_log_init) \
    FN(ma_log_uninit) \
    FN(ma_log_register_callback) \
    FN(ma_log_unregister_callback) \
    FN(ma_log_post) \
    FN(ma_log_postv) \
    FN(ma_log_postf) \
    FN(ma_calculate_frame_count_after_resampling) \
    FN(ma_spinlock_lock) \
    FN(ma_spinlock_lock_noyield) \
    FN(ma_spinlock_unlock) \
    FN(ma_mutex_init) \
    FN(ma_mutex_uninit) \
    FN(ma_mutex_lock) \
    FN(ma_mutex_unlock) \
    FN(ma_event_init) \
    FN(ma_event_uninit) \
    FN(ma_event_wait) \
    FN(ma_event_signal) \
    FN(ma_semaphore_init) \
    FN(ma_semaphore_uninit) \
    FN(ma_semaphore_wait) \
    FN(ma_semaphore_release) \
    FN(ma_fence_init) \
    FN(ma_fence_uninit) \
    FN(ma_fence_acquire) \
    FN(ma_fence_release) \
    FN(ma_fence_wait) \
    FN(ma_async_notification_signal) \
    FN(ma_async_notification_poll_init) \
    FN(ma_async_notification_poll_is_signalled) \
    FN(ma_async_notification_event_init) \
    FN(ma_async_notification_event_uninit) \
    FN(ma_async_notification_event_wait) \
    FN(ma_async_notification_event_signal) \
    FN(ma_slot_allocator_config_init) \
    FN(ma_slot_allocator_get_heap_size) \
    FN(ma_slot_allocator_init_preallocated) \
    FN(ma_slot_allocator_init) \
    FN(ma_slot_allocator_uninit) \
    FN(ma_slot_allocator_alloc) \
    FN(ma_slot_allocator_free) \
    FN(ma_job_init) \
    FN(ma_job_process) \
    FN(ma_job_queue_config_init) \
    FN(ma_job_queue_get_heap_size) \
    FN(ma_job_queue_init_preallocated) \
    FN(ma_job_queue_init) \
    FN(ma_job_queue_uninit) \
    FN(ma_job_queue_post) \
    FN(ma_job_queue_next) \
    FN(ma_dlopen) \
    FN(ma_dlclose) \
    FN(ma_dlsym) \
    FN(ma_device_info_add_native_data_format) \
    FN(ma_get_backend_name) \
    FN(ma_get_backend_from_name) \
    FN(ma_is_backend_enabled) \
    FN(ma_get_enabled_backends) \
    FN(ma_is_loopback_supported) \
    FN(ma_strlen_WCHAR) \
    FN(ma_strcmp_WCHAR) \
    FN(ma_strcpy_s_WCHAR) \
    FN(ma_get_format_priority_index) \
    FN(ma_device_job_thread_config_init) \
    FN(ma_device_job_thread_init) \
    FN(ma_device_job_thread_uninit) \
    FN(ma_device_job_thread_post) \
    FN(ma_device_job_thread_next) \
    FN(ma_device_id_equal) \
    FN(ma_context_config_init) \
    FN(ma_context_init) \
    FN(ma_context_uninit) \
    FN(ma_context_sizeof) \
    FN(ma_context_get_log) \
    FN(ma_context_enumerate_devices) \
    FN(ma_context_get_devices) \
    FN(ma_context_get_device_info) \
    FN(ma_context_is_loopback_supported) \
    FN(ma_device_config_init) \
    FN(ma_device_get_context) \
    FN(ma_device_get_log) \
    FN(ma_device_get_info) \
    FN(ma_device_get_name) \
    FN(ma_device_start) \
    FN(ma_device_stop) \
    FN(ma_device_is_started) \
    FN(ma_device_get_state) \
    FN(ma_device_set_master_volume) \
    FN(ma_device_get_master_volume) \
    FN(ma_device_set_master_volume_db) \
    FN(ma_device_get_master_volume_db) \
    FN(ma_calculate_buffer_size_in_frames_from_descriptor) \
    FN(ma_calculate_buffer_size_in_milliseconds_from_frames) \
    FN(ma_calculate_buffer_size_in_frames_from_milliseconds) \
    FN(ma_copy_pcm_frames) \
    FN(ma_silence_pcm_frames) \
    FN(ma_offset_pcm_frames_ptr) \
    FN(ma_offset_pcm_frames_const_ptr) \
    FN(ma_clip_samples_u8) \
    FN(ma_clip_samples_s16) \
    FN(ma_clip_samples_s24) \
    FN(ma_clip_samples_s32) \
    FN(ma_clip_samples_f32) \
    FN(ma_clip_pcm_frames) \
    FN(ma_copy_and_apply_volume_factor_u8) \
    FN(ma_copy_and_apply_volume_factor_s16) \
    FN(ma_copy_and_apply_volume_factor_s24) \
    FN(ma_copy_and_apply_volume_factor_s32) \
    FN(ma_copy_and_apply_volume_factor_f32) \
    FN(ma_apply_volume_factor_u8) \
    FN(ma_apply_volume_factor_s16) \
    FN(ma_apply_volume_factor_s24) \
    FN(ma_apply_volume_factor_s32) \
    FN(ma_apply_volume_factor_f32) \
    FN(ma_copy_and_apply_volume_factor_pcm_frames_u8) \
    FN(ma_copy_and_apply_volume_factor_pcm_frames_s16) \
    FN(ma_copy_and_apply_volume_factor_pcm_frames_s24) \
    FN(ma_copy_and_apply_volume_factor_pcm_frames_s32) \
    FN(ma_copy_and_apply_volume_factor_pcm_frames_f32) \
    FN(ma_copy_and_apply_volume_factor_pcm_frames) \
    FN(ma_apply_volume_factor_pcm_frames_u8) \
    FN(ma_apply_volume_factor_pcm_frames_s16) \
    FN(ma_apply_volume_factor_pcm_frames_s24) \
    FN(ma_apply_volume_factor_pcm_frames_s32) \
    FN(ma_apply_volume_factor_pcm_frames_f32) \
    FN(ma_apply_volume_factor_pcm_frames) \
    FN(ma_copy_and_apply_volume_factor_per_channel_f32) \
    FN(ma_copy_and_apply_volume_and_clip_samples_u8) \
    FN(ma_copy_and_apply_volume_and_clip_samples_s16) \
    FN(ma_copy_and_apply_volume_and_clip_samples_s24) \
    FN(ma_copy_and_apply_volume_and_clip_samples_s32) \
    FN(ma_copy_and_apply_volume_and_clip_samples_f32) \
    FN(ma_copy_and_apply_volume_and_clip_pcm_frames) \
    FN(ma_volume_linear_to_db) \
    FN(ma_volume_db_to_linear) \
    FN(ma_mix_pcm_frames_f32) \
    FN(ma_pcm_u8_to_u8) \
    FN(ma_pcm_u8_to_s16) \
    FN(ma_pcm_u8_to_s24) \
    FN(ma_pcm_u8_to_s32) \
    FN(ma_pcm_u8_to_f32) \
    FN(ma_pcm_interleave_u8) \
    FN(ma_pcm_deinterleave_u8) \
    FN(ma_pcm_s16_to_u8) \
    FN(ma_pcm_s16_to_s16) \
    FN(ma_pcm_s16_to_s24) \
    FN(ma_pcm_s16_to_s32) \
    FN(ma_pcm_s16_to_f32) \
    FN(ma_pcm_interleave_s16) \
    FN(ma_pcm_deinterleave_s16) \
    FN(ma_pcm_s24_to_u8) \
    FN(ma_pcm_s24_to_s16) \
    FN(ma_pcm_s24_to_s24) \
    FN(ma_pcm_s24_to_s32) \
    FN(ma_pcm_s24_to_f32) \
    FN(ma_pcm_interleave_s24) \
    FN(ma_pcm_deinterleave_s24) \
    FN(ma_pcm_s32_to_u8) \
    FN(ma_pcm_s32_to_s16) \
    FN(ma_pcm_s32_to_s24) \
    FN(ma_pcm_s32_to_s32) \
    FN(ma_pcm_s32_to_f32) \
    FN(ma_pcm_interleave_s32) \
    FN(ma_pcm_deinterleave_s32) \
    FN(ma_pcm_f32_to_u8) \
    FN(ma_pcm_f32_to_s16) \
    FN(ma_pcm_f32_to_s24) \
    FN(ma_pcm_f32_to_s32) \
    FN(ma_pcm_f32_to_f32) \
    FN(ma_pcm_interleave_f32) \
    FN(ma_pcm_deinterleave_f32) \
    FN(ma_pcm_convert) \
    FN(ma_convert_pcm_frames_format) \
    FN(ma_deinterleave_pcm_frames) \
    FN(ma_interleave_pcm_frames) \
    FN(ma_biquad_config_init) \
    FN(ma_biquad_get_heap_size) \
    FN(ma_biquad_uninit) \
    FN(ma_biquad_reinit) \
    FN(ma_biquad_init_preallocated) \
    FN(ma_biquad_init) \
    FN(ma_biquad_clear_cache) \
    FN(ma_biquad_process_pcm_frames) \
    FN(ma_biquad_get_latency) \
    FN(ma_lpf1_config_init) \
    FN(ma_lpf2_config_init) \
    FN(ma_lpf1_get_heap_size) \
    FN(ma_lpf1_uninit) \
    FN(ma_lpf1_reinit) \
    FN(ma_lpf1_init_preallocated) \
    FN(ma_lpf1_init) \
    FN(ma_lpf1_clear_cache) \
    FN(ma_lpf1_process_pcm_frames) \
    FN(ma_lpf1_get_latency) \
    FN(ma_lpf2_get_heap_size) \
    FN(ma_lpf2_init_preallocated) \
    FN(ma_lpf2_init) \
    FN(ma_lpf2_uninit) \
    FN(ma_lpf2_reinit) \
    FN(ma_lpf2_clear_cache) \
    FN(ma_lpf2_process_pcm_frames) \
    FN(ma_lpf2_get_latency) \
    FN(ma_lpf_config_init) \
    FN(ma_lpf_get_heap_size) \
    FN(ma_lpf_init_preallocated) \
    FN(ma_lpf_init) \
    FN(ma_lpf_uninit) \
    FN(ma_lpf_reinit) \
    FN(ma_lpf_clear_cache) \
    FN(ma_lpf_process_pcm_frames) \
    FN(ma_lpf_get_latency) \
    FN(ma_hpf1_config_init) \
    FN(ma_hpf2_config_init) \
    FN(ma_hpf1_get_heap_size) \
    FN(ma_hpf1_init_preallocated) \
    FN(ma_hpf1_init) \
    FN(ma_hpf1_uninit) \
    FN(ma_hpf1_reinit) \
    FN(ma_hpf1_process_pcm_frames) \
    FN(ma_hpf1_get_latency) \
    FN(ma_hpf2_get_heap_size) \
    FN(ma_hpf2_init_preallocated) \
    FN(ma_hpf2_init) \
    FN(ma_hpf2_uninit) \
    FN(ma_hpf2_reinit) \
    FN(ma_hpf2_process_pcm_frames) \
    FN(ma_hpf2_get_latency) \
    FN(ma_hpf_config_init) \
    FN(ma_hpf_get_heap_size) \
    FN(ma_hpf_init_preallocated) \
    FN(ma_hpf_init) \
    FN(ma_hpf_uninit) \
    FN(ma_hpf_reinit) \
    FN(ma_hpf_process_pcm_frames) \
    FN(ma_hpf_get_latency) \
    FN(ma_bpf2_config_init) \
    FN(ma_bpf2_get_heap_size) \
    FN(ma_bpf2_init_preallocated) \
    FN(ma_bpf2_init) \
    FN(ma_bpf2_uninit) \
    FN(ma_bpf2_reinit) \
    FN(ma_bpf2_process_pcm_frames) \
    FN(ma_bpf2_get_latency) \
    FN(ma_bpf_config_init) \
    FN(ma_bpf_get_heap_size) \
    FN(ma_bpf_init_preallocated) \
    FN(ma_bpf_init) \
    FN(ma_bpf_uninit) \
    FN(ma_bpf_reinit) \
    FN(ma_bpf_process_pcm_frames) \
    FN(ma_bpf_get_latency) \
    FN(ma_notch2_config_init) \
    FN(ma_notch2_get_heap_size) \
    FN(ma_notch2_init_preallocated) \
    FN(ma_notch2_init) \
    FN(ma_notch2_uninit) \
    FN(ma_notch2_reinit) \
    FN(ma_notch2_process_pcm_frames) \
    FN(ma_notch2_get_latency) \
    FN(ma_peak2_config_init) \
    FN(ma_peak2_get_heap_size) \
    FN(ma_peak2_init_preallocated) \
    FN(ma_peak2_init) \
    FN(ma_peak2_uninit) \
    FN(ma_peak2_reinit) \
    FN(ma_peak2_process_pcm_frames) \
    FN(ma_peak2_get_latency) \
    FN(ma_loshelf2_config_init) \
    FN(ma_loshelf2_get_heap_size) \
    FN(ma_loshelf2_init_preallocated) \
    FN(ma_loshelf2_init) \
    FN(ma_loshelf2_uninit) \
    FN(ma_loshelf2_reinit) \
    FN(ma_loshelf2_process_pcm_frames) \
    FN(ma_loshelf2_get_latency) \
    FN(ma_hishelf2_config_init) \
    FN(ma_hishelf2_get_heap_size) \
    FN(ma_hishelf2_init_preallocated) \
    FN(ma_hishelf2_init) \
    FN(ma_hishelf2_uninit) \
    FN(ma_hishelf2_reinit) \
    FN(ma_hishelf2_process_pcm_frames) \
    FN(ma_hishelf2_get_latency) \
    FN(ma_delay_config_init) \
    FN(ma_delay_init) \
    FN(ma_delay_uninit) \
    FN(ma_delay_process_pcm_frames) \
    FN(ma_delay_set_wet) \
    FN(ma_delay_get_wet) \
    FN(ma_delay_set_dry) \
    FN(ma_delay_get_dry) \
    FN(ma_delay_set_decay) \
    FN(ma_delay_get_decay) \
    FN(ma_gainer_config_init) \
    FN(ma_gainer_get_heap_size) \
    FN(ma_gainer_init_preallocated) \
    FN(ma_gainer_init) \
    FN(ma_gainer_uninit) \
    FN(ma_gainer_process_pcm_frames) \
    FN(ma_gainer_set_gain) \
    FN(ma_gainer_set_gains) \
    FN(ma_gainer_set_master_volume) \
    FN(ma_gainer_get_master_volume) \
    FN(ma_panner_config_init) \
    FN(ma_panner_init) \
    FN(ma_panner_process_pcm_frames) \
    FN(ma_panner_set_mode) \
    FN(ma_panner_get_mode) \
    FN(ma_panner_set_pan) \
    FN(ma_panner_get_pan) \
    FN(ma_fader_config_init) \
    FN(ma_fader_init) \
    FN(ma_fader_process_pcm_frames) \
    FN(ma_fader_get_data_format) \
    FN(ma_fader_set_fade) \
    FN(ma_fader_set_fade_ex) \
    FN(ma_fader_get_current_volume) \
    FN(ma_vec3f_init_3f) \
    FN(ma_vec3f_sub) \
    FN(ma_vec3f_neg) \
    FN(ma_vec3f_dot) \
    FN(ma_vec3f_len2) \
    FN(ma_vec3f_len) \
    FN(ma_vec3f_dist) \
    FN(ma_vec3f_normalize) \
    FN(ma_vec3f_cross) \
    FN(ma_atomic_vec3f_init) \
    FN(ma_atomic_vec3f_set) \
    FN(ma_atomic_vec3f_get) \
    FN(ma_spatializer_listener_config_init) \
    FN(ma_spatializer_listener_get_heap_size) \
    FN(ma_spatializer_listener_init_preallocated) \
    FN(ma_spatializer_listener_init) \
    FN(ma_spatializer_listener_uninit) \
    FN(ma_spatializer_listener_get_channel_map) \
    FN(ma_spatializer_listener_set_cone) \
    FN(ma_spatializer_listener_get_cone) \
    FN(ma_spatializer_listener_set_position) \
    FN(ma_spatializer_listener_get_position) \
    FN(ma_spatializer_listener_set_direction) \
    FN(ma_spatializer_listener_get_direction) \
    FN(ma_spatializer_listener_set_velocity) \
    FN(ma_spatializer_listener_get_velocity) \
    FN(ma_spatializer_listener_set_speed_of_sound) \
    FN(ma_spatializer_listener_get_speed_of_sound) \
    FN(ma_spatializer_listener_set_world_up) \
    FN(ma_spatializer_listener_get_world_up) \
    FN(ma_spatializer_listener_set_enabled) \
    FN(ma_spatializer_listener_is_enabled) \
    FN(ma_spatializer_config_init) \
    FN(ma_spatializer_get_heap_size) \
    FN(ma_spatializer_init_preallocated) \
    FN(ma_spatializer_init) \
    FN(ma_spatializer_uninit) \
    FN(ma_spatializer_set_master_volume) \
    FN(ma_spatializer_get_master_volume) \
    FN(ma_spatializer_get_input_channels) \
    FN(ma_spatializer_get_output_channels) \
    FN(ma_spatializer_set_attenuation_model) \
    FN(ma_spatializer_get_attenuation_model) \
    FN(ma_spatializer_set_positioning) \
    FN(ma_spatializer_get_positioning) \
    FN(ma_spatializer_set_rolloff) \
    FN(ma_spatializer_get_rolloff) \
    FN(ma_spatializer_set_min_gain) \
    FN(ma_spatializer_get_min_gain) \
    FN(ma_spatializer_set_max_gain) \
    FN(ma_spatializer_get_max_gain) \
    FN(ma_spatializer_set_min_distance) \
    FN(ma_spatializer_get_min_distance) \
    FN(ma_spatializer_set_max_distance) \
    FN(ma_spatializer_get_max_distance) \
    FN(ma_spatializer_set_cone) \
    FN(ma_spatializer_get_cone) \
    FN(ma_spatializer_set_doppler_factor) \
    FN(ma_spatializer_get_doppler_factor) \
    FN(ma_spatializer_set_directional_attenuation_factor) \
    FN(ma_spatializer_get_directional_attenuation_factor) \
    FN(ma_spatializer_set_position) \
    FN(ma_spatializer_get_position) \
    FN(ma_spatializer_set_direction) \
    FN(ma_spatializer_get_direction) \
    FN(ma_spatializer_set_velocity) \
    FN(ma_spatializer_get_velocity) \
    FN(ma_spatializer_get_relative_position_and_direction) \
    FN(ma_spatializer_process_pcm_frames) \
    FN(ma_linear_resampler_config_init) \
    FN(ma_linear_resampler_get_heap_size) \
    FN(ma_linear_resampler_init_preallocated) \
    FN(ma_linear_resampler_init) \
    FN(ma_linear_resampler_uninit) \
    FN(ma_linear_resampler_process_pcm_frames) \
    FN(ma_linear_resampler_set_rate) \
    FN(ma_linear_resampler_set_rate_ratio) \
    FN(ma_linear_resampler_get_input_latency) \
    FN(ma_linear_resampler_get_output_latency) \
    FN(ma_linear_resampler_get_required_input_frame_count) \
    FN(ma_linear_resampler_get_expected_output_frame_count) \
    FN(ma_linear_resampler_reset) \
    FN(ma_resampler_config_init) \
    FN(ma_resampler_get_heap_size) \
    FN(ma_resampler_init_preallocated) \
    FN(ma_resampler_init) \
    FN(ma_resampler_uninit) \
    FN(ma_resampler_process_pcm_frames) \
    FN(ma_resampler_set_rate) \
    FN(ma_resampler_set_rate_ratio) \
    FN(ma_resampler_get_input_latency) \
    FN(ma_resampler_get_output_latency) \
    FN(ma_resampler_get_required_input_frame_count) \
    FN(ma_resampler_get_expected_output_frame_count) \
    FN(ma_resampler_reset) \
    FN(ma_channel_converter_config_init) \
    FN(ma_channel_converter_get_heap_size) \
    FN(ma_channel_converter_init_preallocated) \
    FN(ma_channel_converter_init) \
    FN(ma_channel_converter_uninit) \
    FN(ma_channel_converter_process_pcm_frames) \
    FN(ma_channel_converter_get_input_channel_map) \
    FN(ma_channel_converter_get_output_channel_map) \
    FN(ma_data_converter_config_init_default) \
    FN(ma_data_converter_config_init) \
    FN(ma_data_converter_get_heap_size) \
    FN(ma_data_converter_init_preallocated) \
    FN(ma_data_converter_init) \
    FN(ma_data_converter_uninit) \
    FN(ma_device_post_init) \
    FN(ma_device_uninit) \
    FN(ma_data_converter_process_pcm_frames) \
    FN(ma_data_converter_set_rate) \
    FN(ma_data_converter_set_rate_ratio) \
    FN(ma_data_converter_get_input_latency) \
    FN(ma_data_converter_get_output_latency) \
    FN(ma_data_converter_get_required_input_frame_count) \
    FN(ma_data_converter_get_expected_output_frame_count) \
    FN(ma_data_converter_reset) \
    FN(ma_channel_map_get_channel) \
    FN(ma_channel_map_init_blank) \
    FN(ma_channel_map_init_standard) \
    FN(ma_channel_map_copy) \
    FN(ma_channel_map_copy_or_default) \
    FN(ma_data_converter_get_input_channel_map) \
    FN(ma_data_converter_get_output_channel_map) \
    FN(ma_channel_map_is_valid) \
    FN(ma_channel_map_is_equal) \
    FN(ma_channel_map_is_blank) \
    FN(ma_channel_map_contains_channel_position) \
    FN(ma_channel_map_find_channel_position) \
    FN(ma_channel_position_to_string) \
    FN(ma_channel_map_to_string) \
    FN(ma_convert_frames_ex) \
    FN(ma_convert_frames) \
    FN(ma_rb_init_ex) \
    FN(ma_rb_init) \
    FN(ma_rb_uninit) \
    FN(ma_rb_reset) \
    FN(ma_rb_acquire_read) \
    FN(ma_rb_commit_read) \
    FN(ma_rb_acquire_write) \
    FN(ma_rb_commit_write) \
    FN(ma_rb_seek_read) \
    FN(ma_rb_seek_write) \
    FN(ma_rb_pointer_distance) \
    FN(ma_rb_available_read) \
    FN(ma_rb_available_write) \
    FN(ma_rb_get_subbuffer_size) \
    FN(ma_rb_get_subbuffer_stride) \
    FN(ma_rb_get_subbuffer_offset) \
    FN(ma_rb_get_subbuffer_ptr) \
    FN(ma_pcm_rb_init_ex) \
    FN(ma_pcm_rb_init) \
    FN(ma_pcm_rb_uninit) \
    FN(ma_pcm_rb_reset) \
    FN(ma_pcm_rb_acquire_read) \
    FN(ma_pcm_rb_commit_read) \
    FN(ma_pcm_rb_acquire_write) \
    FN(ma_device_handle_backend_data_callback) \
    FN(ma_pcm_rb_commit_write) \
    FN(ma_pcm_rb_seek_read) \
    FN(ma_pcm_rb_seek_write) \
    FN(ma_pcm_rb_pointer_distance) \
    FN(ma_pcm_rb_available_read) \
    FN(ma_pcm_rb_available_write) \
    FN(ma_pcm_rb_get_subbuffer_size) \
    FN(ma_pcm_rb_get_subbuffer_stride) \
    FN(ma_pcm_rb_get_subbuffer_offset) \
    FN(ma_pcm_rb_get_subbuffer_ptr) \
    FN(ma_pcm_rb_get_format) \
    FN(ma_pcm_rb_get_channels) \
    FN(ma_pcm_rb_get_sample_rate) \
    FN(ma_pcm_rb_set_sample_rate) \
    FN(ma_duplex_rb_init) \
    FN(ma_device_init) \
    FN(ma_device_init_ex) \
    FN(ma_duplex_rb_uninit) \
    FN(ma_result_description) \
    FN(ma_malloc) \
    FN(ma_calloc) \
    FN(ma_realloc) \
    FN(ma_free) \
    FN(ma_aligned_malloc) \
    FN(ma_aligned_free) \
    FN(ma_get_format_name) \
    FN(ma_blend_f32) \
    FN(ma_get_bytes_per_sample) \
    FN(ma_data_source_config_init) \
    FN(ma_data_source_init) \
    FN(ma_data_source_uninit) \
    FN(ma_data_source_seek_to_pcm_frame) \
    FN(ma_data_source_seek_to_second) \
    FN(ma_data_source_get_data_format) \
    FN(ma_data_source_get_cursor_in_pcm_frames) \
    FN(ma_data_source_read_pcm_frames) \
    FN(ma_data_source_seek_pcm_frames) \
    FN(ma_data_source_seek_seconds) \
    FN(ma_data_source_get_length_in_pcm_frames) \
    FN(ma_data_source_get_cursor_in_seconds) \
    FN(ma_data_source_get_length_in_seconds) \
    FN(ma_data_source_set_looping) \
    FN(ma_data_source_is_looping) \
    FN(ma_data_source_set_range_in_pcm_frames) \
    FN(ma_data_source_get_range_in_pcm_frames) \
    FN(ma_data_source_set_loop_point_in_pcm_frames) \
    FN(ma_data_source_get_loop_point_in_pcm_frames) \
    FN(ma_data_source_set_current) \
    FN(ma_data_source_get_current) \
    FN(ma_data_source_set_next) \
    FN(ma_data_source_get_next) \
    FN(ma_data_source_set_next_callback) \
    FN(ma_data_source_get_next_callback) \
    FN(ma_audio_buffer_ref_init) \
    FN(ma_audio_buffer_ref_uninit) \
    FN(ma_audio_buffer_ref_set_data) \
    FN(ma_audio_buffer_ref_read_pcm_frames) \
    FN(ma_audio_buffer_ref_seek_to_pcm_frame) \
    FN(ma_audio_buffer_ref_map) \
    FN(ma_audio_buffer_ref_unmap) \
    FN(ma_audio_buffer_ref_at_end) \
    FN(ma_audio_buffer_ref_get_cursor_in_pcm_frames) \
    FN(ma_audio_buffer_ref_get_length_in_pcm_frames) \
    FN(ma_audio_buffer_ref_get_available_frames) \
    FN(ma_audio_buffer_config_init) \
    FN(ma_audio_buffer_init) \
    FN(ma_audio_buffer_init_copy) \
    FN(ma_audio_buffer_alloc_and_init) \
    FN(ma_audio_buffer_uninit) \
    FN(ma_audio_buffer_uninit_and_free) \
    FN(ma_audio_buffer_read_pcm_frames) \
    FN(ma_audio_buffer_seek_to_pcm_frame) \
    FN(ma_audio_buffer_map) \
    FN(ma_audio_buffer_unmap) \
    FN(ma_audio_buffer_at_end) \
    FN(ma_audio_buffer_get_cursor_in_pcm_frames) \
    FN(ma_audio_buffer_get_length_in_pcm_frames) \
    FN(ma_audio_buffer_get_available_frames) \
    FN(ma_paged_audio_buffer_data_init) \
    FN(ma_paged_audio_buffer_data_uninit) \
    FN(ma_paged_audio_buffer_data_get_head) \
    FN(ma_paged_audio_buffer_data_get_tail) \
    FN(ma_paged_audio_buffer_data_get_length_in_pcm_frames) \
    FN(ma_paged_audio_buffer_data_allocate_page) \
    FN(ma_paged_audio_buffer_data_free_page) \
    FN(ma_paged_audio_buffer_data_append_page) \
    FN(ma_paged_audio_buffer_data_allocate_and_append_page) \
    FN(ma_paged_audio_buffer_config_init) \
    FN(ma_paged_audio_buffer_init) \
    FN(ma_paged_audio_buffer_uninit) \
    FN(ma_paged_audio_buffer_read_pcm_frames) \
    FN(ma_paged_audio_buffer_seek_to_pcm_frame) \
    FN(ma_paged_audio_buffer_get_cursor_in_pcm_frames) \
    FN(ma_paged_audio_buffer_get_length_in_pcm_frames) \
    FN(ma_vfs_open) \
    FN(ma_vfs_open_w) \
    FN(ma_vfs_close) \
    FN(ma_vfs_read) \
    FN(ma_vfs_write) \
    FN(ma_vfs_seek) \
    FN(ma_vfs_tell) \
    FN(ma_vfs_info) \
    FN(ma_default_vfs_init) \
    FN(ma_vfs_or_default_open) \
    FN(ma_vfs_or_default_open_w) \
    FN(ma_vfs_or_default_close) \
    FN(ma_vfs_or_default_read) \
    FN(ma_vfs_or_default_write) \
    FN(ma_vfs_or_default_seek) \
    FN(ma_vfs_or_default_tell) \
    FN(ma_vfs_or_default_info) \
    FN(ma_vfs_open_and_read_file) \
    FN(ma_vfs_open_and_read_file_w) \
    FN(ma_decoding_backend_config_init) \
    FN(ma_decoder_config_init) \
    FN(ma_decoder_config_init_default) \
    FN(ma_decoder_config_init_copy) \
    FN(ma_wav_seek_to_pcm_frame) \
    FN(ma_wav_get_data_format) \
    FN(ma_wav_get_cursor_in_pcm_frames) \
    FN(ma_wav_get_length_in_pcm_frames) \
    FN(ma_flac_init) \
    FN(ma_flac_read_pcm_frames) \
    FN(ma_flac_get_data_format) \
    FN(ma_flac_get_cursor_in_pcm_frames) \
    FN(ma_flac_get_length_in_pcm_frames) \
    FN(ma_mp3_uninit) \
    FN(ma_mp3_get_data_format) \
    FN(ma_mp3_get_cursor_in_pcm_frames) \
    FN(ma_decoder_uninit) \
    FN(ma_decoder_init_vfs) \
    FN(ma_decoder_init) \
    FN(ma_decoder_init_memory) \
    FN(ma_decoder_init_file) \
    FN(ma_decoder_init_vfs_w) \
    FN(ma_decoder_init_file_w) \
    FN(ma_decoder_read_pcm_frames) \
    FN(ma_decoder_seek_to_pcm_frame) \
    FN(ma_decoder_get_data_format) \
    FN(ma_decoder_get_cursor_in_pcm_frames) \
    FN(ma_decoder_get_length_in_pcm_frames) \
    FN(ma_decoder_get_available_frames) \
    FN(ma_decode_from_vfs) \
    FN(ma_decode_file) \
    FN(ma_decode_memory) \
    FN(ma_encoder_config_init) \
    FN(ma_encoder_preinit) \
    FN(ma_encoder_init__internal) \
    FN(ma_encoder_init_vfs) \
    FN(ma_encoder_init_vfs_w) \
    FN(ma_encoder_init_file) \
    FN(ma_encoder_init_file_w) \
    FN(ma_encoder_init) \
    FN(ma_waveform_config_init) \
    FN(ma_waveform_init) \
    FN(ma_waveform_uninit) \
    FN(ma_waveform_set_amplitude) \
    FN(ma_waveform_set_frequency) \
    FN(ma_waveform_set_type) \
    FN(ma_waveform_set_sample_rate) \
    FN(ma_waveform_read_pcm_frames) \
    FN(ma_waveform_seek_to_pcm_frame) \
    FN(ma_pulsewave_config_init) \
    FN(ma_pulsewave_init) \
    FN(ma_pulsewave_uninit) \
    FN(ma_pulsewave_read_pcm_frames) \
    FN(ma_pulsewave_seek_to_pcm_frame) \
    FN(ma_pulsewave_set_amplitude) \
    FN(ma_pulsewave_set_frequency) \
    FN(ma_pulsewave_set_sample_rate) \
    FN(ma_pulsewave_set_duty_cycle) \
    FN(ma_noise_config_init) \
    FN(ma_noise_get_heap_size) \
    FN(ma_noise_init_preallocated) \
    FN(ma_noise_init) \
    FN(ma_noise_uninit) \
    FN(ma_noise_set_amplitude) \
    FN(ma_noise_set_seed) \
    FN(ma_noise_set_type) \
    FN(ma_noise_read_pcm_frames) \
    FN(ma_resource_manager_pipeline_notifications_init) \
    FN(ma_resource_manager_config_init) \
    FN(ma_resource_manager_init) \
    FN(ma_resource_manager_uninit) \
    FN(ma_resource_manager_get_log) \
    FN(ma_resource_manager_data_source_config_init) \
    FN(ma_resource_manager_data_buffer_uninit) \
    FN(ma_resource_manager_data_buffer_seek_to_pcm_frame) \
    FN(ma_resource_manager_data_buffer_init_ex) \
    FN(ma_resource_manager_data_buffer_init) \
    FN(ma_resource_manager_data_buffer_init_w) \
    FN(ma_resource_manager_data_buffer_init_copy) \
    FN(ma_resource_manager_data_buffer_get_data_format) \
    FN(ma_resource_manager_data_buffer_get_cursor_in_pcm_frames) \
    FN(ma_resource_manager_data_buffer_get_length_in_pcm_frames) \
    FN(ma_resource_manager_data_buffer_result) \
    FN(ma_resource_manager_data_buffer_set_looping) \
    FN(ma_resource_manager_data_buffer_is_looping) \
    FN(ma_resource_manager_data_buffer_get_available_frames) \
    FN(ma_resource_manager_data_buffer_read_pcm_frames) \
    FN(ma_resource_manager_register_file) \
    FN(ma_resource_manager_register_file_w) \
    FN(ma_resource_manager_register_decoded_data) \
    FN(ma_resource_manager_register_decoded_data_w) \
    FN(ma_resource_manager_register_encoded_data) \
    FN(ma_resource_manager_register_encoded_data_w) \
    FN(ma_resource_manager_unregister_file) \
    FN(ma_resource_manager_unregister_file_w) \
    FN(ma_resource_manager_unregister_data) \
    FN(ma_resource_manager_unregister_data_w) \
    FN(ma_resource_manager_data_stream_init_ex) \
    FN(ma_resource_manager_data_stream_init) \
    FN(ma_resource_manager_data_stream_init_w) \
    FN(ma_resource_manager_data_stream_uninit) \
    FN(ma_resource_manager_data_stream_seek_to_pcm_frame) \
    FN(ma_resource_manager_data_stream_get_data_format) \
    FN(ma_resource_manager_data_stream_read_pcm_frames) \
    FN(ma_resource_manager_data_stream_get_cursor_in_pcm_frames) \
    FN(ma_resource_manager_data_stream_get_length_in_pcm_frames) \
    FN(ma_resource_manager_data_stream_result) \
    FN(ma_resource_manager_data_stream_set_looping) \
    FN(ma_resource_manager_data_stream_is_looping) \
    FN(ma_resource_manager_data_stream_get_available_frames) \
    FN(ma_resource_manager_data_source_init_ex) \
    FN(ma_resource_manager_data_source_init) \
    FN(ma_resource_manager_data_source_init_w) \
    FN(ma_resource_manager_data_source_init_copy) \
    FN(ma_resource_manager_data_source_uninit) \
    FN(ma_resource_manager_data_source_read_pcm_frames) \
    FN(ma_resource_manager_data_source_seek_to_pcm_frame) \
    FN(ma_resource_manager_data_source_map) \
    FN(ma_resource_manager_data_source_unmap) \
    FN(ma_resource_manager_data_source_get_data_format) \
    FN(ma_resource_manager_data_source_get_cursor_in_pcm_frames) \
    FN(ma_resource_manager_data_source_get_length_in_pcm_frames) \
    FN(ma_resource_manager_data_source_result) \
    FN(ma_resource_manager_data_source_set_looping) \
    FN(ma_resource_manager_data_source_is_looping) \
    FN(ma_resource_manager_data_source_get_available_frames) \
    FN(ma_resource_manager_post_job) \
    FN(ma_resource_manager_post_job_quit) \
    FN(ma_resource_manager_next_job) \
    FN(ma_resource_manager_process_job) \
    FN(ma_resource_manager_process_next_job) \
    FN(ma_debug_fill_pcm_frames_with_sine_wave) \
    FN(ma_node_graph_config_init) \
    FN(ma_node_graph_get_endpoint) \
    FN(ma_node_graph_get_channels) \
    FN(ma_node_graph_get_time) \
    FN(ma_node_graph_set_time) \
    FN(ma_node_graph_get_processing_size_in_frames) \
    FN(ma_node_config_init) \
    FN(ma_node_get_heap_size) \
    FN(ma_node_init_preallocated) \
    FN(ma_node_init) \
    FN(ma_node_get_node_graph) \
    FN(ma_node_get_input_bus_count) \
    FN(ma_node_get_output_bus_count) \
    FN(ma_node_get_input_channels) \
    FN(ma_node_get_output_channels) \
    FN(ma_node_detach_output_bus) \
    FN(ma_node_uninit) \
    FN(ma_node_graph_init) \
    FN(ma_node_graph_uninit) \
    FN(ma_node_detach_all_output_buses) \
    FN(ma_node_attach_output_bus) \
    FN(ma_node_set_output_bus_volume) \
    FN(ma_node_get_output_bus_volume) \
    FN(ma_node_set_state) \
    FN(ma_node_get_state) \
    FN(ma_node_set_state_time) \
    FN(ma_node_get_state_time) \
    FN(ma_node_get_state_by_time) \
    FN(ma_node_get_state_by_time_range) \
    FN(ma_node_graph_read_pcm_frames) \
    FN(ma_node_get_time) \
    FN(ma_node_set_time) \
    FN(ma_data_source_node_config_init) \
    FN(ma_data_source_node_init) \
    FN(ma_data_source_node_uninit) \
    FN(ma_data_source_node_set_looping) \
    FN(ma_data_source_node_is_looping) \
    FN(ma_splitter_node_config_init) \
    FN(ma_splitter_node_init) \
    FN(ma_splitter_node_uninit) \
    FN(ma_biquad_node_config_init) \
    FN(ma_biquad_node_init) \
    FN(ma_biquad_node_reinit) \
    FN(ma_biquad_node_uninit) \
    FN(ma_lpf_node_config_init) \
    FN(ma_lpf_node_init) \
    FN(ma_lpf_node_reinit) \
    FN(ma_lpf_node_uninit) \
    FN(ma_hpf_node_config_init) \
    FN(ma_hpf_node_init) \
    FN(ma_hpf_node_reinit) \
    FN(ma_hpf_node_uninit) \
    FN(ma_bpf_node_config_init) \
    FN(ma_bpf_node_init) \
    FN(ma_bpf_node_reinit) \
    FN(ma_bpf_node_uninit) \
    FN(ma_notch_node_config_init) \
    FN(ma_notch_node_init) \
    FN(ma_notch_node_reinit) \
    FN(ma_notch_node_uninit) \
    FN(ma_peak_node_config_init) \
    FN(ma_peak_node_init) \
    FN(ma_peak_node_reinit) \
    FN(ma_peak_node_uninit) \
    FN(ma_loshelf_node_config_init) \
    FN(ma_loshelf_node_init) \
    FN(ma_loshelf_node_reinit) \
    FN(ma_loshelf_node_uninit) \
    FN(ma_hishelf_node_config_init) \
    FN(ma_hishelf_node_init) \
    FN(ma_hishelf_node_reinit) \
    FN(ma_hishelf_node_uninit) \
    FN(ma_delay_node_config_init) \
    FN(ma_delay_node_init) \
    FN(ma_delay_node_uninit) \
    FN(ma_delay_node_set_wet) \
    FN(ma_delay_node_get_wet) \
    FN(ma_delay_node_set_dry) \
    FN(ma_delay_node_get_dry) \
    FN(ma_delay_node_set_decay) \
    FN(ma_delay_node_get_decay) \
    FN(ma_engine_node_config_init) \
    FN(ma_engine_node_get_heap_size) \
    FN(ma_engine_node_init_preallocated) \
    FN(ma_engine_node_init) \
    FN(ma_engine_node_uninit) \
    FN(ma_sound_config_init) \
    FN(ma_sound_config_init_2) \
    FN(ma_sound_group_config_init) \
    FN(ma_sound_group_config_init_2) \
    FN(ma_engine_config_init) \
    FN(ma_engine_init) \
    FN(ma_engine_read_pcm_frames) \
    FN(ma_engine_get_node_graph) \
    FN(ma_engine_get_resource_manager) \
    FN(ma_engine_get_device) \
    FN(ma_engine_get_log) \
    FN(ma_engine_get_endpoint) \
    FN(ma_engine_get_time_in_pcm_frames) \
    FN(ma_engine_get_time_in_milliseconds) \
    FN(ma_engine_set_time_in_pcm_frames) \
    FN(ma_engine_set_time_in_milliseconds) \
    FN(ma_engine_get_time) \
    FN(ma_engine_set_time) \
    FN(ma_engine_get_channels) \
    FN(ma_engine_get_sample_rate) \
    FN(ma_engine_start) \
    FN(ma_engine_stop) \
    FN(ma_engine_set_volume) \
    FN(ma_engine_get_volume) \
    FN(ma_engine_set_gain_db) \
    FN(ma_engine_get_gain_db) \
    FN(ma_engine_get_listener_count) \
    FN(ma_engine_find_closest_listener) \
    FN(ma_engine_listener_set_position) \
    FN(ma_engine_listener_get_position) \
    FN(ma_engine_listener_set_direction) \
    FN(ma_engine_listener_get_direction) \
    FN(ma_engine_listener_set_velocity) \
    FN(ma_engine_listener_get_velocity) \
    FN(ma_engine_listener_set_cone) \
    FN(ma_engine_listener_get_cone) \
    FN(ma_engine_listener_set_world_up) \
    FN(ma_engine_listener_get_world_up) \
    FN(ma_engine_listener_set_enabled) \
    FN(ma_engine_listener_is_enabled) \
    FN(ma_sound_init_from_file_internal) \
    FN(ma_sound_init_copy) \
    FN(ma_sound_init_ex) \
    FN(ma_sound_init_from_file) \
    FN(ma_sound_init_from_file_w) \
    FN(ma_sound_init_from_data_source) \
    FN(ma_sound_uninit) \
    FN(ma_engine_uninit) \
    FN(ma_sound_get_engine) \
    FN(ma_sound_get_data_source) \
    FN(ma_sound_start) \
    FN(ma_engine_play_sound_ex) \
    FN(ma_engine_play_sound) \
    FN(ma_sound_stop) \
    FN(ma_sound_stop_with_fade_in_pcm_frames) \
    FN(ma_sound_stop_with_fade_in_milliseconds) \
    FN(ma_sound_reset_start_time) \
    FN(ma_sound_reset_stop_time) \
    FN(ma_sound_reset_fade) \
    FN(ma_sound_reset_stop_time_and_fade) \
    FN(ma_sound_set_volume) \
    FN(ma_sound_get_volume) \
    FN(ma_sound_set_pan) \
    FN(ma_sound_get_pan) \
    FN(ma_sound_set_pan_mode) \
    FN(ma_sound_get_pan_mode) \
    FN(ma_sound_set_pitch) \
    FN(ma_sound_get_pitch) \
    FN(ma_sound_set_spatialization_enabled) \
    FN(ma_sound_is_spatialization_enabled) \
    FN(ma_sound_set_pinned_listener_index) \
    FN(ma_sound_get_pinned_listener_index) \
    FN(ma_sound_get_listener_index) \
    FN(ma_sound_get_direction_to_listener) \
    FN(ma_sound_set_position) \
    FN(ma_sound_get_position) \
    FN(ma_sound_set_direction) \
    FN(ma_sound_get_direction) \
    FN(ma_sound_set_velocity) \
    FN(ma_sound_get_velocity) \
    FN(ma_sound_set_attenuation_model) \
    FN(ma_sound_get_attenuation_model) \
    FN(ma_sound_set_positioning) \
    FN(ma_sound_get_positioning) \
    FN(ma_sound_set_rolloff) \
    FN(ma_sound_get_rolloff) \
    FN(ma_sound_set_min_gain) \
    FN(ma_sound_get_min_gain) \
    FN(ma_sound_set_max_gain) \
    FN(ma_sound_get_max_gain) \
    FN(ma_sound_set_min_distance) \
    FN(ma_sound_get_min_distance) \
    FN(ma_sound_set_max_distance) \
    FN(ma_sound_get_max_distance) \
    FN(ma_sound_set_cone) \
    FN(ma_sound_get_cone) \
    FN(ma_sound_set_doppler_factor) \
    FN(ma_sound_get_doppler_factor) \
    FN(ma_sound_set_directional_attenuation_factor) \
    FN(ma_sound_get_directional_attenuation_factor) \
    FN(ma_sound_set_fade_in_pcm_frames) \
    FN(ma_sound_set_fade_in_milliseconds) \
    FN(ma_sound_set_fade_start_in_pcm_frames) \
    FN(ma_sound_set_fade_start_in_milliseconds) \
    FN(ma_sound_get_current_fade_volume) \
    FN(ma_sound_set_start_time_in_pcm_frames) \
    FN(ma_sound_set_start_time_in_milliseconds) \
    FN(ma_sound_set_stop_time_in_pcm_frames) \
    FN(ma_sound_set_stop_time_in_milliseconds) \
    FN(ma_sound_set_stop_time_with_fade_in_pcm_frames) \
    FN(ma_sound_set_stop_time_with_fade_in_milliseconds) \
    FN(ma_sound_is_playing) \
    FN(ma_sound_get_time_in_pcm_frames) \
    FN(ma_sound_get_time_in_milliseconds) \
    FN(ma_sound_set_looping) \
    FN(ma_sound_is_looping) \
    FN(ma_sound_at_end) \
    FN(ma_sound_seek_to_pcm_frame) \
    FN(ma_sound_seek_to_second) \
    FN(ma_sound_get_data_format) \
    FN(ma_sound_get_cursor_in_pcm_frames) \
    FN(ma_sound_get_length_in_pcm_frames) \
    FN(ma_sound_get_cursor_in_seconds) \
    FN(ma_sound_get_length_in_seconds) \
    FN(ma_sound_set_end_callback) \
    FN(ma_sound_group_init) \
    FN(ma_sound_group_init_ex) \
    FN(ma_sound_group_uninit) \
    FN(ma_sound_group_get_engine) \
    FN(ma_sound_group_start) \
    FN(ma_sound_group_stop) \
    FN(ma_sound_group_set_volume) \
    FN(ma_sound_group_get_volume) \
    FN(ma_sound_group_set_pan) \
    FN(ma_sound_group_get_pan) \
    FN(ma_sound_group_set_pan_mode) \
    FN(ma_sound_group_get_pan_mode) \
    FN(ma_sound_group_set_pitch) \
    FN(ma_sound_group_get_pitch) \
    FN(ma_sound_group_set_spatialization_enabled) \
    FN(ma_sound_group_is_spatialization_enabled) \
    FN(ma_sound_group_set_pinned_listener_index) \
    FN(ma_sound_group_get_pinned_listener_index) \
    FN(ma_sound_group_get_listener_index) \
    FN(ma_sound_group_get_direction_to_listener) \
    FN(ma_sound_group_set_position) \
    FN(ma_sound_group_get_position) \
    FN(ma_sound_group_set_direction) \
    FN(ma_sound_group_get_direction) \
    FN(ma_sound_group_set_velocity) \
    FN(ma_sound_group_get_velocity) \
    FN(ma_sound_group_set_attenuation_model) \
    FN(ma_sound_group_get_attenuation_model) \
    FN(ma_sound_group_set_positioning) \
    FN(ma_sound_group_get_positioning) \
    FN(ma_sound_group_set_rolloff) \
    FN(ma_sound_group_get_rolloff) \
    FN(ma_sound_group_set_min_gain) \
    FN(ma_sound_group_get_min_gain) \
    FN(ma_sound_group_set_max_gain) \
    FN(ma_sound_group_get_max_gain) \
    FN(ma_sound_group_set_min_distance) \
    FN(ma_sound_group_get_min_distance) \
    FN(ma_sound_group_set_max_distance) \
    FN(ma_sound_group_get_max_distance) \
    FN(ma_sound_group_set_cone) \
    FN(ma_sound_group_get_cone) \
    FN(ma_sound_group_set_doppler_factor) \
    FN(ma_sound_group_get_doppler_factor) \
    FN(ma_sound_group_set_directional_attenuation_factor) \
    FN(ma_sound_group_get_directional_attenuation_factor) \
    FN(ma_sound_group_set_fade_in_pcm_frames) \
    FN(ma_sound_group_set_fade_in_milliseconds) \
    FN(ma_sound_group_get_current_fade_volume) \
    FN(ma_sound_group_set_start_time_in_pcm_frames) \
    FN(ma_sound_group_set_start_time_in_milliseconds) \
    FN(ma_sound_group_set_stop_time_in_pcm_frames) \
    FN(ma_sound_group_set_stop_time_in_milliseconds) \
    FN(ma_sound_group_is_playing) \
    FN(ma_sound_group_get_time_in_pcm_frames) \
    FN(ma_dr_wav_version) \
    FN(ma_dr_wav_version_string) \
    FN(ma_dr_wav_fmt_get_format) \
    FN(ma_dr_wav_take_ownership_of_metadata) \
    FN(ma_dr_wav_init_write) \
    FN(ma_dr_wav_init_write_sequential) \
    FN(ma_dr_wav_init_write_sequential_pcm_frames) \
    FN(ma_dr_wav_init_write_with_metadata) \
    FN(ma_dr_wav_target_write_size_bytes) \
    FN(ma_dr_wav_init_file_write) \
    FN(ma_dr_wav_init_file_write_sequential) \
    FN(ma_dr_wav_init_file_write_sequential_pcm_frames) \
    FN(ma_dr_wav_init_file_write_w) \
    FN(ma_dr_wav_init_file_write_sequential_w) \
    FN(ma_dr_wav_init_file_write_sequential_pcm_frames_w) \
    FN(ma_dr_wav_init_memory_write) \
    FN(ma_dr_wav_init_memory_write_sequential) \
    FN(ma_dr_wav_init_memory_write_sequential_pcm_frames) \
    FN(ma_dr_wav_uninit) \
    FN(ma_wav_uninit) \
    FN(ma_encoder_uninit) \
    FN(ma_dr_wav_read_raw) \
    FN(ma_dr_wav_read_pcm_frames_le) \
    FN(ma_dr_wav_read_pcm_frames_be) \
    FN(ma_dr_wav_read_pcm_frames) \
    FN(ma_dr_wav_seek_to_pcm_frame) \
    FN(ma_dr_wav_get_cursor_in_pcm_frames) \
    FN(ma_dr_wav_get_length_in_pcm_frames) \
    FN(ma_dr_wav_write_raw) \
    FN(ma_dr_wav_write_pcm_frames_le) \
    FN(ma_encoder_write_pcm_frames) \
    FN(ma_dr_wav_write_pcm_frames_be) \
    FN(ma_dr_wav_write_pcm_frames) \
    FN(ma_dr_wav_u8_to_s16) \
    FN(ma_dr_wav_s24_to_s16) \
    FN(ma_dr_wav_s32_to_s16) \
    FN(ma_dr_wav_f32_to_s16) \
    FN(ma_dr_wav_f64_to_s16) \
    FN(ma_dr_wav_read_pcm_frames_s16) \
    FN(ma_dr_wav_read_pcm_frames_s16le) \
    FN(ma_dr_wav_read_pcm_frames_s16be) \
    FN(ma_dr_wav_alaw_to_s16) \
    FN(ma_dr_wav_mulaw_to_s16) \
    FN(ma_dr_wav_u8_to_f32) \
    FN(ma_dr_wav_s16_to_f32) \
    FN(ma_dr_wav_s24_to_f32) \
    FN(ma_dr_wav_read_pcm_frames_f32) \
    FN(ma_dr_wav_read_pcm_frames_f32le) \
    FN(ma_dr_wav_read_pcm_frames_f32be) \
    FN(ma_dr_wav_s32_to_f32) \
    FN(ma_dr_wav_f64_to_f32) \
    FN(ma_dr_wav_alaw_to_f32) \
    FN(ma_dr_wav_mulaw_to_f32) \
    FN(ma_dr_wav_u8_to_s32) \
    FN(ma_dr_wav_s16_to_s32) \
    FN(ma_dr_wav_s24_to_s32) \
    FN(ma_dr_wav_f32_to_s32) \
    FN(ma_dr_wav_f64_to_s32) \
    FN(ma_dr_wav_read_pcm_frames_s32) \
    FN(ma_dr_wav_read_pcm_frames_s32le) \
    FN(ma_dr_wav_read_pcm_frames_s32be) \
    FN(ma_wav_read_pcm_frames) \
    FN(ma_dr_wav_alaw_to_s32) \
    FN(ma_dr_wav_mulaw_to_s32) \
    FN(ma_dr_wav_free) \
    FN(ma_dr_wav_bytes_to_u16) \
    FN(ma_dr_wav_bytes_to_s16) \
    FN(ma_dr_wav_bytes_to_u32) \
    FN(ma_dr_wav_bytes_to_f32) \
    FN(ma_dr_wav_bytes_to_s32) \
    FN(ma_dr_wav_bytes_to_u64) \
    FN(ma_dr_wav_bytes_to_s64) \
    FN(ma_dr_wav_guid_equal) \
    FN(ma_dr_wav_fourcc_equal) \
    FN(ma_dr_wav_init_ex) \
    FN(ma_wav_init) \
    FN(ma_dr_wav_init_with_metadata) \
    FN(ma_dr_wav_init_file_ex) \
    FN(ma_dr_wav_init_file_ex_w) \
    FN(ma_wav_init_file_w) \
    FN(ma_dr_wav_open_file_and_read_pcm_frames_s16_w) \
    FN(ma_dr_wav_open_file_and_read_pcm_frames_f32_w) \
    FN(ma_dr_wav_open_file_and_read_pcm_frames_s32_w) \
    FN(ma_dr_wav_init_file_with_metadata) \
    FN(ma_dr_wav_init_file_with_metadata_w) \
    FN(ma_dr_wav_init_file) \
    FN(ma_wav_init_file) \
    FN(ma_dr_wav_open_file_and_read_pcm_frames_s16) \
    FN(ma_dr_wav_open_file_and_read_pcm_frames_f32) \
    FN(ma_dr_wav_open_file_and_read_pcm_frames_s32) \
    FN(ma_dr_wav_init_file_w) \
    FN(ma_dr_wav_init_memory_ex) \
    FN(ma_dr_wav_init_memory) \
    FN(ma_wav_init_memory) \
    FN(ma_dr_wav_init_memory_with_metadata) \
    FN(ma_dr_wav_init) \
    FN(ma_dr_wav_open_and_read_pcm_frames_s16) \
    FN(ma_dr_wav_open_and_read_pcm_frames_f32) \
    FN(ma_dr_wav_open_and_read_pcm_frames_s32) \
    FN(ma_dr_wav_open_memory_and_read_pcm_frames_s16) \
    FN(ma_dr_wav_open_memory_and_read_pcm_frames_f32) \
    FN(ma_dr_wav_open_memory_and_read_pcm_frames_s32) \
    FN(ma_dr_flac_version) \
    FN(ma_dr_flac_version_string) \
    FN(ma_dr_flac_open_file) \
    FN(ma_flac_init_file) \
    FN(ma_dr_flac_open_file_w) \
    FN(ma_flac_init_file_w) \
    FN(ma_dr_flac_open_file_with_metadata) \
    FN(ma_dr_flac_open_file_with_metadata_w) \
    FN(ma_dr_flac_open_memory) \
    FN(ma_flac_init_memory) \
    FN(ma_dr_flac_open_memory_with_metadata) \
    FN(ma_dr_flac_open) \
    FN(ma_dr_flac_open_relaxed) \
    FN(ma_dr_flac_open_with_metadata) \
    FN(ma_dr_flac_open_with_metadata_relaxed) \
    FN(ma_dr_flac_close) \
    FN(ma_flac_uninit) \
    FN(ma_dr_flac_read_pcm_frames_s32) \
    FN(ma_dr_flac_read_pcm_frames_s16) \
    FN(ma_dr_flac_read_pcm_frames_f32) \
    FN(ma_dr_flac_seek_to_pcm_frame) \
    FN(ma_flac_seek_to_pcm_frame) \
    FN(ma_dr_flac_open_and_read_pcm_frames_s32) \
    FN(ma_dr_flac_open_and_read_pcm_frames_s16) \
    FN(ma_dr_flac_open_and_read_pcm_frames_f32) \
    FN(ma_dr_flac_open_file_and_read_pcm_frames_s32) \
    FN(ma_dr_flac_open_file_and_read_pcm_frames_s16) \
    FN(ma_dr_flac_open_file_and_read_pcm_frames_f32) \
    FN(ma_dr_flac_open_memory_and_read_pcm_frames_s32) \
    FN(ma_dr_flac_open_memory_and_read_pcm_frames_s16) \
    FN(ma_dr_flac_open_memory_and_read_pcm_frames_f32) \
    FN(ma_dr_flac_free) \
    FN(ma_dr_flac_init_vorbis_comment_iterator) \
    FN(ma_dr_flac_next_vorbis_comment) \
    FN(ma_dr_flac_init_cuesheet_track_iterator) \
    FN(ma_dr_flac_next_cuesheet_track) \
    FN(ma_dr_mp3_version) \
    FN(ma_dr_mp3_version_string) \
    FN(ma_dr_mp3dec_init) \
    FN(ma_dr_mp3dec_decode_frame) \
    FN(ma_mp3_read_pcm_frames) \
    FN(ma_dr_mp3dec_f32_to_s16) \
    FN(ma_dr_mp3_uninit) \
    FN(ma_dr_mp3_read_pcm_frames_f32) \
    FN(ma_dr_mp3_read_pcm_frames_s16) \
    FN(ma_dr_mp3_seek_to_pcm_frame) \
    FN(ma_mp3_seek_to_pcm_frame) \
    FN(ma_mp3_get_length_in_pcm_frames) \
    FN(ma_dr_mp3_get_mp3_and_pcm_frame_count) \
    FN(ma_dr_mp3_get_pcm_frame_count) \
    FN(ma_dr_mp3_get_mp3_frame_count) \
    FN(ma_dr_mp3_calculate_seek_points) \
    FN(ma_dr_mp3_bind_seek_table) \
    FN(ma_dr_mp3_malloc) \
    FN(ma_dr_mp3_init) \
    FN(ma_dr_mp3_init_memory_with_metadata) \
    FN(ma_mp3_init_memory) \
    FN(ma_dr_mp3_init_file_with_metadata) \
    FN(ma_mp3_init_file) \
    FN(ma_dr_mp3_open_file_and_read_pcm_frames_f32) \
    FN(ma_dr_mp3_open_file_and_read_pcm_frames_s16) \
    FN(ma_dr_mp3_init_file) \
    FN(ma_dr_mp3_init_file_with_metadata_w) \
    FN(ma_mp3_init_file_w) \
    FN(ma_dr_mp3_init_file_w) \
    FN(ma_dr_mp3_open_and_read_pcm_frames_f32) \
    FN(ma_dr_mp3_open_and_read_pcm_frames_s16) \
    FN(ma_mp3_init) \
    FN(ma_dr_mp3_open_memory_and_read_pcm_frames_f32) \
    FN(ma_dr_mp3_open_memory_and_read_pcm_frames_s16) \
    FN(ma_dr_mp3_init_memory) \
    FN(ma_dr_mp3_free) \
    FN(mp3dec_init) \
    FN(mp3dec_decode_frame) \
    FN(mp3dec_detect_cb) \
    FN(mp3dec_detect_buf) \
    FN(mp3dec_load_cb) \
    FN(mp3dec_load_buf) \
    FN(mp3dec_iterate_buf) \
    FN(mp3dec_iterate_cb) \
    FN(mp3dec_ex_open_buf) \
    FN(mp3dec_ex_seek) \
    FN(mp3dec_ex_read_frame) \
    FN(mp3dec_ex_read) \
    FN(mp3dec_ex_open_cb) \
    FN(mp3dec_detect) \
    FN(mp3dec_load) \
    FN(mp3dec_iterate) \
    FN(mp3dec_ex_close) \
    FN(mp3dec_ex_open) \
    FN(mp3dec_detect_w) \
    FN(mp3dec_load_w) \
    FN(mp3dec_iterate_w) \
    FN(mp3dec_ex_open_w)

/* Generate one PyCapsule-returning wrapper per entry. */
#define X(name) \
    static PyObject* py_fn_##name(PyObject* s, PyObject* a) { \
        (void)s; (void)a; \
        return PyCapsule_New((void*)(name), #name, NULL); \
    }
AUDIO_FN_LIST(X)
#undef X

/* ============================================================ */
/* Module definition                                            */
/* ============================================================ */
#define LOWAPI(name)  {"fn_" #name, py_fn_##name, METH_NOARGS, "fn_" #name "()"}

static PyMethodDef AudioMethods[] = {
    {"version", py_version, METH_NOARGS, "version"},
    {"decode_file", py_decode_file, METH_VARARGS, "decode_file(path)"},
    {"decode_memory", py_decode_memory, METH_VARARGS, "decode_memory(bytes)"},
    {"play_file", py_play_file, METH_VARARGS, "play_file(path)"},
    {"engine_init", py_engine_init, METH_NOARGS, "engine_init"},
    {"engine_uninit", py_engine_uninit, METH_NOARGS, "engine_uninit"},
    {"sound_load", py_sound_load, METH_VARARGS, "sound_load(path)"},
    {"sound_play", py_sound_play, METH_VARARGS, "sound_play(id)"},
    {"sound_stop", py_sound_stop, METH_VARARGS, "sound_stop(id)"},
    {"sound_pause", py_sound_pause, METH_VARARGS, "sound_pause(id)"},
    {"sound_resume", py_sound_resume, METH_VARARGS, "sound_resume(id)"},
    {"sound_unload", py_sound_unload, METH_VARARGS, "sound_unload(id)"},
    {"sound_set_volume", py_sound_set_volume, METH_VARARGS, "sound_set_volume(id,v)"},
    {"sound_get_volume", py_sound_get_volume, METH_VARARGS, "sound_get_volume(id)"},
    {"sound_set_looping", py_sound_set_looping, METH_VARARGS, "sound_set_looping(id,b)"},
    {"sound_is_playing", py_sound_is_playing, METH_VARARGS, "sound_is_playing(id)"},
    {"sound_seek", py_sound_seek, METH_VARARGS, "sound_seek(id,sec)"},
    {"sound_get_duration", py_sound_get_duration, METH_VARARGS, "sound_get_duration(id)"},
    {"sound_get_position", py_sound_get_position, METH_VARARGS, "sound_get_position(id)"},
    {"engine_set_volume", py_engine_set_volume, METH_VARARGS, "engine_set_volume(v)"},
    {"engine_stop_all", py_engine_stop_all, METH_NOARGS, "engine_stop_all"},
#define X(name) LOWAPI(name),
    AUDIO_FN_LIST(X)
#undef X
    {NULL, NULL, 0, NULL}
};

static struct PyModuleDef audio_module = {
    PyModuleDef_HEAD_INIT,
    "audio",
    "miniaudio + minimp3 Python extension (MIT-0).",
    -1,
    AudioMethods
};

PyMODINIT_FUNC PyInit_audio(void)
{
    return PyModule_Create(&audio_module);
}