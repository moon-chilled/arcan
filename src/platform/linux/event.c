/*
 * Copyright 2014-2015, Björn Ståhl
 * License: 3-Clause BSD, see COPYING file in arcan source repository.
 * Reference: http://arcan-fe.com
 */

/*
 * a lot of the filtering here is copied of platform/sdl/event.c, as it is
 * scheduled for deprecation, we've not bothered designing an interface for the
 * axis bits to be shared. For future refactoring, the basic signalling
 * processing, e.g. determining device orientation from 3-sensor + Kalman,
 * user-configurable analog filters on noisy devices etc. should be generalized
 * and put in a shared directory, and re-used for other input platforms.
 */

#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <assert.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <stdio.h>
#include <errno.h>
#include <poll.h>
#include <glob.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include <linux/input.h>

#include "arcan_shmif.h"
#include "arcan_math.h"
#include "arcan_general.h"
#include "arcan_event.h"
#include "arcan_video.h"
#include "arcan_videoint.h"
#include "keycode_xlate.h"

#include <linux/vt.h>
#include <linux/major.h>
#include <linux/kd.h>
#include <signal.h>
#include <sys/inotify.h>

/*
 * scan / probe a node- dir (ENVV overridable)
 */
#ifndef NOTIFY_SCAN_DIR
#define NOTIFY_SCAN_DIR "/dev/input"
#endif

static const char* notify_scan_dir = NOTIFY_SCAN_DIR;
static bool log_verbose = false;

static struct {
	unsigned long kbmode;
	int mode;
	unsigned char leds;
	bool mute;
	int tty, notify;
}
gstate = {
	.mode = KD_TEXT,
	.tty = STDIN_FILENO,
	.notify = -1
};

static const char* envopts[] = {
	"ARCAN_INPUT_NOMUTETTY", "Don't disable terminal or SIGINT",
	"ARCAN_INPUT_SCANDIR", "Directory to monitor for device nodes "
		"(Default: "NOTIFY_SCAN_DIR")",
	"ARCAN_INPUT_TTYOVERRIDE", "Force a specific tty- device",
	"ARCAN_INPUT_VERBOSE", "_warning log() input node events",
	NULL
};

/*
 * need a reasonable limit on the amount of allowed devices, should this become
 * a problem -- whitelist. See lookup_devnode for an explanation on the problem
 * with devid-.
 */
#define MAX_DEVICES 256

struct arcan_devnode;
#include "device_db.h"

struct axis_opts {
/* none, avg, drop */
	enum ARCAN_ANALOGFILTER_KIND mode;
	enum ARCAN_ANALOGFILTER_KIND oldmode;

	int lower, upper, deadzone;

/* we won't get access to a good range distribution if we don't emit the first
 * / last sample that got into the drop range */
	bool inlzone, inuzone, indzone;

	int kernel_sz;
	int kernel_ofs;
	int32_t flt_kernel[64];
};

static struct {
	size_t n_devs, sz_nodes;

/* repeat is currently enforced uniformly across all keyboards, might be
 * usecases where this is not preferable but there is no higher-level api
 * that provides this granularity. */
	unsigned period, delay;

	unsigned short mouseid;
	struct arcan_devnode* nodes;

	struct pollfd* pollset;
} iodev = {0};

struct arcan_devnode {
	int handle;

/* NULL&size terminated, with chain-block set of the previous one could not
 * handle. This is to cover devices that could expose themselves as being
 * aggregated KEY/DEV/etc. */
	struct evhandler hnd;

	char label[256];
	unsigned short devnum;
	size_t button_count;

	enum devnode_type type;
	union {
		struct {
			struct axis_opts data;
		} sensor;
		struct {
			unsigned short axes;
			unsigned short buttons;
			char hats[16];
			struct axis_opts* adata;
		} game;
		struct {
			uint16_t mx;
			uint16_t my;
			struct axis_opts flt[2];
		} cursor;
		struct {
			unsigned state;
			bool numlock;
			bool capslock;
			bool scrolllock;
		} keyboard;
		struct {
			bool incomplete;
		} touch;
	};
};

static void got_device(int fd, const char*);

/* for other platforms and legacy, devid used to be allocated sequentially
 * and swept linear, even though this platform do not work like that and we
 * have a dynamic set of devices. For this reason, we split the 16 bit space
 * into < MAX_DEVICES and >= MAX_DEVICES and a device a can be accessed by
 * either id */
static struct arcan_devnode* lookup_devnode(int devid)
{
	if (devid < 0)
		devid = iodev.mouseid;

	if (devid < iodev.n_devs)
		return &iodev.nodes[devid];

	for (size_t i = 0; i < iodev.n_devs; i++){
		if (iodev.nodes[i].devnum == devid)
			return &iodev.nodes[i];
	}

	return NULL;
}

/* another option to this mess (as the hashing thing doesn't seem to work out
 * is to move identification/etc. to another level and just let whatever device
 * node generator is active populate with coherent names. and use a hash of that
 * name as the ID */
