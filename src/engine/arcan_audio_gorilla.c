/*
 * Copyright 2020, Elijah Stone
 * License: 3-Clause BSD, see COPYING file in arcan source repository.
 * Reference: http://arcan-fe.com
 * Description: audio management code (just basic buffering, gain, ...)
 */

#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <strings.h>
#include <fcntl.h>
#include <sys/types.h>
#include <assert.h>
#include <limits.h>
#include <inttypes.h>
#include <pthread.h>

#include <gorilla/ga.h>
#include <gorilla/gau.h>

#include "arcan_math.h"
#include "arcan_general.h"
#include "arcan_shmif.h"
#include "arcan_video.h"
#include "arcan_audio.h"
#include "arcan_audioint.h"
#include "arcan_event.h"

struct arcan_acontext {
/* linked list of audio sources, the number of available sources are platform /
 * hw dependant, ranging between 10-100 or so */
	arcan_aobj *first;
	GauManager *manager;

	bool ga_active;

	arcan_aobj_id lastid;
	float def_gain;

/* limit on amount of simultaneous active sources */
	GaHandle *sample_sources[ARCAN_AUDIO_SLIMIT];
	intptr_t sample_tags[ARCAN_AUDIO_SLIMIT];

	arcan_tickv atick_counter;

	arcan_monafunc_cb globalhook;
	void* global_hooktag;
};

static bool gacheck(ga_result res,arcan_aobj*, char*);


static struct arcan_acontext _current_acontext = { .def_gain = 1.0 };
static struct arcan_acontext* current_acontext = &_current_acontext;

static arcan_aobj* arcan_audio_getobj(arcan_aobj_id);
static arcan_errc audio_free(arcan_aobj_id);

static arcan_aobj_id arcan_audio_alloc(arcan_aobj** dst) {
	arcan_aobj_id rv = ARCAN_EID;
	if (dst)
		*dst = NULL;

	arcan_aobj* newcell = arcan_alloc_mem(sizeof(arcan_aobj), ARCAN_MEM_ATAG,
		ARCAN_MEM_BZERO, ARCAN_MEMALIGN_NATURAL);

	newcell->gain = current_acontext->def_gain;

/* unlikely event of wrap-around */
	newcell->id = current_acontext->lastid++;
	if (newcell->id == ARCAN_EID)
		newcell->id = 1;

	if (dst)
		*dst = newcell;

	if (current_acontext->first){
		arcan_aobj* current = current_acontext->first;
		while(current && current->next)
			current = current->next;

		current->next = newcell;
	}
	else
		current_acontext->first = newcell;

	return newcell->id;
}

static arcan_aobj* arcan_audio_getobj(arcan_aobj_id id)
{
	arcan_aobj* current = current_acontext->first;

	while (current){
		if (current->id == id)
			return current;

		current = current->next;
	}

	return NULL;
}

arcan_errc arcan_audio_alterfeed(arcan_aobj_id id, arcan_afunc_cb cb)
{
	arcan_errc rv = ARCAN_ERRC_NO_SUCH_OBJECT;
	arcan_aobj* obj = arcan_audio_getobj(id);

	if (obj) {
		if (!cb)
			rv = ARCAN_ERRC_BAD_ARGUMENT;
		else {
			obj->feed = cb;
			rv = ARCAN_OK;
		}
	}

	return rv;
}

static arcan_errc audio_free(arcan_aobj_id id)
{
	arcan_errc rv = ARCAN_ERRC_NO_SUCH_OBJECT;
	arcan_aobj* current = current_acontext->first;
	arcan_aobj** owner = &(current_acontext->first);

 /* find */
	while(current && current->id != id){
		owner = &(current->next);
		current = current->next;
	}

 /* if found, delink */
	if (current){
		*owner = current->next;

		if (current->handle){
			gacheck(ga_handle_stop(current->handle), NULL, "audio_free(stop)");
			gacheck(ga_handle_destroy(current->handle), NULL, "audio_free(destroy)");
			current->handle = NULL;

			for (size_t i = 0; i < current->n_streambuf; i++)
				ga_sample_source_release(current->streambuf[i]);
		}
		current->next = (void*) 0xdeadbeef;
		current->tag = (void*) 0xdeadbeef;
		current->feed = NULL;
		arcan_mem_free(current);

		rv = ARCAN_OK;
	}

	return rv;
}

