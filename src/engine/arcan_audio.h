/*
 * Copyright 2003-2016, Björn Ståhl
 * License: 3-Clause BSD, see COPYING file in arcan source repository.
 * Reference: http://arcan-fe.com
 */

#ifndef _HAVE_ARCAN_AUDIO
#define _HAVE_ARCAN_AUDIO

/*
 * This part of the engine has received notably less attention, We've so- far
 * stuck with fixed format, fixed frequency etc.  Many of the more interesting
 * OpenAL bits (effects) are missing. The entire interface, buffer management
 * and platform abstraction is slated for rework in 0.6.
 */

enum aobj_kind {
	AOBJ_INVALID,
	AOBJ_STREAM,
	AOBJ_SAMPLE,
	AOBJ_FRAMESTREAM,
	AOBJ_CAPTUREFEED
};
struct arcan_aobj;

/*
 * Request a [buffer] to be filled using buffer_data. Provides a negative
 * value to indicate that the audio object is being destructed. [tag] is a a
 * caller-provided that used when creating the feed.  Expects  ARCAN_OK as
 * result to indicate that the buffer should be queued for playback.
 * if [cont] is set, more buffers will be provided if [ARCAN_OK] is
 * returned. Expects [ARCAN_ERRC_NOTREADY] to indicate that there is no more
 * data to feed. Any other error leads to cleanup / destruction.
 */
typedef arcan_errc(*arcan_afunc_cb)(struct arcan_aobj* aobj,
	ssize_t buffer, bool cont, void* tag);

/*
 * There is one global hook that can be used to get access to audio
 * data as it is beeing flushed to lower layers, and this is the form
 * of that callback.
 */
typedef void(*arcan_monafunc_cb)(arcan_aobj_id id, uint8_t* buf,
	size_t bytes, unsigned channels, unsigned frequency, void* tag);

/*
 * It is possible that the frameserver is a process parasite in another
 * process where we would like to interface audio control anyhow through
 * a gain proxy. This callback is used for those purposes.
 */
typedef arcan_errc(*arcan_again_cb)(float gain, void* tag);

/*
 * Setting nosound enforces a global silence, data will still be buffered
 * and monitoring etc. functions will work as usual.
 */
arcan_errc arcan_audio_setup(bool nosound);

/*
 * We have, unfortunately, seen a lot of driver- related issues with
 * these ones, so prefer buffering silence (or not buffering data at
 * all and have the engine put the mixing slot to better use).
 */
arcan_errc arcan_audio_suspend();
arcan_errc arcan_audio_resume();

/*
 * Process the list of active audio object and adjust time- based transforms,
 * e.g. changing pitch or volume.
 */
void arcan_audio_tick(uint8_t ntt);

/*
 * Process the list of active streaming audio sources and dequeue/refill
 * buffers as needed. Returns the number of sources with active buffers.
 */
size_t arcan_audio_refresh();

arcan_errc arcan_audio_shutdown();

/*
 * Add a hook to the feed functions of a specific audio ID, primarily
 * used for implementing audio recording of multiple sources.
 */
arcan_errc arcan_audio_hookfeed(arcan_aobj_id id,
	void* tag, arcan_monafunc_cb hookfun, void** oldtag);

/*
 * One-shot WAV- kind of samples. Internal caching etc, may apply.
 */
arcan_aobj_id arcan_audio_load_sample(
	const char* fname, float gain, arcan_errc* err);

/*
 * build an audio object from a preset normalised float buffer (-1..1) caller
 * retains ownership but buffer should be allocated through arcan_alloc_mem
 * with ARCAN_MEM_ABUFFER and page-alignment.
 *
 * elems refers to the count_of buffer, not the number of samples
 * (/= channels) and the packing format is always interleaved.
 *
 * fmt_specifier is reserved for future surround format support and any
 * provided specifier is ignored
 */
arcan_aobj_id arcan_audio_sample_buffer(float* buffer,
	size_t elems, int channels, int samplerate, const char* fmt_specifier);

/*
 * Alter the feed function associated with an audio object in a streaming
 * state.
 */
arcan_errc arcan_audio_alterfeed(arcan_aobj_id id, arcan_afunc_cb feed);

/*
 * Allocate a streaming audio source, tag is a regular context pointer
 * that will be passed to the corresponding callback / feed function.
 * These will likely be invoked as part of audio_refresh.
 */
arcan_aobj_id arcan_audio_feed(arcan_afunc_cb feed,
	void* tag, arcan_errc* errc);

/*
 * Get the underlying type associated with an audio object.
 */
enum aobj_kind arcan_audio_kind(arcan_aobj_id);

/* destroy an audio object and everything associated with it */
arcan_errc arcan_audio_stop(arcan_aobj_id);

/* initiate playback of a sample buffer or stream (i.e. push buffers to OpenAL),
 * if [gain] override is set the device gain will be ignored in favor of [gain]
 *
 * if the [id] refers to a sample, and [tag] is set to >= 0, an event will be
 * emitted direct-to-drain when the buffer has been finished according to the
 * audio stack, with the [tag] set as the [otag] member of the event structure
 */
arcan_errc arcan_audio_play(
	arcan_aobj_id, bool gain_override, float gain, intptr_t tag);

/* Pause might not work satisfactory. If this starts acting weird,
 * consider using the rebuild hack above */
arcan_errc arcan_audio_pause(arcan_aobj_id);

/* Might only be applicable for some aobjs */
arcan_errc arcan_audio_rewind(arcan_aobj_id);

/* get a null- terminated list of available capture devices */
char** arcan_audio_capturelist();

/*
 * try and get a lock on a specific capture device
 * (matching arcan_audio_capturelist),
 * actual sampled data is dropped silently
 * unless there's a monitor attached
 */
arcan_aobj_id arcan_audio_capturefeed(const char* identifier);

/*
 * update the gain value for a source either immediately (time == 0)
 * or gradually over [time] ticks. Multiple calls with [time > 0] will
 * queue additional transformations. A single call with [time == 0] will
 * always reset any current chain.
 *
 * calling setgain on [id == 0] will change the default value for
 * new sources, and the [time] argument will be ignored.
 */
arcan_errc arcan_audio_setgain(arcan_aobj_id id, float gain, uint16_t time);

/*
 * Retrieve the current gain value (and, if possible, the target)
 * and store in [cgain] (if !NULL)
 *
 * calling getgain on [id == 0] will retrieve the global default for
 * new sources.
 *
 * If the function fails (retv != ARCAN_OK), [cgain] and [dgain] will be
 * left unchanged.
 */
arcan_errc arcan_audio_getgain(arcan_aobj_id id, float* cgain);

/*
 * This function is used similarly to the collapse/adopt style
 * functions in the video subsystem. If the scripting / execution layer
 * fails for some reason, we want to keep the audio objects that
 * are associated with frameservers and leave any samples etc. to rot.
 */
void arcan_audio_purge(arcan_aobj_id* save, size_t save_count);

#endif