static bool identify(int fd, const char* path,
	char* label, size_t label_sz, unsigned short* dnum)
{
	if (-1 == ioctl(fd, EVIOCGNAME(label_sz), label)){
		if (log_verbose)
			arcan_warning("input/identify: bad EVIOCGNAME, setting unknown\n");
		snprintf(label, label_sz, "unknown");
	}
	else
		if (log_verbose)
			arcan_warning("input/identify(%d): %s name resolved to %s\n",
				fd, path, label);

	struct input_id nodeid;
	if (-1 == ioctl(fd, EVIOCGID, &nodeid)){
		arcan_warning("input/identify(%d): no EVIOCGID, "
			"reason:%s\n", fd, strerror(errno));
		return false;
	}

/* didn't find much on how unique eviocguniq actually was, nor common lengths
 * or what not so just mix them in a buffer, hash and let unsigned overflow
 * modulo take us down to 16bit */
	size_t bpl = sizeof(long) * 8;
	size_t nbits = ((EV_MAX)-1) / bpl + 1;

	char buf[nbits * sizeof(long)];
	char bbuf[sizeof(buf)];
	memset(buf, '\0', sizeof(buf));
	memset(bbuf, '\0', sizeof(bbuf));

/* some test devices here answered to the ioctl and returned full empty UNIQs,
 * do something to lower the likelihood of collisions */
	unsigned long hash = 5381;

	if (-1 == ioctl(fd, EVIOCGUNIQ(sizeof(buf)), buf) ||
		memcmp(buf, bbuf, sizeof(buf)) == 0){

		size_t llen = strlen(label);
		for (size_t i = 0; i < llen; i++)
			hash = ((hash << 5) + hash) + label[i];

		llen = strlen(path);
		for (size_t i = 0; i < llen; i++)
			hash  = ((hash << 5) + hash) + path[i];

	 	buf[11] ^= nodeid.vendor >> 8;
		buf[10] ^= nodeid.vendor;
		buf[9] ^= nodeid.product >> 8;
		buf[8] ^= nodeid.product;
		buf[7] ^= nodeid.version >> 8;
		buf[6] ^= nodeid.version;

/* even this point has a few collisions, particularly some keyboards and mice
 * that don't respond to CGUNIQ and expose multiple- subdevices but with
 * different button/axis count */
		ioctl(fd, EVIOCGBIT(0, EV_MAX), &buf);
	}

	for (size_t i = 0; i < sizeof(buf); i++)
		hash = ((hash << 5) + hash) + buf[i];

/* 16-bit clamp is legacy in the scripting layer */
	unsigned short devnum = hash;
	if (devnum < MAX_DEVICES)
		devnum += MAX_DEVICES;

	*dnum = devnum;

	return true;
}

static inline bool process_axis(struct arcan_evctx* ctx,
	struct axis_opts* daxis, int16_t samplev, int16_t* outv)
{
	if (daxis->mode == ARCAN_ANALOGFILTER_NONE)
		return false;

	if (daxis->mode == ARCAN_ANALOGFILTER_PASS)
		goto accept_sample;

/* quickfilter deadzone */
	if (abs(samplev) < daxis->deadzone){
		if (!daxis->indzone){
			samplev = 0;
			daxis->indzone = true;
		}
		else
			return false;
	}
	else
		daxis->indzone = false;

/* quickfilter out controller edgenoise */
	if (samplev < daxis->lower){
		if (!daxis->inlzone){
			samplev = daxis->lower;
			daxis->inlzone = true;
			daxis->inuzone = false;
		}
		else
			return false;
	}
	else if (samplev > daxis->upper){
		if (!daxis->inuzone){
			samplev = daxis->upper;
			daxis->inuzone = true;
			daxis->inlzone = false;
		}
		else
			return false;
	}
	else
		daxis->inlzone = daxis->inuzone = false;

	daxis->flt_kernel[ daxis->kernel_ofs++ ] = samplev;

/* don't proceed until the kernel is filled */
	if (daxis->kernel_ofs < daxis->kernel_sz)
		return false;

	if (daxis->kernel_sz > 1){
		int32_t tot = 0;

		if (daxis->mode == ARCAN_ANALOGFILTER_ALAST){
			samplev = daxis->flt_kernel[daxis->kernel_sz - 1];
		}
		else {
			for (int i = 0; i < daxis->kernel_sz; i++)
				tot += daxis->flt_kernel[i];

			samplev = tot != 0 ? tot / daxis->kernel_sz : 0;
		}

	}
	else;
	daxis->kernel_ofs = 0;

accept_sample:
	*outv = samplev;
	return true;
}

static void set_analogstate(struct axis_opts* dst,
	int lower_bound, int upper_bound, int deadzone,
	int kernel_size, enum ARCAN_ANALOGFILTER_KIND mode)
{
	dst->lower = lower_bound;
	dst->upper = upper_bound;
	dst->deadzone = deadzone;
	dst->kernel_sz = kernel_size;
	dst->mode = mode;

	dst->kernel_ofs = 0;
}

static struct axis_opts* find_axis(int devid, unsigned axisid, bool* outn)
{
	struct arcan_devnode* node = lookup_devnode(devid);
	*outn = node != NULL;

	if (!node)
		return NULL;