arcan_errc arcan_audio_setup(bool nosound)
{
/* don't supported repeated calls without shutting down in between */
	if (!current_acontext || current_acontext->manager) return ARCAN_ERRC_NOAUDIO;

	current_acontext->manager = gau_manager_create_custom(&(GaDeviceType){nosound ? GaDeviceType_Dummy : GaDeviceType_Default}, GauThreadPolicy_Multi, &(uint32_t){4}, &(uint32_t){512});
	if (!current_acontext->manager) return ARCAN_ERRC_NOAUDIO;

	current_acontext->ga_active = true;

	/* just give a slightly "random" base so that
	 * user scripts don't get locked into hard-coded ids .. */
	current_acontext->lastid = rand() % 32768;

	return ARCAN_OK;
}

arcan_errc arcan_audio_shutdown()
{
	if (!current_acontext->manager) return ARCAN_OK;

/* there might be more to clean-up here, monitoring /callback buffers/tags */

	current_acontext->ga_active = false;
	gau_manager_destroy(current_acontext->manager);
	current_acontext->manager = NULL;
	memset(current_acontext->sample_sources,
		0, sizeof(current_acontext->sample_sources));

	return ARCAN_OK;
}

static void handle_done_cb(GaHandle *handle, void *ctx) {
	arcan_aobj *current = ctx;
	current->active = false;
	ga_handle_destroy(current->handle);
	current->handle = NULL;
	arcan_event newevent = {
		.category = EVENT_AUDIO,
		.aud.kind = EVENT_AUDIO_PLAYBACK_FINISHED,
		.aud.source = current->id,
	};

	/* enqueue direct into drain, this might invoke audio callback on the scripting
	 * side in order to immediately chain the playback of another sample */
	arcan_event_denqueue(arcan_event_defaultctx(), &newevent);
}

arcan_errc arcan_audio_play(arcan_aobj_id id, bool gain_override, float gain, intptr_t tag) {
	arcan_aobj* aobj = arcan_audio_getobj(id);

	if (!aobj)
		return ARCAN_ERRC_NO_SUCH_OBJECT;

	/* for aobj sample, just find a free sample slot (if any) and
	 * attach the buffer already part of the aobj */
	if (false && aobj->kind == AOBJ_SAMPLE){
#if 0
		for (size_t i = 0; i < ARCAN_AUDIO_SLIMIT; i++)
			if (current_acontext->sample_sources[i] == NULL) {
				// only one stream per aobj atm
				current_acontext->sample_sources[i] = ga_handle_create(mixer, gau_sample_source_create_sound(aobj->streambuf[0]));
				current_acontext->sample_tags[i] = tag;
				gacheck(ga_handle_set_paramf(current_acontext->sample_sources[i], GaHandleParam_Gain, gain_override ? gain : aobj->gain), aobj, "load_sample(set_paramf)");

				gacheck(ga_handle_play(current_acontext->sample_source[i]), aobj, "load_sample(play)");
				break;
			}
#endif
	/* some kind of streaming source, can't play if it is already active */
	} else if (aobj->active == false && aobj->handle == NULL) {
		// only one stream per aobj atm
		aobj->handle = gau_create_handle_buffered_samples(current_acontext->manager, aobj->streambuf[0], handle_done_cb, aobj, NULL);
		if (!aobj->handle) {
			arcan_warning("(gorilla audio): could not create handle");
			return ARCAN_ERRC_OUT_OF_SPACE; //?
		}
		gacheck(ga_handle_set_paramf(aobj->handle, GaHandleParam_Gain, gain_override ? gain : aobj->gain), aobj, "load_stream(set_paramf)");

		gacheck(ga_handle_play(aobj->handle), aobj, "load_stream(play)");
		aobj->active = true;
	} else if (aobj->handle) {
		gacheck(ga_handle_play(aobj->handle), aobj, "load_stream(play)");
		aobj->active = true;
	} else {
		return ARCAN_ERRC_BAD_ARGUMENT;
	}

	return ARCAN_OK;
}