	switch(node->type){
	case DEVNODE_SENSOR:
		return axisid == 0 ? &node->sensor.data : NULL;
	break;

	case DEVNODE_GAME:
		if (axisid < node->game.axes)
			return &node->game.adata[axisid];
	break;

	case DEVNODE_MOUSE:
		if (axisid == 0)
			return &node->cursor.flt[0];
		else if (axisid == 1)
			return &node->cursor.flt[1];
	break;

	default:
	break;
	}

	return NULL;
}

arcan_errc platform_event_analogstate(int devid, int axisid,
	int* lower_bound, int* upper_bound, int* deadzone,
	int* kernel_size, enum ARCAN_ANALOGFILTER_KIND* mode)
{
	bool gotnode;
	struct axis_opts* axis = find_axis(devid, axisid, &gotnode);

	if (!axis)
		return gotnode ?
			ARCAN_ERRC_BAD_RESOURCE : ARCAN_ERRC_NO_SUCH_OBJECT;

	*lower_bound = axis->lower;
	*upper_bound = axis->upper;
	*deadzone = axis->deadzone;
	*kernel_size = axis->kernel_sz;
	*mode = axis->mode;

	return ARCAN_OK;
}

void platform_event_analogall(bool enable, bool mouse)
{
	struct arcan_devnode* node = lookup_devnode(iodev.mouseid);
	if (!node)
		return;

/*
 * FIXME sweep all devices and all axes (or just mouseid) if (enable) then set
 * whatever the previous mode was, else store current mode and set NONE
 */
}

void platform_event_analogfilter(int devid,
	int axisid, int lower_bound, int upper_bound, int deadzone,
	int buffer_sz, enum ARCAN_ANALOGFILTER_KIND kind)
{
	bool node;
	struct axis_opts* axis = find_axis(devid, axisid, &node);
	if (!axis)
		return;

	int kernel_lim = sizeof(axis->flt_kernel) / sizeof(axis->flt_kernel[0]);

	if (buffer_sz > kernel_lim)
		buffer_sz = kernel_lim;

	if (buffer_sz <= 0)
		buffer_sz = 1;

	set_analogstate(axis,lower_bound, upper_bound, deadzone, buffer_sz, kind);
}

static void discovered(const char* name, size_t name_len)
{
	int fd = fmt_open(0, O_NONBLOCK | O_RDONLY | O_CLOEXEC,
		"%s/%.*s", notify_scan_dir, name_len, name);

	if (log_verbose)
		arcan_warning(
			"input: discovered %s/%.*s\n", notify_scan_dir, name_len, name);

	if (-1 != fd)
		got_device(fd, name);
	else
		arcan_warning("input: couldn't open new device (%s), reason: %s\n",
			name, strerror(errno));
}

void platform_event_process(struct arcan_evctx* ctx)
{
/* lovely little variable length field at end of struct here /sarcasm,
 * could get away with running the notify polling less often than once
 * every frame, somewhat excessive. */
	if (-1 != gstate.notify){
		char inbuf[1024];
		ssize_t nr = read(gstate.notify, inbuf, sizeof(inbuf));
		off_t ofs = 0;

		if (-1 != nr)
			while (nr - ofs > sizeof(struct inotify_event)){
				struct inotify_event cur;
				memcpy(&cur, &inbuf[ofs], sizeof(struct inotify_event));
				ofs += sizeof(struct inotify_event);

				if ((cur.mask & IN_CREATE) && !(cur.mask & IN_ISDIR)){
					discovered(&inbuf[ofs], cur.len);
					ofs += cur.len;
				}
			}
	}

	char dump[256];
	size_t nr __attribute__((unused));

	if (poll(iodev.pollset, iodev.n_devs, 0) <= 0)
		return;

	for (size_t i = 0; i < iodev.n_devs; i++){
		if (!(iodev.pollset[i].revents & POLLIN))
			continue;

		if (iodev.nodes[i].hnd.handler)
			iodev.nodes[i].hnd.handler(ctx, &iodev.nodes[i]);
		else
			nr = read(iodev.nodes[i].handle, dump, 256);
	}
}

void platform_event_keyrepeat(struct arcan_evctx* ctx, int* period, int* delay)
{
	bool upd = false;

	if (*period < 0){
		*period = iodev.period;
	}
	else{
		int tmp = *period;
		*period = iodev.period;
		iodev.period = tmp;
		upd = true;
	}

	if (*delay < 0){
		int tmp = *delay;
		*delay = iodev.delay;
		iodev.delay = tmp;
		upd = true;
	}

	if (upd){
		for (size_t i = 0; i < iodev.n_devs; i++)
			if (iodev.nodes[i].type == DEVNODE_KEYBOARD){
				struct kbd_repeat kbrv = {
					.period = iodev.period,
					.delay = iodev.delay
				};
				ioctl(iodev.nodes[i].handle, KDKBDREP, &kbrv);
			}
	}
}

static const char* lookup_type(int val)
{
	switch(val){
	case DEVNODE_GAME:
		return "game";
	case DEVNODE_MOUSE:
		return "mouse";
	case DEVNODE_SENSOR:
		return "sensor";
	case DEVNODE_KEYBOARD:
		return "keyboard";
	break;
	default:
	return "unknown";
	}
}

#define bit_longn(x) ( (x) / (sizeof(long)*8) )
#define bit_ofs(x) ( (x) % (sizeof(long)*8) )
#define bit_isset(ary, bit) (( ary[bit_longn(bit)] >> bit_ofs(bit)) & 1)
#define bit_count(x) ( ((x) - 1 ) / (sizeof(long) * 8 ) + 1 )

static size_t button_count(int fd, size_t bitn, bool* got_mouse, bool* got_joy)
{
	size_t count = 0;

	unsigned long bits[ bit_count(KEY_MAX) ];

	if (-1 == ioctl(fd, EVIOCGBIT(bitn, KEY_MAX), bits))
		return false;

	for (size_t i = 0; i < KEY_MAX; i++){
		if (bit_isset(bits, i)){
			count++;
		}
	}

	*got_mouse = (bit_isset(bits, BTN_MOUSE) || bit_isset(bits, BTN_LEFT) ||
		bit_isset(bits, BTN_RIGHT) || bit_isset(bits, BTN_MIDDLE));

	*got_joy = (bit_isset(bits, BTN_JOYSTICK) || bit_isset(bits, BTN_GAMEPAD) ||
		bit_isset(bits, BTN_WHEEL));

	return count;
}

static bool check_mouse_axis(int fd, size_t bitn)
{
	unsigned long bits[ bit_count(KEY_MAX) ];
	if (-1 == ioctl(fd, EVIOCGBIT(bitn, KEY_MAX), bits))
		return false;

/* uncertain if other (REL_Z, REL_RX, REL_RY, REL_RZ, REL_DIAL, REL_MISC)
 * should be used as a failing criteria */
	return bit_isset(bits, REL_X) && bit_isset(bits, REL_Y);
}

static void map_axes(int fd, size_t bitn, struct arcan_devnode* node)
{
	unsigned long bits[ bit_count(ABS_MAX) ];

	if (-1 == ioctl(fd, EVIOCGBIT(bitn, ABS_MAX), bits))
		return;

	assert(node->type == DEVNODE_GAME);
	if (node->game.adata)
		return;

	node->game.axes = 0;

	for (size_t i = 0; i < ABS_MAX; i++){
		if (bit_isset(bits, i))
			node->game.axes++;
	}

	if (node->game.axes == 0)
		return;

	node->game.adata = arcan_alloc_mem(
		sizeof(struct axis_opts) * node->game.axes,
		ARCAN_MEM_BINDING, ARCAN_MEM_BZERO, ARCAN_MEMALIGN_NATURAL);

	size_t ac = 0;

	for (size_t i = 0; i < ABS_MAX; i++)
		if (bit_isset(bits, i)){
			struct input_absinfo ainf;
			struct axis_opts* ax = &node->game.adata[ac++];

			memset(ax, '\0', sizeof(struct axis_opts));
			ax->mode = ax->oldmode = ARCAN_ANALOGFILTER_AVG;
			ax->lower = -32768;
			ax->upper = 32767;

			if (-1 == ioctl(fd, EVIOCGABS(i), &ainf))
				continue;

			ax->upper = ainf.maximum;
			ax->lower = ainf.minimum;
			assert(ainf.maximum != ainf.minimum && ainf.maximum > ainf.minimum);
		}
}