static int16_t float_s16(float val) {
	if (val < 0.0)
		return -val * -32768.0;
	else
		return val * 32767.0;
}

arcan_aobj_id arcan_audio_sample_buffer(float* buffer,
	size_t elems, int channels, int samplerate, const char* fmt_specifier) {
	if (!buffer || !elems || channels <= 0 || channels > 2 || elems % channels != 0)
		return ARCAN_EID;

	arcan_aobj* aobj;
	arcan_aobj_id rid = arcan_audio_alloc(&aobj);

	if (rid == ARCAN_EID)
		return ARCAN_EID;

	GaMemory *pcm = ga_memory_create(NULL, elems * sizeof(int16_t));
	int16_t *samplebuf = ga_memory_data(pcm);

	for (size_t i = 0; i < elems; i++){
		samplebuf[i] = float_s16(buffer[i]);
	}

	GaSound *sound = ga_sound_create(pcm, &(GaFormat){.sample_rate=samplerate, .bits_per_sample=16, .num_channels=channels});
	ga_memory_release(pcm);
	GaSampleSource *ssrc = gau_sample_source_create_sound(sound);
	ga_sound_release(sound);

	aobj->kind = AOBJ_SAMPLE;
	aobj->gain = 1.0;
	aobj->n_streambuf = 1;
	aobj->streambuf[0] = ssrc;
	aobj->used = 1;

	return rid;
}

arcan_aobj_id arcan_audio_load_sample(
	const char* fname, float gain, arcan_errc* err)
{
	if (fname == NULL)
		return ARCAN_EID;

	arcan_aobj* aobj;
	arcan_aobj_id rid = arcan_audio_alloc(&aobj);

	if (rid == ARCAN_EID){
		if (err) *err = ARCAN_ERRC_OUT_OF_SPACE;
		return ARCAN_EID;
	}

	// todo use arcan_open_resource and map data_source to GaDataSource
	// todo add another function that does i/o buffering, as in:
	//GaHandle *handle = gau_create_handle_buffered_file(current_acontext->manager, fname, GauAudioType_Wav, handle_done_cb, aobj, NULL);
	GaSound *sound = gau_load_sound_file(fname, GauAudioType_Wav);
	if (!sound) {
		if (err) *err = ARCAN_ERRC_BAD_RESOURCE;
		return ARCAN_EID;
	}

	GaSampleSource *ssrc = gau_sample_source_create_sound(sound);
	ga_sound_release(sound);
	if (!ssrc) {
		// create_handle_sound only fails on allocation failure
		if (err) *err = ARCAN_ERRC_OUT_OF_SPACE;
		return ARCAN_EID;
	}

	aobj->kind = AOBJ_SAMPLE;
	aobj->gain = gain;
	aobj->n_streambuf = 1;
	aobj->streambuf[0] = ssrc;
	aobj->used = 1;

	if (err) *err = ARCAN_OK;

	return rid;
}

arcan_errc arcan_audio_hookfeed(arcan_aobj_id id, void* tag,
	arcan_monafunc_cb hookfun, void** oldtag)
{
	arcan_aobj* aobj = arcan_audio_getobj(id);
	if (!aobj)
		return ARCAN_ERRC_NO_SUCH_OBJECT;

	if (oldtag)
		*oldtag = aobj->monitortag ? aobj->monitortag : NULL;

	aobj->monitor = hookfun;
	aobj->monitortag = tag;

	return ARCAN_OK;
}