static void got_device(int fd, const char* path)
{
	struct arcan_devnode node = {
		.handle = fd
	};

	struct stat fdstat;
	if (-1 == fstat(fd, &fdstat)){
		if (log_verbose)
			arcan_warning(
				"input: couldn't stat node to identify (%s)\n", strerror(errno));
		return;
	}

	if ((fdstat.st_mode & (S_IFCHR | S_IFBLK)) == 0){
		if (log_verbose)
			arcan_warning(
				"input: ignoring %s, not a character or block device\n", path);
		return;
	}

	if (!identify(fd, path, node.label, sizeof(node.label), &node.devnum)){
		if (log_verbose)
			arcan_warning("input: identify failed on %s, ignoring unknown.\n", path);
		close(fd);
		return;
	}

	if (iodev.n_devs >= MAX_DEVICES){
		arcan_warning("input: device limit reached, ignoring %s.\n", path);
		close(fd);
	}

/* figure out what kind of a device this is from the exposed capabilities,
 * heuristic nonsense rather than an interface exposing what the driver should
 * know or decide, fantastic.
 *
 * keyboards typically have longer key masks (and we can check for a few common
 * ones) no REL/ABS (don't know if those built-in trackball ones expose as two
 * devices or not these days), but also a ton of .. keys
 */
	struct evhandler eh = lookup_dev_handler(node.label);

/* [eh] may contain overrides, but we still need to probe the driver state for
 * axes etc. and allocate accordingly */
	node.type = DEVNODE_GAME;

	bool mouse_ax = false;
	bool mouse_btn = false;
	bool joystick_btn = false;

	if (1){
	size_t bpl = sizeof(long) * 8;
	size_t nbits = ((EV_MAX)-1) / bpl + 1;
	long prop[ nbits ];

	if (-1 == ioctl(fd, EVIOCGBIT(0, EV_MAX), &prop)){
		if (log_verbose)
			arcan_warning("input: probing %s failed, %s\n", path, strerror(errno));
		close(fd);
		return;
	}

	for (size_t bit = 0; bit < EV_MAX; bit++)
		if ( 1ul & (prop[bit/bpl]) >> (bit & (bpl - 1)) )
		switch(bit){
		case EV_KEY:
			node.button_count = button_count(fd, bit, &mouse_btn, &joystick_btn);
		break;

		case EV_REL:
			mouse_ax = check_mouse_axis(fd, bit);
		break;

		case EV_ABS:
			map_axes(fd, bit, &node);
		break;

/* useless for the time being */
		case EV_MSC:
		break;
		case EV_SYN:
		break;
		case EV_LED:
		break;
		case EV_SND:
		break;
		case EV_REP:
		break;
		case EV_PWR:
		break;
		case EV_FF:
		case EV_FF_STATUS:
		break;
		}

	if (!eh.handler){
		if (mouse_ax && mouse_btn){
			node.type = DEVNODE_MOUSE;
			node.cursor.flt[0].mode = ARCAN_ANALOGFILTER_PASS;
			node.cursor.flt[1].mode = ARCAN_ANALOGFILTER_PASS;

			if (!iodev.mouseid)
				iodev.mouseid = node.devnum;
		}
/* not particularly pretty and rather arbitrary */
		else if (!mouse_btn && !joystick_btn && node.button_count > 84){
			node.type = DEVNODE_KEYBOARD;
			struct kbd_repeat kbrv = {0};
			ioctl(node.handle, KDKBDREP, &kbrv);
/* FIX: query current LED states and set corresponding states in the devnode */
		}

		node.hnd.handler = defhandlers[node.type];
	}
	else{
		node.hnd = eh;
		node.type = eh.type;
	}

/* pre-existing? close old node and replace with this one */
	int hole = -1;

	for (size_t i = 0; i < iodev.sz_nodes; i++){
		if (-1 == hole && iodev.nodes[i].handle <= 0){
			hole = i;
			continue;
		}

		if (iodev.nodes[i].devnum == node.devnum){
			if (iodev.nodes[i].handle > 0)
				close(iodev.nodes[i].handle);

			iodev.nodes[i].handle = fd;
			iodev.pollset[i].fd = fd;
			iodev.pollset[i].events = POLLIN;

			return;
		}
	}

/* no empty slot, grow pollsets and node tracking */
	if (hole == -1){
		size_t new_sz = iodev.sz_nodes + 8;
		struct arcan_devnode* nn = realloc(
			iodev.nodes, sizeof(struct arcan_devnode) * new_sz);
		if (!nn)
			goto cleanup;
		iodev.nodes = nn;
		memset(nn + iodev.sz_nodes, '\0', sizeof(struct arcan_devnode) * 8);

		struct pollfd* np = realloc(
			iodev.pollset, sizeof(struct pollfd) * new_sz);
		if (!np)
			goto cleanup;

		memset(np + iodev.sz_nodes, '\0', sizeof(struct pollfd) * 8);
		iodev.pollset = np;
		hole = iodev.sz_nodes;
		iodev.sz_nodes = new_sz;
	}

	iodev.n_devs++;
	iodev.pollset[hole].fd = fd;
	iodev.pollset[hole].events = POLLIN;
	iodev.nodes[hole] = node;

	if (log_verbose)
		arcan_warning("input: (%s:%s) added as type: %s\n",
			path, node.label, lookup_type(node.type));

	return;
	}
cleanup:
	if (log_verbose)
		arcan_warning("input: dropped %s due to errors during scan.\n", path);
	close(fd);
}

#undef bit_isset
#undef bit_ofs
#undef bit_longn
#undef bit_count

void platform_event_rescan_idev(struct arcan_evctx* ctx)
{
/* rescan is not needed here as we check inotify while polling */
	static bool init;
	if (!init)
		init = true;
	else
		return;

	char ibuf [strlen(notify_scan_dir) + sizeof("/*")];
	glob_t res = {0};
	snprintf(ibuf, sizeof(ibuf), "%s/*", notify_scan_dir);

	if (glob(ibuf, 0, NULL, &res) == 0){
		char** beg = res.gl_pathv;

		while(*beg){
			int fd = open(*beg, O_NONBLOCK | O_RDONLY | O_CLOEXEC);
			if (-1 != fd)
				got_device(fd, *beg);
			beg++;
		}

		globfree(&res);
	}
	else if (log_verbose)
		arcan_warning("input: couldn't scan %s\n", notify_scan_dir);
}

static void update_state(int code, bool state, unsigned* statev)
{
	int modifier = 0;

	switch (klut[code]){
	case K_LSHIFT:
		modifier = ARKMOD_LSHIFT;
	break;
	case K_RSHIFT:
		modifier = ARKMOD_RSHIFT;
	break;
	case K_LCTRL:
		modifier = ARKMOD_LCTRL;
	break;
	case K_RCTRL:
		modifier = ARKMOD_RCTRL;
	break;
	case K_CAPSLOCK:
		modifier = ARKMOD_CAPS;
	break;
	default:
		return;
	}

	if (state)
		*statev |= (1 << modifier);
	else
		*statev &= ~(1 << modifier);
}