arcan_aobj_id arcan_audio_feed(arcan_afunc_cb feed, void* tag, arcan_errc* errc)
{
	arcan_aobj* aobj;
	arcan_aobj_id rid = arcan_audio_alloc(&aobj);
	if (!aobj){
		if (errc) *errc = ARCAN_ERRC_OUT_OF_SPACE;
		return ARCAN_EID;
	}

/* the id will be allocated when we first get data as there
 * is a limit to how many streaming / mixed sources we can support */
	aobj->handle = NULL;
	aobj->streaming = true;
	aobj->tag = tag;
	aobj->n_streambuf = ARCAN_ASTREAMBUF_LIMIT;
	aobj->feed = feed;
	aobj->gain = 1.0;
	aobj->kind = AOBJ_STREAM;

	if (errc) *errc = ARCAN_OK;
	return rid;
}

enum aobj_kind arcan_audio_kind(arcan_aobj_id id)
{
	arcan_aobj* aobj = arcan_audio_getobj(id);
	return aobj ? aobj->kind : AOBJ_INVALID;
}

arcan_errc arcan_audio_suspend()
{
	ga_mixer_suspend(gau_manager_mixer(current_acontext->manager));
	current_acontext->ga_active = false;

	return ARCAN_OK;
}

arcan_errc arcan_audio_resume()
{
	ga_mixer_unsuspend(gau_manager_mixer(current_acontext->manager));
	current_acontext->ga_active = true;

	return ARCAN_OK;
}

arcan_errc arcan_audio_pause(arcan_aobj_id id)
{
	arcan_aobj* dobj = arcan_audio_getobj(id);
	arcan_errc rv = ARCAN_ERRC_NO_SUCH_OBJECT;

	if (dobj && dobj->handle) {
		gacheck(ga_handle_stop(dobj->handle), dobj, "audio_pause(get/unqueue/stop)");
		dobj->active = false;
		rv = ARCAN_OK;
	}

	return rv;
}

arcan_errc arcan_audio_stop(arcan_aobj_id id)
{
	arcan_aobj* dobj = arcan_audio_getobj(id);
	if (!dobj)
		return ARCAN_ERRC_NO_SUCH_OBJECT;

	dobj->kind = AOBJ_INVALID;
	dobj->feed = NULL;

	audio_free(id);

	arcan_event newevent = {
		.category = EVENT_AUDIO,
		.aud.kind = EVENT_AUDIO_OBJECT_GONE,
		.aud.source = id
	};

	arcan_event_enqueue(arcan_event_defaultctx(), &newevent);
	return ARCAN_OK;
}

static inline void reset_chain(arcan_aobj* dobj)
{
	struct arcan_achain* current = dobj->transform;
	struct arcan_achain* next;

	while (current) {
		next = current->next;
		arcan_mem_free(current);
		current = next;
	}

	dobj->transform = NULL;
}

arcan_errc arcan_audio_getgain(arcan_aobj_id id, float* gain)
{
	if (id == ARCAN_EID){
		if (gain)
			*gain = current_acontext->def_gain;
		return ARCAN_OK;
	}

	arcan_aobj* dobj = arcan_audio_getobj(id);

	if (!dobj)
		return ARCAN_ERRC_NO_SUCH_OBJECT;

	if (gain)
		*gain = dobj->gain;

	return ARCAN_OK;
}

arcan_errc arcan_audio_setgain(arcan_aobj_id id, float gain, uint16_t time)
{
	if (id == ARCAN_EID){
		current_acontext->def_gain = gain;
		return ARCAN_OK;
	}

	arcan_aobj* dobj = arcan_audio_getobj(id);

	if (!dobj)
		return ARCAN_ERRC_NO_SUCH_OBJECT;

/* immediately */
	if (time == 0) {
		reset_chain(dobj);
		dobj->gain = gain;

		if (dobj->gproxy)
			dobj->gproxy(dobj->gain, dobj->tag);
		else if (dobj->handle)
			gacheck(ga_handle_set_paramf(dobj->handle, GaHandleParam_Gain, gain), dobj, "audio_setgain(set_param)");
	} else {
		struct arcan_achain** dptr = &dobj->transform;

		while(*dptr){
			dptr = &(*dptr)->next;
		}

		*dptr = arcan_alloc_mem(sizeof(struct arcan_achain),
			ARCAN_MEM_ATAG, 0, ARCAN_MEMALIGN_NATURAL);

		(*dptr)->next = NULL;
		(*dptr)->t_gain = time;
		(*dptr)->d_gain = gain;
	}

	return ARCAN_OK;
}

static ssize_t find_freebufferind(arcan_aobj* cur, bool tag){
	for (size_t i = 0; i < cur->n_streambuf; i++){
		if (cur->streambufmask[i] == false){
			if (tag){
				cur->used++;
				cur->streambufmask[i] = true;
			}

			return i;
		}
	}

	return -1;
}

void arcan_audio_buffer(arcan_aobj* aobj, ssize_t buffer, void* audbuf,
	size_t abufs, unsigned int channels, unsigned int samplerate, void* tag)
{
/*
 * even if the AL subsystem should fail, our monitors and globalhook
 * can still work (so record, streaming etc. doesn't cascade)
 */
	if (aobj->monitor)
		aobj->monitor(aobj->id, audbuf, abufs, channels,
			samplerate, aobj->monitortag);

	if (current_acontext->globalhook)
		current_acontext->globalhook(aobj->id, audbuf, abufs, channels,
			samplerate, current_acontext->global_hooktag);

/*
 * the audio system can bounce back in the case of many allocations
 * exceeding what can be mixed internally, through the _tick mechanism
 * keeping track of which sources that are actively in use and freeing
 * up those that havn't seen any use for a while.
 */
	//todo need more in arcan_aobj
#if 0
	if (!aobj->handle){
		alGenSources(1, &aobj->alid);
		alGenBuffers(aobj->n_streambuf, aobj->streambuf);
		alSourcef(aobj->alid, AL_GAIN, aobj->gain);

		alSourceQueueBuffers(aobj->alid, 1, &aobj->streambuf[0]);
		aobj->streambufmask[0] = true;
		aobj->used++;
		alSourcePlay(aobj->alid);

	_wrap_alError(NULL, "audio_feed(genBuffers)");
	}
	else if (aobj->gproxy == false){
		aobj->last_used = current_acontext->atick_counter;
		alBufferData(buffer, channels == 2 ? AL_FORMAT_STEREO16 :
			AL_FORMAT_MONO16, audbuf, abufs, samplerate);
	}
#endif
}

int arcan_audio_findstreambufslot(arcan_aobj_id id)
{
	arcan_aobj* aobj = arcan_audio_getobj(id);
	return aobj ? find_freebufferind(aobj, true) : -1;
}

static void astream_refill(arcan_aobj* current) {
	if (!current->handle && current->feed){
		current->feed(current, 0, false, current->tag);
		return;
	}
}

void arcan_aid_refresh(arcan_aobj_id aid)
{
	struct arcan_aobj* obj = arcan_audio_getobj(aid);
	if (obj)
		astream_refill(obj);
}

// capture is not (yet) supported by ga
char** arcan_audio_capturelist()
{
	return NULL;
}

arcan_aobj_id arcan_audio_capturefeed(const char* dev)
{
	return ARCAN_EID;
}

size_t arcan_audio_refresh()
{
	if (!current_acontext->manager || !current_acontext->ga_active)
		return 0;

	arcan_aobj* current = current_acontext->first;
	size_t rv = 0;

	while(current){
		if (
			current->kind == AOBJ_STREAM      ||
			current->kind == AOBJ_FRAMESTREAM ||
			current->kind == AOBJ_CAPTUREFEED
		)
			astream_refill(current);

		if (current->used)
			rv++;

		current = current->next;
	}

	return rv;
}