static void disconnect(struct arcan_devnode* node)
{
	for (size_t i = 0; i < iodev.n_devs; i++)
		if (node->devnum == iodev.nodes[i].devnum){
			close(node->handle);
			node->handle = 0;
			iodev.pollset[i].fd = 0;
			iodev.pollset[i].events = 0;
			if (i == iodev.n_devs - 1)
				iodev.n_devs--;
			break;
		}
}

static void defhandler_kbd(struct arcan_evctx* out,
	struct arcan_devnode* node)
{
	struct input_event inev[64];
	ssize_t evs = read(node->handle, &inev, sizeof(inev));

	if (-1 == evs)
		return disconnect(node);

	if (evs < 0 || evs < sizeof(struct input_event))
		return;

	arcan_event newev = {
		.category = EVENT_IO,
		.io = {
			.kind = EVENT_IO_BUTTON,
			.datatype = EVENT_IDATATYPE_TRANSLATED,
			.devkind = EVENT_IDEVKIND_KEYBOARD,
			.input.translated = {
				.devid = node->devnum
			}
		}
	};

	for (size_t i = 0; i < evs / sizeof(struct input_event); i++){
		switch(inev[i].type){
		case EV_KEY:
		newev.io.input.translated.scancode = inev[i].code;
		newev.io.input.translated.keysym = lookup_keycode(inev[i].code);

		update_state(inev[i].code, inev[i].value != 0, &node->keyboard.state);

		newev.io.input.translated.modifiers = node->keyboard.state;
		newev.io.input.translated.subid =
			lookup_character(inev[i].code, node->keyboard.state);

		if (inev[i].value == 2){
			newev.io.input.translated.active = false;
			arcan_event_enqueue(out, &newev);
			newev.io.input.translated.active = true;
			arcan_event_enqueue(out, &newev);
		}
		else{
			newev.io.input.translated.active = inev[i].value != 0;
			arcan_event_enqueue(out, &newev);
		}

		break;

		default:
		break;
		}

	}
}

static void decode_hat(struct arcan_evctx* ctx,
	struct arcan_devnode* node, int ind, int val)
{
	arcan_event newev = {
		.category = EVENT_IO,
		.io = {
			.label = "gamepad",
			.kind = EVENT_IO_BUTTON,
			.devkind = EVENT_IDEVKIND_GAMEDEV,
			.datatype = EVENT_IDATATYPE_DIGITAL
		}
	};

	ind *= 2;
	const int base = 64;

	newev.io.input.digital.devid = node->devnum;

/* clamp */
	if (val < 0)
		val = -1;
	else if (val > 0)
		val = 1;
	else {
/* which of the two possibilities was released? */
		newev.io.input.digital.active = false;

		if (node->game.hats[ind] != 0){
			newev.io.input.digital.subid = base + ind;
			node->game.hats[ind] = 0;
			arcan_event_enqueue(ctx, &newev);
		}

		if (node->game.hats[ind+1] != 0){
			newev.io.input.digital.subid = base + ind + 1;
			node->game.hats[ind+1] = 0;
			arcan_event_enqueue(ctx, &newev);
		}

		return;
	}

	if (val > 0)
		ind++;

	node->game.hats[ind] = val;
	newev.io.input.digital.active = true;
	newev.io.input.digital.subid = base + ind;
	arcan_event_enqueue(ctx, &newev);
}

static void defhandler_game(struct arcan_evctx* ctx,
	struct arcan_devnode* node)
{
	struct input_event inev[64];
	ssize_t evs = read(node->handle, &inev, sizeof(inev));

	if (-1 == evs)
		return disconnect(node);

	if (evs < 0 || evs < sizeof(struct input_event))
		return;

	arcan_event newev = {
		.category = EVENT_IO,
		.io = {
			.label = "gamepad",
			.devkind = EVENT_IDEVKIND_GAMEDEV
		}
	};

	short samplev;

	for (size_t i = 0; i < evs / sizeof(struct input_event); i++){
		switch(inev[i].type){
		case EV_KEY:
			inev[i].code -= BTN_JOYSTICK;
			if (node->hnd.button_mask && inev[i].code <= 64 &&
				( (node->hnd.button_mask >> inev[i].code) & 1) )
				continue;

			newev.io.kind = EVENT_IO_BUTTON;
			newev.io.datatype = EVENT_IDATATYPE_DIGITAL;
			newev.io.input.digital.active = inev[i].value;
			newev.io.input.digital.subid = inev[i].code - BTN_JOYSTICK;
			newev.io.input.digital.devid = node->devnum;
			arcan_event_enqueue(ctx, &newev);
		break;

		case EV_ABS:
			if (node->hnd.axis_mask && inev[i].code <= 64 &&
				( (node->hnd.axis_mask >> inev[i].code) & 1) )
				continue;

			if (node->hnd.digital_hat &&
				inev[i].code >= ABS_HAT0X && inev[i].code <= ABS_HAT3Y)
				decode_hat(ctx, node, inev[i].code - ABS_HAT0X, inev[i].value);

			else if (inev[i].code < node->game.axes &&
				process_axis(ctx,
				&node->game.adata[inev[i].code], inev[i].value, &samplev)){
				newev.io.kind = EVENT_IO_AXIS_MOVE;
				newev.io.datatype = EVENT_IDATATYPE_ANALOG;
				newev.io.input.analog.gotrel = false;
				newev.io.input.analog.subid = inev[i].code;
				newev.io.input.analog.devid = node->devnum;
				newev.io.input.analog.axisval[0] = samplev;
				newev.io.input.analog.nvalues = 2;

				arcan_event_enqueue(ctx, &newev);
			}
		break;

		default:
		break;
		}
	}

}

static inline short code_to_mouse(int code)
{
	return (code < BTN_MOUSE || code >= BTN_JOYSTICK) ?
		-1 : (code - BTN_MOUSE + 1);
}

static void defhandler_mouse(struct arcan_evctx* ctx,
	struct arcan_devnode* node)
{
	struct input_event inev[64];

	ssize_t evs = read(node->handle, &inev, sizeof(inev));
	if (-1 == evs)
		return disconnect(node);

	if (evs < 0 || evs < sizeof(struct input_event))
		return;

	arcan_event newev = {
		.category = EVENT_IO,
		.io = {
			.label = "mouse",
			.devkind = EVENT_IDEVKIND_MOUSE,
		}
	};

	short samplev;

	for (size_t i = 0; i < evs / sizeof(struct input_event); i++){
		switch(inev[i].type){
		case EV_KEY:
			samplev = code_to_mouse(inev[i].code);
			if (samplev < 0)
				continue;

			newev.io.kind = EVENT_IO_BUTTON;
			newev.io.datatype = EVENT_IDATATYPE_DIGITAL;
			newev.io.input.digital.active = inev[i].value;
			newev.io.input.digital.subid = samplev;
			newev.io.input.digital.devid = node->devnum;

			arcan_event_enqueue(ctx, &newev);
		break;
		case EV_REL:
			switch (inev[i].code){
			case REL_X:
				if (process_axis(ctx, &node->cursor.flt[0], inev[i].value, &samplev)){
					samplev = inev[i].value;

					node->cursor.mx = ((int)node->cursor.mx + samplev < 0) ?
						0 : node->cursor.mx + samplev;

					newev.io.kind = EVENT_IO_AXIS_MOVE;
					newev.io.datatype = EVENT_IDATATYPE_ANALOG;
					newev.io.input.analog.gotrel = true;
					newev.io.input.analog.subid = 0;
					newev.io.input.analog.devid = node->devnum;
					newev.io.input.analog.axisval[0] = node->cursor.mx;
					newev.io.input.analog.axisval[1] = samplev;
					newev.io.input.analog.nvalues = 2;

					arcan_event_enqueue(ctx, &newev);
				}
			break;
			case REL_Y:
				if (process_axis(ctx, &node->cursor.flt[1], inev[i].value, &samplev)){
					node->cursor.my = ((int)node->cursor.my + samplev < 0) ?
						0 : node->cursor.my + samplev;

					newev.io.kind = EVENT_IO_AXIS_MOVE;
					newev.io.datatype = EVENT_IDATATYPE_ANALOG;
					newev.io.input.analog.gotrel = true;
					newev.io.input.analog.subid = 1;
					newev.io.input.analog.devid = node->devnum;
					newev.io.input.analog.axisval[0] = node->cursor.my;
					newev.io.input.analog.axisval[1] = samplev;
					newev.io.input.analog.nvalues = 2;

					arcan_event_enqueue(ctx, &newev);
				}
			break;
			default:
			break;
			}
		break;
		case EV_ABS:
		break;
		}
	}
}

static void defhandler_null(struct arcan_evctx* out,
	struct arcan_devnode* node)
{
	char nbuf[256];
	ssize_t evs = read(node->handle, nbuf, sizeof(nbuf));
	if (-1 == evs)
		return disconnect(node);
}

const char* platform_event_devlabel(int devid)
{
	if (devid == -1)
		return "mouse";

	if (devid < 0 || devid >= iodev.n_devs)
		return "no device";

	return strlen(iodev.nodes[devid].label) == 0 ?
		"no identifier" : iodev.nodes[devid].label;
}

/* ajax @ xorg-dev ml, [PATCH] linux: Prefer ioctl(KDSKBMUTE), ... */
#ifndef KDSKBMUTE
#define KDSKBMUTE 0x4B51
#endif