static inline bool step_transform(arcan_aobj* obj)
{
	if (obj->transform == NULL)
		return false;

/* OpenAL maps dB to linear */
	obj->gain += (obj->transform->d_gain - obj->gain) /
		(float) obj->transform->t_gain;

	obj->transform->t_gain--;
	if (obj->transform->t_gain == 0){
		obj->gain = obj->transform->d_gain;
		struct arcan_achain* ct = obj->transform;
		obj->transform = obj->transform->next;
		free(ct);
	}

	return true;
}

void arcan_audio_tick(uint8_t ntt)
{
/*
 * scan list of allocated IDs and update buffers for all streaming / cb
 * functions, also make sure our context is the current active one,
 * flush error buffers etc.
 */
	if (!current_acontext->manager || !current_acontext->ga_active)
		return;

	arcan_audio_refresh();

/* update time-dependent transformations */
	while (ntt-- > 0) {
		for (arcan_aobj* current = current_acontext->first; current; current = current->next) {
			if (step_transform(current)){
				if (current->gproxy)
					current->gproxy(current->gain, current->tag);
				else if (current->handle){
					gacheck(ga_handle_set_paramf(current->handle, GaHandleParam_Gain, current->gain), current, "audio_tick(set_paramf/gain)");
				}
			}
		}
	}

/* scan all streaming buffers and free up those no-longer needed */
	for (size_t i = 0; i < ARCAN_AUDIO_SLIMIT; i++)
	if (current_acontext->sample_sources[i]) {
		if (!ga_handle_playing(current_acontext->sample_sources[i])) {
			gacheck(ga_handle_destroy(current_acontext->sample_sources[i]), NULL, "audio_tick(handle_destroy)");
			current_acontext->sample_sources[i] = NULL;

			if (current_acontext->sample_tags[i] != 0){
				arcan_event_enqueue(arcan_event_defaultctx(),
				&(struct arcan_event){
					.category = EVENT_AUDIO,
					.aud.kind = EVENT_AUDIO_PLAYBACK_FINISHED,
					.aud.otag = current_acontext->sample_tags[i]
				});
				current_acontext->sample_tags[i] = 0;
			}
		}
	}

	gau_manager_update(current_acontext->manager);
}

/*
 * very inefficient, but the set of IDs to delete is reasonably small
 */
void arcan_audio_purge(arcan_aobj_id* ids, size_t nids)
{
	arcan_aobj* current = _current_acontext.first;
	arcan_aobj** previous = &_current_acontext.first;

	while(current){
		bool match = false;

		for (size_t i = 0; i < nids; i++){
			if (ids[i] == current->id){
				match = true;
				break;
			}
		}

		arcan_aobj* next = current->next;
		if (!match){
			(*previous) = next;
			if (current->feed)
				current->feed(current, -1, false, current->tag);

			if (current->handle){
				ga_handle_stop(current->handle);
				ga_handle_destroy(current->handle);
				current->handle = NULL;

				for (size_t i = 0; i < current->n_streambuf; i++)
					ga_sample_source_release(current->streambuf[i]);
				
			}

			arcan_mem_free(current);
		}
		else {
			previous = &current->next;
		}

		current = next;
	}
}

static bool gacheck(ga_result res, arcan_aobj* obj, char* prefix)
{
	static arcan_aobj empty = {0};
	if (!obj)
		obj = &empty;

#ifndef _DEBUG
	return res == GA_OK;
#endif

	if (res != GA_OK) {
		arcan_warning("(gorilla audio): ");

		switch (res) {
		case GA_ERR_GENERIC:
			arcan_warning("(%u:%p), %s - generic error\n", obj->id,
				obj->handle, prefix);
			break;
		default:
			arcan_warning("(%u:%p), %s - undefined error\n", obj->id,
				obj->handle, prefix);
		}
		return false;
	}

	return true;
}