void platform_event_deinit(struct arcan_evctx* ctx)
{
	if (isatty(gstate.tty) && gstate.mute){
		ioctl(gstate.tty, KDSKBMUTE, 0);
		if (-1 == ioctl(gstate.tty, KDSETMODE, KD_TEXT)){
			arcan_warning("reset failed %s\n", strerror(errno));
		}
		gstate.kbmode = gstate.kbmode == K_OFF ? K_XLATE : gstate.kbmode;
		ioctl(gstate.tty, KDSKBMODE, gstate.kbmode);
		ioctl(gstate.tty, KDSETLED, gstate.leds);
		gstate.mute = false;
	}

	if (gstate.tty != STDIN_FILENO){
		close(gstate.tty);
		gstate.tty = STDIN_FILENO;
	}

	if (gstate.notify != -1){
		close(gstate.notify);
		gstate.notify = -1;
	}

	for (size_t i = 0; i < iodev.n_devs; i++)
		if (iodev.nodes[i].handle > 0){
			close(iodev.nodes[i].handle);
			memset(&iodev.nodes[i], '\0', sizeof(struct arcan_devnode));
		}

	iodev.n_devs = 0;
}

void platform_device_lock(int devind, bool state)
{
	struct arcan_devnode* node = lookup_devnode(devind);
	if (!node || !node->handle)
		return;

	ioctl(node->handle, EVIOCGRAB, state? 1 : 0);

/*
 * doesn't make sense outside some window systems, might be useful to propagate
 * further to device locking on systems that are less forgiving.
 */
}

enum PLATFORM_EVENT_CAPABILITIES platform_input_capabilities()
{
	enum PLATFORM_EVENT_CAPABILITIES rv = 0;

	for (size_t i = 0; i < iodev.n_devs; i++){
		if (iodev.nodes[i].handle)
			switch(iodev.nodes[i].type){
/* don't have better granularity in this step at the moment */
			case DEVNODE_SENSOR:
				rv |= ACAP_POSITION | ACAP_ORIENTATION;
			break;
			case DEVNODE_MOUSE:
				rv |= ACAP_MOUSE;
			break;
			case DEVNODE_GAME:
				rv |= ACAP_GAMING;
			break;
			case DEVNODE_KEYBOARD:
				rv |= ACAP_TRANSLATED;
			break;
			case DEVNODE_TOUCH:
				rv |= ACAP_TOUCH;
			break;
			default:
			break;
		}
	}
	return rv;
}

const char** platform_input_envopts()
{
	return (const char**) envopts;
}

static int find_tty()
{
/* first, check if the env. defines a specific TTY device to use and try that */
	const char* newtty = NULL;
	int tty = -1;

	if ((newtty = getenv("ARCAN_INPUT_TTYOVERRIDE"))){
		int fd = open(newtty, O_RDWR, O_CLOEXEC);
		if (-1 == fd)
			arcan_warning("couldn't open TTYOVERRIDE %s, reason: %s\n",
				newtty, strerror(errno));
		else
			tty = fd;
	}

/* Failing that, try and find what tty we might be on -- some might redirect
 * stdin to something else and then it is not a valid tty to work on. Which,
 * of course, brings us back to the special kid in the class, sysfs. */
	if (!isatty(tty)){
		FILE* fpek = fopen("/sys/class/tty/tty0/active", "r");
		if (fpek){
			char line[32] = "/dev/";
			if (fgets(line+5, 32-5, fpek)){
				char* endl = strrchr(line, '\n');
				if (endl)
					*endl = '\0';
				tty = open(line, O_RDWR);
			}
			fclose(fpek);
		}
	}

	return tty == -1 ? STDIN_FILENO : tty;
}

void platform_event_init(arcan_evctx* ctx)
{
	gstate.notify = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
	init_keyblut();

	gstate.tty = find_tty();

	if (isatty(gstate.tty)){
		ioctl(gstate.tty, KDGETMODE, &gstate.mode);
		ioctl(gstate.tty, KDGETLED, &gstate.leds);
		ioctl(gstate.tty, KDGKBMODE, &gstate.kbmode);
		ioctl(gstate.tty, KDSETLED, 0);

		if (!getenv("ARCAN_INPUT_NOMUTETTY")){
			ioctl(gstate.tty, KDSKBMUTE, 1);
			ioctl(gstate.tty, KDSKBMODE, K_OFF);
			ioctl(gstate.tty, KDSETMODE, KD_GRAPHICS);
		}

/* missing: install handler for signal - switching VTY:
 * setup VT_PROCESS for the TTY, with a relsig and an acqsig along
 * with matching signal handlers.
 *
 * relsig sets value that during next event process will force a
 * set_external, sleep-loop until acqsig is triggered where we restore
 */
		struct sigaction er_sh = {.sa_handler = SIG_IGN};
		sigaction(SIGINT, &er_sh, NULL);
		gstate.mute = true;
	}

	log_verbose = getenv("ARCAN_INPUT_VERBOSE");

	const char* newsd;
	if ((newsd = getenv("ARCAN_INPUT_SCANDIR")))
		notify_scan_dir = newsd;

	if (-1 == gstate.notify || inotify_add_watch(
		gstate.notify, notify_scan_dir, IN_CREATE) == -1){
		arcan_warning("inotify initialization failure (%s),"
			"	device discovery disabled.", strerror(errno));

		if (-1 != gstate.notify){
			close(gstate.notify);
			gstate.notify = -1;
		}
	}

	platform_event_rescan_idev(ctx);
}
