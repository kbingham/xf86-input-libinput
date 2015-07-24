/*
 * Copyright © 2013 Red Hat, Inc.
 *
 * Permission to use, copy, modify, distribute, and sell this software
 * and its documentation for any purpose is hereby granted without
 * fee, provided that the above copyright notice appear in all copies
 * and that both that copyright notice and this permission notice
 * appear in supporting documentation, and that the name of Red Hat
 * not be used in advertising or publicity pertaining to distribution
 * of the software without specific, written prior permission.  Red
 * Hat makes no representations about the suitability of this software
 * for any purpose.  It is provided "as is" without express or implied
 * warranty.
 *
 * THE AUTHORS DISCLAIM ALL WARRANTIES WITH REGARD TO THIS SOFTWARE,
 * INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS, IN
 * NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY SPECIAL, INDIRECT OR
 * CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS
 * OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT,
 * NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
 * CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <sys/epoll.h>
#include <errno.h>
#include <fcntl.h>
#include <time.h>
#include <unistd.h>
#include <xorg-server.h>
#include <list.h>
#include <exevents.h>
#include <xkbsrv.h>
#include <xf86Xinput.h>
#include <xserver-properties.h>
#include <libinput.h>
#include <linux/input.h>

#include <X11/Xatom.h>

#include "libinput-properties.h"

#ifndef XI86_SERVER_FD
#define XI86_SERVER_FD 0x20
#endif

#if GET_ABI_MAJOR(ABI_XINPUT_VERSION) * 1000 + GET_ABI_MINOR(ABI_XINPUT_VERSION) > 22000
#define HAVE_VMASK_UNACCEL 1
#else
#undef HAVE_VMASK_UNACCEL
#endif

#define TOUCHPAD_NUM_AXES 4 /* x, y, hscroll, vscroll */
#define TOUCH_MAX_SLOTS 15
#define XORG_KEYCODE_OFFSET 8

/*
   libinput does not provide axis information for absolute devices, instead
   it scales into the screen dimensions provided. So we set up the axes with
   a fixed range, let libinput scale into that range and then the server
   do the scaling it usually does.
 */
#define TOUCH_AXIS_MAX 0xffff

struct xf86libinput_driver {
	struct libinput *libinput;
	int device_enabled_count;
	struct xorg_list server_fds;
};

static struct xf86libinput_driver driver_context;

struct xf86libinput {
	char *path;
	struct libinput_device *device;

	struct {
		int vdist;
		int hdist;
		int vdist_remainder;
		int hdist_remainder;
	} scroll;

	struct {
		double x;
		double y;
		double x_remainder;
		double y_remainder;
	} scale;

	BOOL has_abs;

	ValuatorMask *valuators;
	ValuatorMask *valuators_unaccelerated;

	struct options {
		BOOL tapping;
		BOOL tap_drag_lock;
		BOOL natural_scrolling;
		BOOL left_handed;
		BOOL middle_emulation;
		BOOL halfkey;
		CARD32 sendevents;
		CARD32 scroll_button; /* xorg button number */
		float speed;
		float matrix[9];
		enum libinput_config_scroll_method scroll_method;
		enum libinput_config_click_method click_method;

		unsigned char btnmap[MAX_BUTTONS + 1];
	} options;
};

/*
   libinput provides a userdata for the context, but not per path device. so
   the open_restricted call has the libinput context, but no reference to
   the pInfo->fd that we actually need to return.
   To avoid this, we store each path/fd combination during pre_init in the
   context, then return that during open_restricted. If a device is added
   twice with two different fds this may give us the wrong fd but why are
   you doing that anyway.
 */
struct serverfd {
	struct xorg_list node;
	int fd;
	char *path;
};

static inline int
use_server_fd(const InputInfoPtr pInfo) {
	return pInfo->fd > -1 && (pInfo->flags & XI86_SERVER_FD);
}

static inline void
fd_push(struct xf86libinput_driver *context,
	int fd,
	const char *path)
{
	struct serverfd *sfd = xnfcalloc(1, sizeof(*sfd));

	sfd->fd = fd;
	sfd->path = xnfstrdup(path);
	xorg_list_add(&sfd->node, &context->server_fds);
}

static inline int
fd_get(struct xf86libinput_driver *context,
       const char *path)
{
	struct serverfd *sfd;

	xorg_list_for_each_entry(sfd, &context->server_fds, node) {
		if (strcmp(path, sfd->path) == 0)
			return sfd->fd;
	}

	return -1;
}

static inline void
fd_pop(struct xf86libinput_driver *context, int fd)
{
	struct serverfd *sfd;

	xorg_list_for_each_entry(sfd, &context->server_fds, node) {
		if (fd != sfd->fd)
			continue;

		xorg_list_del(&sfd->node);
		free(sfd->path);
		free(sfd);
		break;
	}
}

static inline int
fd_find(struct xf86libinput_driver *context, int fd)
{
	struct serverfd *sfd;

	xorg_list_for_each_entry(sfd, &context->server_fds, node) {
		if (fd == sfd->fd)
			return fd;
	}
	return -1;
}

static inline unsigned int
btn_linux2xorg(unsigned int b)
{
	unsigned int button;

	switch(b) {
	case 0: button = 0; break;
	case BTN_LEFT: button = 1; break;
	case BTN_MIDDLE: button = 2; break;
	case BTN_RIGHT: button = 3; break;
	default:
		button = 8 + b - BTN_SIDE;
		break;
	}

	return button;
}
static inline unsigned int
btn_xorg2linux(unsigned int b)
{
	unsigned int button;

	switch(b) {
	case 0: button = 0; break;
	case 1: button = BTN_LEFT; break;
	case 2: button = BTN_MIDDLE; break;
	case 3: button = BTN_RIGHT; break;
	default:
		button = b - 8 + BTN_SIDE;
		break;
	}

	return button;
}

static int
LibinputSetProperty(DeviceIntPtr dev, Atom atom, XIPropertyValuePtr val,
                 BOOL checkonly);
static void
LibinputInitProperty(DeviceIntPtr dev);

static inline void
LibinputApplyConfig(DeviceIntPtr dev)
{
	InputInfoPtr pInfo = dev->public.devicePrivate;
	struct xf86libinput *driver_data = pInfo->private;
	struct libinput_device *device = driver_data->device;
	unsigned int scroll_button;

	if (libinput_device_config_send_events_get_modes(device) != LIBINPUT_CONFIG_SEND_EVENTS_ENABLED &&
	    libinput_device_config_send_events_set_mode(device,
							driver_data->options.sendevents) != LIBINPUT_CONFIG_STATUS_SUCCESS)
		xf86IDrvMsg(pInfo, X_ERROR,
			    "Failed to set SendEventsMode %u\n",
			    driver_data->options.sendevents);

	if (libinput_device_config_scroll_has_natural_scroll(device) &&
	    libinput_device_config_scroll_set_natural_scroll_enabled(device,
								     driver_data->options.natural_scrolling) != LIBINPUT_CONFIG_STATUS_SUCCESS)
		xf86IDrvMsg(pInfo, X_ERROR,
			    "Failed to set NaturalScrolling to %d\n",
			    driver_data->options.natural_scrolling);

	if (libinput_device_config_accel_is_available(device) &&
	    libinput_device_config_accel_set_speed(device,
						   driver_data->options.speed) != LIBINPUT_CONFIG_STATUS_SUCCESS)
			xf86IDrvMsg(pInfo, X_ERROR,
				    "Failed to set speed %.2f\n",
				    driver_data->options.speed);
	if (libinput_device_config_tap_get_finger_count(device) > 0 &&
	    libinput_device_config_tap_set_enabled(device,
						   driver_data->options.tapping) != LIBINPUT_CONFIG_STATUS_SUCCESS)
		xf86IDrvMsg(pInfo, X_ERROR,
			    "Failed to set Tapping to %d\n",
			    driver_data->options.tapping);

	if (libinput_device_config_tap_get_finger_count(device) > 0 &&
	    libinput_device_config_tap_set_drag_lock_enabled(device,
							     driver_data->options.tap_drag_lock) != LIBINPUT_CONFIG_STATUS_SUCCESS)
		xf86IDrvMsg(pInfo, X_ERROR,
			    "Failed to set Tapping DragLock to %d\n",
			    driver_data->options.tap_drag_lock);

	if (libinput_device_config_calibration_has_matrix(device) &&
	    libinput_device_config_calibration_set_matrix(device,
							  driver_data->options.matrix) != LIBINPUT_CONFIG_STATUS_SUCCESS)
		xf86IDrvMsg(pInfo, X_ERROR,
			    "Failed to apply matrix: "
			    "%.2f %.2f %.2f %2.f %.2f %.2f %.2f %.2f %.2f\n",
			    driver_data->options.matrix[0], driver_data->options.matrix[1],
			    driver_data->options.matrix[2], driver_data->options.matrix[3],
			    driver_data->options.matrix[4], driver_data->options.matrix[5],
			    driver_data->options.matrix[6], driver_data->options.matrix[7],
			    driver_data->options.matrix[8]);

	if (libinput_device_config_left_handed_is_available(device) &&
	    libinput_device_config_left_handed_set(device,
						   driver_data->options.left_handed) != LIBINPUT_CONFIG_STATUS_SUCCESS)
		xf86IDrvMsg(pInfo, X_ERROR,
			    "Failed to set LeftHanded to %d\n",
			    driver_data->options.left_handed);

	if (libinput_device_config_scroll_set_method(device,
						     driver_data->options.scroll_method) != LIBINPUT_CONFIG_STATUS_SUCCESS) {
		const char *method;

		switch(driver_data->options.scroll_method) {
		case LIBINPUT_CONFIG_SCROLL_NO_SCROLL: method = "none"; break;
		case LIBINPUT_CONFIG_SCROLL_2FG: method = "twofinger"; break;
		case LIBINPUT_CONFIG_SCROLL_EDGE: method = "edge"; break;
		case LIBINPUT_CONFIG_SCROLL_ON_BUTTON_DOWN: method = "button"; break;
		default:
			method = "unknown"; break;
		}

		xf86IDrvMsg(pInfo, X_ERROR,
			    "Failed to set scroll to %s\n",
			    method);
	}

	if (libinput_device_config_scroll_get_methods(device) & LIBINPUT_CONFIG_SCROLL_ON_BUTTON_DOWN) {
		scroll_button = btn_xorg2linux(driver_data->options.scroll_button);
		if (libinput_device_config_scroll_set_button(device, scroll_button) != LIBINPUT_CONFIG_STATUS_SUCCESS)
			xf86IDrvMsg(pInfo, X_ERROR,
				    "Failed to set ScrollButton to %u\n",
				    driver_data->options.scroll_button);
	}

	if (libinput_device_config_click_set_method(device,
						    driver_data->options.click_method) != LIBINPUT_CONFIG_STATUS_SUCCESS) {
		const char *method;

		switch (driver_data->options.click_method) {
		case LIBINPUT_CONFIG_CLICK_METHOD_NONE: method = "none"; break;
		case LIBINPUT_CONFIG_CLICK_METHOD_BUTTON_AREAS: method = "buttonareas"; break;
		case LIBINPUT_CONFIG_CLICK_METHOD_CLICKFINGER: method = "clickfinger"; break;
		default:
			method = "unknown"; break;
		}

		xf86IDrvMsg(pInfo, X_ERROR,
			    "Failed to set click method to %s\n",
			    method);
	}

	if (libinput_device_config_middle_emulation_is_available(device) &&
	    libinput_device_config_middle_emulation_set_enabled(device,
								driver_data->options.middle_emulation) != LIBINPUT_CONFIG_STATUS_SUCCESS)
		xf86IDrvMsg(pInfo, X_ERROR,
			    "Failed to set MiddleEmulation to %d\n",
			    driver_data->options.middle_emulation);

	if (libinput_device_config_halfkey_is_available(device) &&
	    libinput_device_config_halfkey_set_enabled(device,
						       driver_data->options.halfkey) != LIBINPUT_CONFIG_STATUS_SUCCESS)
		xf86IDrvMsg(pInfo, X_ERROR,
			    "Failed to set Halfkey Accessiblity support to %d\n",
			    driver_data->options.halfkey);
}

static int
xf86libinput_on(DeviceIntPtr dev)
{
	InputInfoPtr pInfo = dev->public.devicePrivate;
	struct xf86libinput *driver_data = pInfo->private;
	struct libinput *libinput = driver_context.libinput;
	struct libinput_device *device = driver_data->device;

	if (use_server_fd(pInfo)) {
		char *path = xf86SetStrOption(pInfo->options, "Device", NULL);
		fd_push(&driver_context, pInfo->fd, path);
		free(path);
	}

	device = libinput_path_add_device(libinput, driver_data->path);
	if (!device)
		return !Success;

	libinput_device_ref(device);
	libinput_device_set_user_data(device, pInfo);
	driver_data->device = device;

	/* if we use server fds, overwrite the fd with the one from
	   libinput nonetheless, otherwise the server won't call ReadInput
	   for our device. This must be swapped back to the real fd in
	   DEVICE_OFF so systemd-logind closes the right fd */
	pInfo->fd = libinput_get_fd(libinput);

	if (driver_context.device_enabled_count == 0) {
		/* Can't use xf86AddEnabledDevice on an epollfd */
		AddEnabledDevice(pInfo->fd);
	}

	driver_context.device_enabled_count++;
	dev->public.on = TRUE;

	LibinputApplyConfig(dev);

	return Success;
}

static int
xf86libinput_off(DeviceIntPtr dev)
{
	InputInfoPtr pInfo = dev->public.devicePrivate;
	struct xf86libinput *driver_data = pInfo->private;

	if (--driver_context.device_enabled_count == 0) {
		RemoveEnabledDevice(pInfo->fd);
	}

	if (use_server_fd(pInfo)) {
		fd_pop(&driver_context, pInfo->fd);
		pInfo->fd = xf86SetIntOption(pInfo->options, "fd", -1);
	} else {
		pInfo->fd = -1;
	}

	dev->public.on = FALSE;

	libinput_device_set_user_data(driver_data->device, NULL);
	libinput_path_remove_device(driver_data->device);
	libinput_device_unref(driver_data->device);
	driver_data->device = NULL;

	return Success;
}

static void
xf86libinput_ptr_ctl(DeviceIntPtr dev, PtrCtrl *ctl)
{
}

static void
init_button_map(unsigned char *btnmap, size_t size)
{
	int i;

	memset(btnmap, 0, size);
	for (i = 0; i < size; i++)
		btnmap[i] = i;
}

static void
init_button_labels(Atom *labels, size_t size)
{
	assert(size > 10);

	memset(labels, 0, size * sizeof(Atom));
	labels[0] = XIGetKnownProperty(BTN_LABEL_PROP_BTN_LEFT);
	labels[1] = XIGetKnownProperty(BTN_LABEL_PROP_BTN_MIDDLE);
	labels[2] = XIGetKnownProperty(BTN_LABEL_PROP_BTN_RIGHT);
	labels[3] = XIGetKnownProperty(BTN_LABEL_PROP_BTN_WHEEL_UP);
	labels[4] = XIGetKnownProperty(BTN_LABEL_PROP_BTN_WHEEL_DOWN);
	labels[5] = XIGetKnownProperty(BTN_LABEL_PROP_BTN_HWHEEL_LEFT);
	labels[6] = XIGetKnownProperty(BTN_LABEL_PROP_BTN_HWHEEL_RIGHT);
	labels[7] = XIGetKnownProperty(BTN_LABEL_PROP_BTN_SIDE);
	labels[8] = XIGetKnownProperty(BTN_LABEL_PROP_BTN_EXTRA);
	labels[9] = XIGetKnownProperty(BTN_LABEL_PROP_BTN_FORWARD);
	labels[10] = XIGetKnownProperty(BTN_LABEL_PROP_BTN_BACK);
}

static void
init_axis_labels(Atom *labels, size_t size)
{
	memset(labels, 0, size * sizeof(Atom));
	labels[0] = XIGetKnownProperty(AXIS_LABEL_PROP_REL_X);
	labels[1] = XIGetKnownProperty(AXIS_LABEL_PROP_REL_Y);
	labels[2] = XIGetKnownProperty(AXIS_LABEL_PROP_REL_HSCROLL);
	labels[3] = XIGetKnownProperty(AXIS_LABEL_PROP_REL_VSCROLL);
}

static int
xf86libinput_init_pointer(InputInfoPtr pInfo)
{
	DeviceIntPtr dev= pInfo->dev;
	struct xf86libinput *driver_data = pInfo->private;
	int min, max, res;
	int nbuttons = 7;
	int i;

	Atom btnlabels[MAX_BUTTONS];
	Atom axislabels[TOUCHPAD_NUM_AXES];

	for (i = BTN_JOYSTICK - 1; i >= BTN_SIDE; i--) {
		if (libinput_device_pointer_has_button(driver_data->device, i)) {
			nbuttons += i - BTN_SIDE + 1;
			break;
		}
	}

	init_button_labels(btnlabels, ARRAY_SIZE(btnlabels));
	init_axis_labels(axislabels, ARRAY_SIZE(axislabels));

	InitPointerDeviceStruct((DevicePtr)dev,
				driver_data->options.btnmap,
				nbuttons,
				btnlabels,
				xf86libinput_ptr_ctl,
				GetMotionHistorySize(),
				TOUCHPAD_NUM_AXES,
				axislabels);
	min = -1;
	max = -1;
	res = 0;

	xf86InitValuatorAxisStruct(dev, 0,
			           XIGetKnownProperty(AXIS_LABEL_PROP_REL_X),
				   min, max, res * 1000, 0, res * 1000, Relative);
	xf86InitValuatorAxisStruct(dev, 1,
			           XIGetKnownProperty(AXIS_LABEL_PROP_REL_Y),
				   min, max, res * 1000, 0, res * 1000, Relative);

	SetScrollValuator(dev, 2, SCROLL_TYPE_HORIZONTAL, driver_data->scroll.hdist, 0);
	SetScrollValuator(dev, 3, SCROLL_TYPE_VERTICAL, driver_data->scroll.vdist, 0);

	return Success;
}

static int
xf86libinput_init_pointer_absolute(InputInfoPtr pInfo)
{
	DeviceIntPtr dev= pInfo->dev;
	struct xf86libinput *driver_data = pInfo->private;
	int min, max, res;
	int nbuttons = 7;
	int i;

	Atom btnlabels[MAX_BUTTONS];
	Atom axislabels[TOUCHPAD_NUM_AXES];

	for (i = BTN_BACK; i >= BTN_SIDE; i--) {
		if (libinput_device_pointer_has_button(driver_data->device, i)) {
			nbuttons += i - BTN_SIDE + 1;
			break;
		}
	}

	init_button_labels(btnlabels, ARRAY_SIZE(btnlabels));
	init_axis_labels(axislabels, ARRAY_SIZE(axislabels));

	InitPointerDeviceStruct((DevicePtr)dev,
				driver_data->options.btnmap,
				nbuttons,
				btnlabels,
				xf86libinput_ptr_ctl,
				GetMotionHistorySize(),
				TOUCHPAD_NUM_AXES,
				axislabels);
	min = 0;
	max = TOUCH_AXIS_MAX;
	res = 0;

	xf86InitValuatorAxisStruct(dev, 0,
			           XIGetKnownProperty(AXIS_LABEL_PROP_ABS_X),
				   min, max, res * 1000, 0, res * 1000, Absolute);
	xf86InitValuatorAxisStruct(dev, 1,
			           XIGetKnownProperty(AXIS_LABEL_PROP_ABS_Y),
				   min, max, res * 1000, 0, res * 1000, Absolute);

	SetScrollValuator(dev, 2, SCROLL_TYPE_HORIZONTAL, driver_data->scroll.hdist, 0);
	SetScrollValuator(dev, 3, SCROLL_TYPE_VERTICAL, driver_data->scroll.vdist, 0);

	driver_data->has_abs = TRUE;

	return Success;
}
static void
xf86libinput_kbd_ctrl(DeviceIntPtr device, KeybdCtrl *ctrl)
{
#define CAPSFLAG	1
#define NUMFLAG		2
#define SCROLLFLAG	4

    static struct { int xbit, code; } bits[] = {
        { CAPSFLAG,	LIBINPUT_LED_CAPS_LOCK },
        { NUMFLAG,	LIBINPUT_LED_NUM_LOCK },
        { SCROLLFLAG,	LIBINPUT_LED_SCROLL_LOCK },
	{ 0, 0 },
    };
    int i = 0;
    enum libinput_led leds = 0;
    InputInfoPtr pInfo = device->public.devicePrivate;
    struct xf86libinput *driver_data = pInfo->private;
    struct libinput_device *ldevice = driver_data->device;

    while (bits[i].xbit) {
	    if (ctrl->leds & bits[i].xbit)
		    leds |= bits[i].code;
	    i++;
    }

    libinput_device_led_update(ldevice, leds);
}

static void
xf86libinput_init_keyboard(InputInfoPtr pInfo)
{
	DeviceIntPtr dev= pInfo->dev;
	XkbRMLVOSet rmlvo = {0};
	XkbRMLVOSet defaults = {0};

	XkbGetRulesDflts(&defaults);

	rmlvo.rules = xf86SetStrOption(pInfo->options,
				       "xkb_rules",
				       defaults.rules);
	rmlvo.model = xf86SetStrOption(pInfo->options,
				       "xkb_model",
				       defaults.model);
	rmlvo.layout = xf86SetStrOption(pInfo->options,
					"xkb_layout",
					defaults.layout);
	rmlvo.variant = xf86SetStrOption(pInfo->options,
					 "xkb_variant",
					 defaults.variant);
	rmlvo.options = xf86SetStrOption(pInfo->options,
					 "xkb_options",
					 defaults.options);

	InitKeyboardDeviceStruct(dev, &rmlvo, NULL,
				 xf86libinput_kbd_ctrl);
	XkbFreeRMLVOSet(&rmlvo, FALSE);
	XkbFreeRMLVOSet(&defaults, FALSE);
}

static void
xf86libinput_init_touch(InputInfoPtr pInfo)
{
	DeviceIntPtr dev = pInfo->dev;
	struct xf86libinput *driver_data = pInfo->private;
	int min, max, res;
	unsigned char btnmap[MAX_BUTTONS + 1];
	Atom btnlabels[MAX_BUTTONS];
	Atom axislabels[TOUCHPAD_NUM_AXES];
	int nbuttons = 7;

	init_button_map(btnmap, ARRAY_SIZE(btnmap));
	init_button_labels(btnlabels, ARRAY_SIZE(btnlabels));
	init_axis_labels(axislabels, ARRAY_SIZE(axislabels));

	InitPointerDeviceStruct((DevicePtr)dev,
				driver_data->options.btnmap,
				nbuttons,
				btnlabels,
				xf86libinput_ptr_ctl,
				GetMotionHistorySize(),
				TOUCHPAD_NUM_AXES,
				axislabels);
	min = 0;
	max = TOUCH_AXIS_MAX;
	res = 0;

	xf86InitValuatorAxisStruct(dev, 0,
			           XIGetKnownProperty(AXIS_LABEL_PROP_ABS_X),
				   min, max, res * 1000, 0, res * 1000, Absolute);
	xf86InitValuatorAxisStruct(dev, 1,
			           XIGetKnownProperty(AXIS_LABEL_PROP_ABS_Y),
				   min, max, res * 1000, 0, res * 1000, Absolute);
	InitTouchClassDeviceStruct(dev, TOUCH_MAX_SLOTS, XIDirectTouch, 2);

}

static int
xf86libinput_init(DeviceIntPtr dev)
{
	InputInfoPtr pInfo = dev->public.devicePrivate;
	struct xf86libinput *driver_data = pInfo->private;
	struct libinput_device *device = driver_data->device;

	dev->public.on = FALSE;

	if (libinput_device_has_capability(device, LIBINPUT_DEVICE_CAP_KEYBOARD))
		xf86libinput_init_keyboard(pInfo);
	if (libinput_device_has_capability(device, LIBINPUT_DEVICE_CAP_POINTER)) {
		if (libinput_device_config_calibration_has_matrix(device) &&
		    !libinput_device_config_accel_is_available(device))
			xf86libinput_init_pointer_absolute(pInfo);
		else
			xf86libinput_init_pointer(pInfo);
	}
	if (libinput_device_has_capability(device, LIBINPUT_DEVICE_CAP_TOUCH))
		xf86libinput_init_touch(pInfo);

	LibinputApplyConfig(dev);
	LibinputInitProperty(dev);
	XIRegisterPropertyHandler(dev, LibinputSetProperty, NULL, NULL);

	/* unref the device now, because we'll get a new ref during
	   DEVICE_ON */
	libinput_device_unref(device);

	return 0;
}

static void
xf86libinput_destroy(DeviceIntPtr dev)
{
}

static int
xf86libinput_device_control(DeviceIntPtr dev, int mode)
{
	int rc = BadValue;

	switch(mode) {
		case DEVICE_INIT:
			rc = xf86libinput_init(dev);
			break;
		case DEVICE_ON:
			rc = xf86libinput_on(dev);
			break;
		case DEVICE_OFF:
			rc = xf86libinput_off(dev);
			break;
		case DEVICE_CLOSE:
			xf86libinput_destroy(dev);
			break;
	}

	return rc;
}

static void
xf86libinput_handle_motion(InputInfoPtr pInfo, struct libinput_event_pointer *event)
{
	DeviceIntPtr dev = pInfo->dev;
	struct xf86libinput *driver_data = pInfo->private;
	ValuatorMask *mask = driver_data->valuators;
	double x, y;

	x = libinput_event_pointer_get_dx(event);
	y = libinput_event_pointer_get_dy(event);

	valuator_mask_zero(mask);

#if HAVE_VMASK_UNACCEL
	{
		double ux, uy;

		ux = libinput_event_pointer_get_dx_unaccelerated(event);
		uy = libinput_event_pointer_get_dy_unaccelerated(event);

		valuator_mask_set_unaccelerated(mask, 0, x, ux);
		valuator_mask_set_unaccelerated(mask, 1, y, uy);
	}
#else
	valuator_mask_set_double(mask, 0, x);
	valuator_mask_set_double(mask, 1, y);
#endif
	xf86PostMotionEventM(dev, Relative, mask);
}

static void
xf86libinput_handle_absmotion(InputInfoPtr pInfo, struct libinput_event_pointer *event)
{
	DeviceIntPtr dev = pInfo->dev;
	struct xf86libinput *driver_data = pInfo->private;
	ValuatorMask *mask = driver_data->valuators;
	double x, y;

	if (!driver_data->has_abs) {
		xf86IDrvMsg(pInfo, X_ERROR,
			    "Discarding absolute event from relative device. "
			    "Please file a bug\n");
		return;
	}

	x = libinput_event_pointer_get_absolute_x_transformed(event, TOUCH_AXIS_MAX);
	y = libinput_event_pointer_get_absolute_y_transformed(event, TOUCH_AXIS_MAX);

	valuator_mask_zero(mask);
	valuator_mask_set_double(mask, 0, x);
	valuator_mask_set_double(mask, 1, y);

	xf86PostMotionEventM(dev, Absolute, mask);
}

static void
xf86libinput_handle_button(InputInfoPtr pInfo, struct libinput_event_pointer *event)
{
	DeviceIntPtr dev = pInfo->dev;
	int button;
	int is_press;

	button = btn_linux2xorg(libinput_event_pointer_get_button(event));
	is_press = (libinput_event_pointer_get_button_state(event) == LIBINPUT_BUTTON_STATE_PRESSED);
	xf86PostButtonEvent(dev, Relative, button, is_press, 0, 0);
}

static void
xf86libinput_handle_key(InputInfoPtr pInfo, struct libinput_event_keyboard *event)
{
	DeviceIntPtr dev = pInfo->dev;
	int is_press;
	int key = libinput_event_keyboard_get_key(event);

	key += XORG_KEYCODE_OFFSET;

	is_press = (libinput_event_keyboard_get_key_state(event) == LIBINPUT_KEY_STATE_PRESSED);
	xf86PostKeyboardEvent(dev, key, is_press);
}

static void
xf86libinput_handle_axis(InputInfoPtr pInfo, struct libinput_event_pointer *event)
{
	DeviceIntPtr dev = pInfo->dev;
	struct xf86libinput *driver_data = pInfo->private;
	ValuatorMask *mask = driver_data->valuators;
	double value;
	enum libinput_pointer_axis axis;
	enum libinput_pointer_axis_source source;

	valuator_mask_zero(mask);

	source = libinput_event_pointer_get_axis_source(event);
	switch(source) {
		case LIBINPUT_POINTER_AXIS_SOURCE_FINGER:
		case LIBINPUT_POINTER_AXIS_SOURCE_WHEEL:
		case LIBINPUT_POINTER_AXIS_SOURCE_CONTINUOUS:
			break;
		default:
			return;
	}

	axis = LIBINPUT_POINTER_AXIS_SCROLL_VERTICAL;
	if (libinput_event_pointer_has_axis(event, axis)) {
		if (source == LIBINPUT_POINTER_AXIS_SOURCE_WHEEL) {
			value = libinput_event_pointer_get_axis_value_discrete(event, axis);
			value *=  driver_data->scroll.vdist;
		} else {
			value = libinput_event_pointer_get_axis_value(event, axis);
		}
		valuator_mask_set_double(mask, 3, value);
	}
	axis = LIBINPUT_POINTER_AXIS_SCROLL_HORIZONTAL;
	if (libinput_event_pointer_has_axis(event, axis)) {
		if (source == LIBINPUT_POINTER_AXIS_SOURCE_WHEEL) {
			value = libinput_event_pointer_get_axis_value_discrete(event, axis);
			value *=  driver_data->scroll.hdist;
		} else {
			value = libinput_event_pointer_get_axis_value(event, axis);
		}
		valuator_mask_set_double(mask, 2, value);
	}

	xf86PostMotionEventM(dev, Relative, mask);
}

static void
xf86libinput_handle_touch(InputInfoPtr pInfo,
			  struct libinput_event_touch *event,
			  enum libinput_event_type event_type)
{
	DeviceIntPtr dev = pInfo->dev;
	struct xf86libinput *driver_data = pInfo->private;
	int type;
	int slot;
	ValuatorMask *m = driver_data->valuators;
	double val;

	/* libinput doesn't give us hw touch ids which X expects, so
	   emulate them here */
	static unsigned int next_touchid;
	static unsigned int touchids[TOUCH_MAX_SLOTS] = {0};

	slot = libinput_event_touch_get_slot(event);

	switch (event_type) {
		case LIBINPUT_EVENT_TOUCH_DOWN:
			type = XI_TouchBegin;
			touchids[slot] = next_touchid++;
			break;
		case LIBINPUT_EVENT_TOUCH_UP:
			type = XI_TouchEnd;
			break;
		case LIBINPUT_EVENT_TOUCH_MOTION:
			type = XI_TouchUpdate;
			break;
		default:
			return;
	};

	valuator_mask_zero(m);

	if (event_type != LIBINPUT_EVENT_TOUCH_UP) {
		val = libinput_event_touch_get_x_transformed(event, TOUCH_AXIS_MAX);
		valuator_mask_set_double(m, 0, val);

		val = libinput_event_touch_get_y_transformed(event, TOUCH_AXIS_MAX);
		valuator_mask_set_double(m, 1, val);
	}

	xf86PostTouchEvent(dev, touchids[slot], type, 0, m);
}

static void
xf86libinput_handle_event(struct libinput_event *event)
{
	struct libinput_device *device;
	InputInfoPtr pInfo;

	device = libinput_event_get_device(event);
	pInfo = libinput_device_get_user_data(device);

	if (!pInfo || !pInfo->dev->public.on)
		return;

	switch (libinput_event_get_type(event)) {
		case LIBINPUT_EVENT_NONE:
		case LIBINPUT_EVENT_DEVICE_ADDED:
		case LIBINPUT_EVENT_DEVICE_REMOVED:
			break;
		case LIBINPUT_EVENT_POINTER_MOTION_ABSOLUTE:
			xf86libinput_handle_absmotion(pInfo,
						      libinput_event_get_pointer_event(event));
			break;

		case LIBINPUT_EVENT_POINTER_MOTION:
			xf86libinput_handle_motion(pInfo,
						   libinput_event_get_pointer_event(event));
			break;
		case LIBINPUT_EVENT_POINTER_BUTTON:
			xf86libinput_handle_button(pInfo,
						   libinput_event_get_pointer_event(event));
			break;
		case LIBINPUT_EVENT_KEYBOARD_KEY:
			xf86libinput_handle_key(pInfo,
						libinput_event_get_keyboard_event(event));
			break;
		case LIBINPUT_EVENT_POINTER_AXIS:
			xf86libinput_handle_axis(pInfo,
						 libinput_event_get_pointer_event(event));
			break;
		case LIBINPUT_EVENT_TOUCH_FRAME:
			break;
		case LIBINPUT_EVENT_TOUCH_UP:
		case LIBINPUT_EVENT_TOUCH_DOWN:
		case LIBINPUT_EVENT_TOUCH_MOTION:
		case LIBINPUT_EVENT_TOUCH_CANCEL:
			xf86libinput_handle_touch(pInfo,
						  libinput_event_get_touch_event(event),
						  libinput_event_get_type(event));
			break;
	}
}

static void
xf86libinput_read_input(InputInfoPtr pInfo)
{
	struct libinput *libinput = driver_context.libinput;
	int rc;
	struct libinput_event *event;

        rc = libinput_dispatch(libinput);
	if (rc == -EAGAIN)
		return;

	if (rc < 0) {
		xf86IDrvMsg(pInfo, X_ERROR,
			    "Error reading events: %s\n",
			    strerror(-rc));
		return;
	}

	while ((event = libinput_get_event(libinput))) {
		xf86libinput_handle_event(event);
		libinput_event_destroy(event);
	}
}

static int
open_restricted(const char *path, int flags, void *data)
{
	struct xf86libinput_driver *context = data;
	int fd;

	fd = fd_get(context, path);
	if (fd == -1)
		fd = open(path, flags);
	return fd < 0 ? -errno : fd;
}

static void
close_restricted(int fd, void *data)
{
	struct xf86libinput_driver *context = data;

	if (fd_find(context, fd) == -1)
		close(fd);
}

const struct libinput_interface interface = {
    .open_restricted = open_restricted,
    .close_restricted = close_restricted,
};

static void
xf86libinput_log_handler(struct libinput *libinput,
			 enum libinput_log_priority priority,
			 const char *format,
			 va_list args)
	_X_ATTRIBUTE_PRINTF(3, 0);

static void
xf86libinput_log_handler(struct libinput *libinput,
			 enum libinput_log_priority priority,
			 const char *format,
			 va_list args)
{
	MessageType type;
	int verbosity;

	switch(priority) {
	case LIBINPUT_LOG_PRIORITY_DEBUG:
		type = X_DEBUG;
		verbosity = 10;
		break;
	case LIBINPUT_LOG_PRIORITY_ERROR:
		type = X_ERROR;
		verbosity = -1;
		break;
	case LIBINPUT_LOG_PRIORITY_INFO:
		type = X_INFO;
		verbosity = 3;
		break;
	default:
		return;
	}

	/* log messages in libinput are per-context, not per device, so we
	   can't use xf86IDrvMsg here, and the server has no xf86VMsg or
	   similar */
	LogVMessageVerb(type, verbosity, format, args);
}

static inline BOOL
xf86libinput_parse_tap_option(InputInfoPtr pInfo,
			      struct libinput_device *device)
{
	BOOL tap;

	if (libinput_device_config_tap_get_finger_count(device) == 0)
		return FALSE;

	tap = xf86SetBoolOption(pInfo->options,
				"Tapping",
				libinput_device_config_tap_get_enabled(device));

	if (libinput_device_config_tap_set_enabled(device, tap) !=
	    LIBINPUT_CONFIG_STATUS_SUCCESS) {
		xf86IDrvMsg(pInfo, X_ERROR,
			    "Failed to set Tapping to %d\n",
			    tap);
		tap = libinput_device_config_tap_get_enabled(device);
	}

	return tap;
}

static inline BOOL
xf86libinput_parse_tap_drag_lock_option(InputInfoPtr pInfo,
					struct libinput_device *device)
{
	BOOL drag_lock;

	if (libinput_device_config_tap_get_finger_count(device) == 0)
		return FALSE;

	drag_lock = xf86SetBoolOption(pInfo->options,
				      "TappingDragLock",
				      libinput_device_config_tap_get_drag_lock_enabled(device));

	if (libinput_device_config_tap_set_drag_lock_enabled(device, drag_lock) !=
	    LIBINPUT_CONFIG_STATUS_SUCCESS) {
		xf86IDrvMsg(pInfo, X_ERROR,
			    "Failed to set Tapping Drag Lock to %d\n",
			    drag_lock);
		drag_lock = libinput_device_config_tap_get_drag_lock_enabled(device);
	}

	return drag_lock;
}

static inline double
xf86libinput_parse_accel_option(InputInfoPtr pInfo,
				struct libinput_device *device)
{
	double speed;

	if (!libinput_device_config_accel_is_available(device))
		return 0.0;

	speed = xf86SetRealOption(pInfo->options,
				  "AccelSpeed",
				  libinput_device_config_accel_get_speed(device));
	if (libinput_device_config_accel_set_speed(device, speed) !=
	    LIBINPUT_CONFIG_STATUS_SUCCESS) {
		xf86IDrvMsg(pInfo, X_ERROR,
			    "Invalid speed %.2f, using 0 instead\n",
			    speed);
		speed = libinput_device_config_accel_get_speed(device);
	}

	return speed;
}

static inline BOOL
xf86libinput_parse_natscroll_option(InputInfoPtr pInfo,
				    struct libinput_device *device)
{
	BOOL natural_scroll;

	if (!libinput_device_config_scroll_has_natural_scroll(device))
		return FALSE;

	natural_scroll = xf86SetBoolOption(pInfo->options,
					   "NaturalScrolling",
					   libinput_device_config_scroll_get_natural_scroll_enabled(device));
	if (libinput_device_config_scroll_set_natural_scroll_enabled(device,
								     natural_scroll) !=
	    LIBINPUT_CONFIG_STATUS_SUCCESS) {
		xf86IDrvMsg(pInfo, X_ERROR,
			    "Failed to set NaturalScrolling to %d\n",
			    natural_scroll);

		natural_scroll = libinput_device_config_scroll_get_natural_scroll_enabled(device);
	}

	return natural_scroll;
}

static inline enum libinput_config_send_events_mode
xf86libinput_parse_sendevents_option(InputInfoPtr pInfo,
				     struct libinput_device *device)
{
	char *strmode;
	enum libinput_config_send_events_mode mode;

	if (libinput_device_config_send_events_get_modes(device) == LIBINPUT_CONFIG_SEND_EVENTS_ENABLED)
		return LIBINPUT_CONFIG_SEND_EVENTS_ENABLED;

	mode = libinput_device_config_send_events_get_mode(device);
	strmode = xf86SetStrOption(pInfo->options,
				   "SendEventsMode",
				   NULL);
	if (strmode) {
		if (strcmp(strmode, "enabled") == 0)
			mode = LIBINPUT_CONFIG_SEND_EVENTS_ENABLED;
		else if (strcmp(strmode, "disabled") == 0)
			mode = LIBINPUT_CONFIG_SEND_EVENTS_DISABLED;
		else if (strcmp(strmode, "disabled-on-external-mouse") == 0)
			mode = LIBINPUT_CONFIG_SEND_EVENTS_DISABLED_ON_EXTERNAL_MOUSE;
		else
			xf86IDrvMsg(pInfo, X_ERROR,
				    "Invalid SendeventsMode: %s\n",
				    strmode);
		free(strmode);
	}

	if (libinput_device_config_send_events_set_mode(device, mode) !=
	    LIBINPUT_CONFIG_STATUS_SUCCESS) {
		xf86IDrvMsg(pInfo, X_ERROR,
			    "Failed to set SendEventsMode %u\n", mode);
		mode = libinput_device_config_send_events_get_mode(device);
	}

	return mode;
}

static inline void
xf86libinput_parse_calibration_option(InputInfoPtr pInfo,
				      struct libinput_device *device,
				      float matrix_out[9])
{
	char *str;
	float matrix[9] = { 1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0};

	memcpy(matrix_out, matrix, sizeof(matrix));

	if (!libinput_device_config_calibration_has_matrix(device))
		return;

	libinput_device_config_calibration_get_matrix(device, matrix);
	memcpy(matrix_out, matrix, sizeof(matrix));

	if ((str = xf86CheckStrOption(pInfo->options,
				      "CalibrationMatrix",
				      NULL))) {
		int num_calibration = sscanf(str, "%f %f %f %f %f %f %f %f %f ",
					     &matrix[0], &matrix[1],
					     &matrix[2], &matrix[3],
					     &matrix[4], &matrix[5],
					     &matrix[6], &matrix[7],
					     &matrix[8]);
		if (num_calibration != 9) {
			xf86IDrvMsg(pInfo, X_ERROR,
				    "Invalid matrix: %s, using default\n",  str);
		} else if (libinput_device_config_calibration_set_matrix(device,
									 matrix) ==
			   LIBINPUT_CONFIG_STATUS_SUCCESS) {
			memcpy(matrix_out, matrix, sizeof(matrix));
		} else
			xf86IDrvMsg(pInfo, X_ERROR,
				    "Failed to apply matrix: %s, using default\n",  str);
		free(str);
	}
}

static inline BOOL
xf86libinput_parse_lefthanded_option(InputInfoPtr pInfo,
				     struct libinput_device *device)
{
	BOOL left_handed;

	if (!libinput_device_config_left_handed_is_available(device))
		return FALSE;

	left_handed = xf86SetBoolOption(pInfo->options,
					"LeftHanded",
					libinput_device_config_left_handed_get(device));
	if (libinput_device_config_left_handed_set(device,
						   left_handed) !=
	    LIBINPUT_CONFIG_STATUS_SUCCESS) {
		xf86IDrvMsg(pInfo, X_ERROR,
			    "Failed to set LeftHanded to %d\n",
			    left_handed);
		left_handed = libinput_device_config_left_handed_get(device);
	}

	return left_handed;
}

static inline enum libinput_config_scroll_method
xf86libinput_parse_scroll_option(InputInfoPtr pInfo,
				 struct libinput_device *device)
{
	uint32_t scroll_methods;
	enum libinput_config_scroll_method m;
	char *method;

	scroll_methods = libinput_device_config_scroll_get_methods(device);
	if (scroll_methods == LIBINPUT_CONFIG_SCROLL_NO_SCROLL)
		return LIBINPUT_CONFIG_SCROLL_NO_SCROLL;

	method = xf86SetStrOption(pInfo->options, "ScrollMethod", NULL);
	if (!method)
		m = libinput_device_config_scroll_get_method(device);
	else if (strncasecmp(method, "twofinger", 9) == 0)
		m = LIBINPUT_CONFIG_SCROLL_2FG;
	else if (strncasecmp(method, "edge", 4) == 0)
		m = LIBINPUT_CONFIG_SCROLL_EDGE;
	else if (strncasecmp(method, "button", 6) == 0)
		m = LIBINPUT_CONFIG_SCROLL_ON_BUTTON_DOWN;
	else if (strncasecmp(method, "none", 4) == 0)
		m = LIBINPUT_CONFIG_SCROLL_NO_SCROLL;
	else {
		xf86IDrvMsg(pInfo, X_ERROR,
			    "Unknown scroll method '%s'. Using default.\n",
			    method);
		m = libinput_device_config_scroll_get_method(device);
	}

	free(method);
	return m;
}

static inline unsigned int
xf86libinput_parse_scrollbutton_option(InputInfoPtr pInfo,
				       struct libinput_device *device)
{
	unsigned int b;
	CARD32 scroll_button;

	if ((libinput_device_config_scroll_get_methods(device) &
	    LIBINPUT_CONFIG_SCROLL_ON_BUTTON_DOWN) == 0)
		return 0;

	b = btn_linux2xorg(libinput_device_config_scroll_get_button(device));
	scroll_button = xf86SetIntOption(pInfo->options,
					 "ScrollButton",
					 b);

	b = btn_xorg2linux(scroll_button);

	if (libinput_device_config_scroll_set_button(device,
						     b) != LIBINPUT_CONFIG_STATUS_SUCCESS) {
		xf86IDrvMsg(pInfo, X_ERROR,
			    "Failed to set ScrollButton to %u\n",
			    scroll_button);
		scroll_button = btn_linux2xorg(libinput_device_config_scroll_get_button(device));
	}
	return scroll_button;
}

static inline unsigned int
xf86libinput_parse_clickmethod_option(InputInfoPtr pInfo,
				      struct libinput_device *device)
{
	uint32_t click_methods = libinput_device_config_click_get_methods(device);
	enum libinput_config_click_method m;
	char *method;

	if (click_methods == LIBINPUT_CONFIG_CLICK_METHOD_NONE)
		return LIBINPUT_CONFIG_CLICK_METHOD_NONE;

	method = xf86SetStrOption(pInfo->options, "ClickMethod", NULL);

	if (!method)
		m = libinput_device_config_click_get_method(device);
	else if (strncasecmp(method, "buttonareas", 11) == 0)
		m = LIBINPUT_CONFIG_CLICK_METHOD_BUTTON_AREAS;
	else if (strncasecmp(method, "clickfinger", 11) == 0)
		m = LIBINPUT_CONFIG_CLICK_METHOD_CLICKFINGER;
	else if (strncasecmp(method, "none", 4) == 0)
		m = LIBINPUT_CONFIG_CLICK_METHOD_NONE;
	else {
		xf86IDrvMsg(pInfo, X_ERROR,
			    "Unknown click method '%s'. Using default.\n",
			    method);
		m = libinput_device_config_click_get_method(device);
	}
	free(method);

	return m;
}

static inline BOOL
xf86libinput_parse_middleemulation_option(InputInfoPtr pInfo,
					  struct libinput_device *device)
{
	BOOL enabled;

	if (!libinput_device_config_middle_emulation_is_available(device))
		return FALSE;

	enabled = xf86SetBoolOption(pInfo->options,
				    "MiddleEmulation",
				    libinput_device_config_middle_emulation_get_default_enabled(device));
	if (libinput_device_config_middle_emulation_set_enabled(device, enabled) !=
	    LIBINPUT_CONFIG_STATUS_SUCCESS) {
		xf86IDrvMsg(pInfo, X_ERROR,
			    "Failed to set MiddleEmulation to %d\n",
			    enabled);
		enabled = libinput_device_config_middle_emulation_get_enabled(device);
	}

	return enabled;
}

static inline BOOL
xf86libinput_parse_halfkey_option(InputInfoPtr pInfo,
				  struct libinput_device *device)
{
	BOOL enabled;

	if (!libinput_device_config_halfkey_is_available(device))
		return FALSE;

	enabled = xf86SetBoolOption(pInfo->options,
				    "Halfkey",
				    libinput_device_config_halfkey_get_default_enabled(device));
	if (libinput_device_config_halfkey_set_enabled(device, enabled) !=
	    LIBINPUT_CONFIG_STATUS_SUCCESS) {
		xf86IDrvMsg(pInfo, X_ERROR,
			    "Failed to set Halfkey Accessiblity to %d\n",
			    enabled);
		enabled = libinput_device_config_halfkey_get_enabled(device);
	}

	return enabled;
}

static void
xf86libinput_parse_buttonmap_option(InputInfoPtr pInfo,
				    unsigned char *btnmap,
				    size_t size)
{
	const int MAXBUTTONS = 32;
	char *mapping, *map, *s = NULL;
	int idx = 1;

	init_button_map(btnmap, size);

	mapping = xf86SetStrOption(pInfo->options, "ButtonMapping", NULL);
	if (!mapping)
		return;

	map = mapping;
	do
	{
		unsigned long int btn = strtoul(map, &s, 10);

		if (s == map || btn > MAXBUTTONS)
		{
			xf86IDrvMsg(pInfo, X_ERROR,
				    "... Invalid button mapping. Using defaults\n");
			init_button_map(btnmap, size);
			break;
		}

		btnmap[idx++] = btn;
		map = s;
	} while (s && *s != '\0' && idx < MAXBUTTONS);

	free(mapping);
}

static void
xf86libinput_parse_options(InputInfoPtr pInfo,
			   struct xf86libinput *driver_data,
			   struct libinput_device *device)
{
	struct options *options = &driver_data->options;

	/* libinput options */
	options->tapping = xf86libinput_parse_tap_option(pInfo, device);
	options->tap_drag_lock = xf86libinput_parse_tap_drag_lock_option(pInfo, device);
	options->speed = xf86libinput_parse_accel_option(pInfo, device);
	options->natural_scrolling = xf86libinput_parse_natscroll_option(pInfo, device);
	options->sendevents = xf86libinput_parse_sendevents_option(pInfo, device);
	options->left_handed = xf86libinput_parse_lefthanded_option(pInfo, device);
	options->scroll_method = xf86libinput_parse_scroll_option(pInfo, device);
	options->scroll_button = xf86libinput_parse_scrollbutton_option(pInfo, device);
	options->click_method = xf86libinput_parse_clickmethod_option(pInfo, device);
	options->middle_emulation = xf86libinput_parse_middleemulation_option(pInfo, device);
	options->halfkey = xf86libinput_parse_halfkey_option(pInfo, device);
	xf86libinput_parse_calibration_option(pInfo, device, driver_data->options.matrix);

	/* non-libinput options */
	xf86libinput_parse_buttonmap_option(pInfo,
					    options->btnmap,
					    sizeof(options->btnmap));
}

static int
xf86libinput_pre_init(InputDriverPtr drv,
		      InputInfoPtr pInfo,
		      int flags)
{
	struct xf86libinput *driver_data = NULL;
        struct libinput *libinput = NULL;
	struct libinput_device *device;
	char *path = NULL;

	pInfo->type_name = 0;
	pInfo->device_control = xf86libinput_device_control;
	pInfo->read_input = xf86libinput_read_input;
	pInfo->control_proc = NULL;
	pInfo->switch_mode = NULL;

	driver_data = calloc(1, sizeof(*driver_data));
	if (!driver_data)
		goto fail;

	driver_data->valuators = valuator_mask_new(2);
	if (!driver_data->valuators)
		goto fail;

	driver_data->valuators_unaccelerated = valuator_mask_new(2);
	if (!driver_data->valuators_unaccelerated)
		goto fail;

	driver_data->scroll.vdist = 15;
	driver_data->scroll.hdist = 15;

	path = xf86SetStrOption(pInfo->options, "Device", NULL);
	if (!path)
		goto fail;

	if (!driver_context.libinput) {
		driver_context.libinput = libinput_path_create_context(&interface, &driver_context);
		libinput_log_set_handler(driver_context.libinput,
					 xf86libinput_log_handler);
		/* we want all msgs, let the server filter */
		libinput_log_set_priority(driver_context.libinput,
					  LIBINPUT_LOG_PRIORITY_DEBUG);
		xorg_list_init(&driver_context.server_fds);
	} else {
		libinput_ref(driver_context.libinput);
	}

	libinput = driver_context.libinput;

	if (libinput == NULL) {
		xf86IDrvMsg(pInfo, X_ERROR, "Creating a device for %s failed\n", path);
		goto fail;
	}

	if (use_server_fd(pInfo))
		fd_push(&driver_context, pInfo->fd, path);

	device = libinput_path_add_device(libinput, path);
	if (!device) {
		xf86IDrvMsg(pInfo, X_ERROR, "Failed to create a device for %s\n", path);
		goto fail;
	}

	/* We ref the device but remove it afterwards. The hope is that
	   between now and DEVICE_INIT/DEVICE_ON, the device doesn't change.
	  */
	libinput_device_ref(device);
	libinput_path_remove_device(device);
	if (use_server_fd(pInfo))
	    fd_pop(&driver_context, pInfo->fd);

	pInfo->private = driver_data;
	driver_data->path = path;
	driver_data->device = device;

	/* Disable acceleration in the server, libinput does it for us */
	pInfo->options = xf86ReplaceIntOption(pInfo->options, "AccelerationProfile", -1);
	pInfo->options = xf86ReplaceStrOption(pInfo->options, "AccelerationScheme", "none");

	xf86libinput_parse_options(pInfo, driver_data, device);

	/* now pick an actual type */
	if (libinput_device_config_tap_get_finger_count(device) > 0)
		pInfo->type_name = XI_TOUCHPAD;
	else if (libinput_device_has_capability(device,
						LIBINPUT_DEVICE_CAP_TOUCH))
		pInfo->type_name = XI_TOUCHSCREEN;
	else if (libinput_device_has_capability(device,
						LIBINPUT_DEVICE_CAP_POINTER))
		pInfo->type_name = XI_MOUSE;
	else
		pInfo->type_name = XI_KEYBOARD;

	return Success;
fail:
	if (use_server_fd(pInfo) && driver_context.libinput != NULL)
		fd_pop(&driver_context, pInfo->fd);
	if (driver_data->valuators)
		valuator_mask_free(&driver_data->valuators);
	if (driver_data->valuators_unaccelerated)
		valuator_mask_free(&driver_data->valuators_unaccelerated);
	free(path);
	free(driver_data);
	return BadValue;
}

static void
xf86libinput_uninit(InputDriverPtr drv,
		    InputInfoPtr pInfo,
		    int flags)
{
	struct xf86libinput *driver_data = pInfo->private;
	if (driver_data) {
		driver_context.libinput = libinput_unref(driver_context.libinput);
		valuator_mask_free(&driver_data->valuators);
		free(driver_data->path);
		free(driver_data);
		pInfo->private = NULL;
	}
	xf86DeleteInput(pInfo, flags);
}

InputDriverRec xf86libinput_driver = {
	.driverVersion	= 1,
	.driverName	= "libinput",
	.PreInit	= xf86libinput_pre_init,
	.UnInit		= xf86libinput_uninit,
	.module		= NULL,
	.default_options= NULL,
#ifdef XI86_DRV_CAP_SERVER_FD
	.capabilities	= XI86_DRV_CAP_SERVER_FD
#endif
};

static XF86ModuleVersionInfo xf86libinput_version_info = {
	"libinput",
	MODULEVENDORSTRING,
	MODINFOSTRING1,
	MODINFOSTRING2,
	XORG_VERSION_CURRENT,
	PACKAGE_VERSION_MAJOR, PACKAGE_VERSION_MINOR, PACKAGE_VERSION_PATCHLEVEL,
	ABI_CLASS_XINPUT,
	ABI_XINPUT_VERSION,
	MOD_CLASS_XINPUT,
	{0, 0, 0, 0}
};

static pointer
xf86libinput_setup_proc(pointer module, pointer options, int *errmaj, int *errmin)
{
	xf86AddInputDriver(&xf86libinput_driver, module, 0);
	return module;
}

_X_EXPORT XF86ModuleData libinputModuleData = {
	.vers		= &xf86libinput_version_info,
	.setup		= &xf86libinput_setup_proc,
	.teardown	= NULL
};

/* Property support */

/* libinput-specific properties */
static Atom prop_tap;
static Atom prop_tap_default;
static Atom prop_tap_drag_lock;
static Atom prop_tap_drag_lock_default;
static Atom prop_calibration;
static Atom prop_calibration_default;
static Atom prop_accel;
static Atom prop_accel_default;
static Atom prop_natural_scroll;
static Atom prop_natural_scroll_default;
static Atom prop_sendevents_available;
static Atom prop_sendevents_enabled;
static Atom prop_sendevents_default;
static Atom prop_left_handed;
static Atom prop_left_handed_default;
static Atom prop_scroll_methods_available;
static Atom prop_scroll_method_enabled;
static Atom prop_scroll_method_default;
static Atom prop_scroll_button;
static Atom prop_scroll_button_default;
static Atom prop_click_methods_available;
static Atom prop_click_method_enabled;
static Atom prop_click_method_default;
static Atom prop_middle_emulation;
static Atom prop_middle_emulation_default;
static Atom prop_halfkey;
static Atom prop_halfkey_default;

/* general properties */
static Atom prop_float;
static Atom prop_device;
static Atom prop_product_id;

static inline BOOL
xf86libinput_check_device (DeviceIntPtr dev,
			   Atom atom)
{
	InputInfoPtr pInfo = dev->public.devicePrivate;
	struct xf86libinput *driver_data = pInfo->private;
	struct libinput_device *device = driver_data->device;

	if (device == NULL) {
		BUG_WARN(dev->public.on);
		xf86IDrvMsg(pInfo, X_INFO,
			    "SetProperty on %u called but device is disabled.\n"
			    "This driver cannot change properties on a disabled device\n",
			    atom);
		return FALSE;
	}

	return TRUE;
}

static inline int
LibinputSetPropertyTap(DeviceIntPtr dev,
                       Atom atom,
                       XIPropertyValuePtr val,
                       BOOL checkonly)
{
	InputInfoPtr pInfo = dev->public.devicePrivate;
	struct xf86libinput *driver_data = pInfo->private;
	struct libinput_device *device = driver_data->device;
	BOOL* data;

	if (val->format != 8 || val->size != 1 || val->type != XA_INTEGER)
		return BadMatch;

	data = (BOOL*)val->data;
	if (checkonly) {
		if (*data != 0 && *data != 1)
			return BadValue;

		if (!xf86libinput_check_device (dev, atom))
			return BadMatch;

		if (libinput_device_config_tap_get_finger_count(device) == 0)
			return BadMatch;
	} else {
		driver_data->options.tapping = *data;
	}

	return Success;
}

static inline int
LibinputSetPropertyTapDragLock(DeviceIntPtr dev,
			       Atom atom,
			       XIPropertyValuePtr val,
			       BOOL checkonly)
{
	InputInfoPtr pInfo = dev->public.devicePrivate;
	struct xf86libinput *driver_data = pInfo->private;
	struct libinput_device *device = driver_data->device;
	BOOL* data;

	if (val->format != 8 || val->size != 1 || val->type != XA_INTEGER)
		return BadMatch;

	data = (BOOL*)val->data;
	if (checkonly) {
		if (*data != 0 && *data != 1)
			return BadValue;

		if (!xf86libinput_check_device(dev, atom))
			return BadMatch;

		if (libinput_device_config_tap_get_finger_count(device) == 0)
			return BadMatch;
	} else {
		driver_data->options.tap_drag_lock = *data;
	}

	return Success;
}

static inline int
LibinputSetPropertyCalibration(DeviceIntPtr dev,
                               Atom atom,
                               XIPropertyValuePtr val,
			       BOOL checkonly)
{
	InputInfoPtr pInfo = dev->public.devicePrivate;
	struct xf86libinput *driver_data = pInfo->private;
	struct libinput_device *device = driver_data->device;
	float* data;

	if (val->format != 32 || val->size != 9 || val->type != prop_float)
		return BadMatch;

	data = (float*)val->data;

	if (checkonly) {
		if (data[6] != 0 ||
		    data[7] != 0 ||
		    data[8] != 1)
			return BadValue;

		if (!xf86libinput_check_device (dev, atom))
			return BadMatch;

		if (!libinput_device_config_calibration_has_matrix(device))
			return BadMatch;
	} else {
		memcpy(driver_data->options.matrix,
		       data,
		       sizeof(driver_data->options.matrix));
	}

	return Success;
}

static inline int
LibinputSetPropertyAccel(DeviceIntPtr dev,
			 Atom atom,
			 XIPropertyValuePtr val,
			 BOOL checkonly)
{
	InputInfoPtr pInfo = dev->public.devicePrivate;
	struct xf86libinput *driver_data = pInfo->private;
	struct libinput_device *device = driver_data->device;
	float* data;

	if (val->format != 32 || val->size != 1 || val->type != prop_float)
		return BadMatch;

	data = (float*)val->data;

	if (checkonly) {
		if (*data < -1 || *data > 1)
			return BadValue;

		if (!xf86libinput_check_device (dev, atom))
			return BadMatch;

		if (libinput_device_config_accel_is_available(device) == 0)
			return BadMatch;
	} else {
		driver_data->options.speed = *data;
	}

	return Success;
}

static inline int
LibinputSetPropertyNaturalScroll(DeviceIntPtr dev,
                                 Atom atom,
                                 XIPropertyValuePtr val,
                                 BOOL checkonly)
{
	InputInfoPtr pInfo = dev->public.devicePrivate;
	struct xf86libinput *driver_data = pInfo->private;
	struct libinput_device *device = driver_data->device;
	BOOL* data;

	if (val->format != 8 || val->size != 1 || val->type != XA_INTEGER)
		return BadMatch;

	data = (BOOL*)val->data;

	if (checkonly) {
		if (*data != 0 && *data != 1)
			return BadValue;

		if (!xf86libinput_check_device (dev, atom))
			return BadMatch;

		if (libinput_device_config_scroll_has_natural_scroll(device) == 0)
			return BadMatch;
	} else {
		driver_data->options.natural_scrolling = *data;
	}

	return Success;
}

static inline int
LibinputSetPropertySendEvents(DeviceIntPtr dev,
			      Atom atom,
			      XIPropertyValuePtr val,
			      BOOL checkonly)
{
	InputInfoPtr pInfo = dev->public.devicePrivate;
	struct xf86libinput *driver_data = pInfo->private;
	struct libinput_device *device = driver_data->device;
	BOOL* data;
	uint32_t modes = 0;

	if (val->format != 8 || val->size != 2 || val->type != XA_INTEGER)
		return BadMatch;

	data = (BOOL*)val->data;

	if (data[0])
		modes |= LIBINPUT_CONFIG_SEND_EVENTS_DISABLED;
	if (data[1])
		modes |= LIBINPUT_CONFIG_SEND_EVENTS_DISABLED_ON_EXTERNAL_MOUSE;

	if (checkonly) {
		uint32_t supported;

		if (!xf86libinput_check_device (dev, atom))
			return BadMatch;

		supported = libinput_device_config_send_events_get_modes(device);
		if ((modes | supported) != supported)
			return BadValue;

	} else {
		driver_data->options.sendevents = modes;
	}

	return Success;
}

static inline int
LibinputSetPropertyLeftHanded(DeviceIntPtr dev,
			      Atom atom,
			      XIPropertyValuePtr val,
			      BOOL checkonly)
{
	InputInfoPtr pInfo = dev->public.devicePrivate;
	struct xf86libinput *driver_data = pInfo->private;
	struct libinput_device *device = driver_data->device;
	BOOL* data;

	if (val->format != 8 || val->size != 1 || val->type != XA_INTEGER)
		return BadMatch;

	data = (BOOL*)val->data;

	if (checkonly) {
		int supported;
		int left_handed = *data;

		if (!xf86libinput_check_device (dev, atom))
			return BadMatch;

		supported = libinput_device_config_left_handed_is_available(device);
		if (!supported && left_handed)
			return BadValue;
	} else {
		driver_data->options.left_handed = *data;
	}

	return Success;
}

static inline int
LibinputSetPropertyScrollMethods(DeviceIntPtr dev,
				 Atom atom,
				 XIPropertyValuePtr val,
				 BOOL checkonly)
{
	InputInfoPtr pInfo = dev->public.devicePrivate;
	struct xf86libinput *driver_data = pInfo->private;
	struct libinput_device *device = driver_data->device;
	BOOL* data;
	uint32_t modes = 0;

	if (val->format != 8 || val->size != 3 || val->type != XA_INTEGER)
		return BadMatch;

	data = (BOOL*)val->data;

	if (data[0])
		modes |= LIBINPUT_CONFIG_SCROLL_2FG;
	if (data[1])
		modes |= LIBINPUT_CONFIG_SCROLL_EDGE;
	if (data[2])
		modes |= LIBINPUT_CONFIG_SCROLL_ON_BUTTON_DOWN;

	if (checkonly) {
		uint32_t supported;

		if (__builtin_popcount(modes) > 1)
			return BadValue;

		if (!xf86libinput_check_device (dev, atom))
			return BadMatch;

		supported = libinput_device_config_scroll_get_methods(device);
		if (modes && (modes & supported) == 0)
			return BadValue;
	} else {
		driver_data->options.scroll_method = modes;
	}

	return Success;
}

static inline int
LibinputSetPropertyScrollButton(DeviceIntPtr dev,
				Atom atom,
				XIPropertyValuePtr val,
				BOOL checkonly)
{
	InputInfoPtr pInfo = dev->public.devicePrivate;
	struct xf86libinput *driver_data = pInfo->private;
	struct libinput_device *device = driver_data->device;
	CARD32* data;

	if (val->format != 32 || val->size != 1 || val->type != XA_CARDINAL)
		return BadMatch;

	data = (CARD32*)val->data;

	if (checkonly) {
		uint32_t button = *data;
		uint32_t supported;

		if (!xf86libinput_check_device (dev, atom))
			return BadMatch;

		supported = libinput_device_pointer_has_button(device,
							       btn_xorg2linux(button));
		if (button && !supported)
			return BadValue;
	} else {
		driver_data->options.scroll_button = *data;
	}

	return Success;
}

static inline int
LibinputSetPropertyClickMethod(DeviceIntPtr dev,
			       Atom atom,
			       XIPropertyValuePtr val,
			       BOOL checkonly)
{
	InputInfoPtr pInfo = dev->public.devicePrivate;
	struct xf86libinput *driver_data = pInfo->private;
	struct libinput_device *device = driver_data->device;
	BOOL* data;
	uint32_t modes = 0;

	if (val->format != 8 || val->size != 2 || val->type != XA_INTEGER)
		return BadMatch;

	data = (BOOL*)val->data;

	if (data[0])
		modes |= LIBINPUT_CONFIG_CLICK_METHOD_BUTTON_AREAS;
	if (data[1])
		modes |= LIBINPUT_CONFIG_CLICK_METHOD_CLICKFINGER;

	if (checkonly) {
		uint32_t supported;

		if (__builtin_popcount(modes) > 1)
			return BadValue;

		if (!xf86libinput_check_device(dev, atom))
			return BadMatch;

		supported = libinput_device_config_click_get_methods(device);
		if (modes && (modes & supported) == 0)
			return BadValue;
	} else {
		driver_data->options.click_method = modes;
	}

	return Success;
}

static inline int
LibinputSetPropertyMiddleEmulation(DeviceIntPtr dev,
				   Atom atom,
				   XIPropertyValuePtr val,
				   BOOL checkonly)
{
	InputInfoPtr pInfo = dev->public.devicePrivate;
	struct xf86libinput *driver_data = pInfo->private;
	struct libinput_device *device = driver_data->device;
	BOOL* data;

	if (val->format != 8 || val->size != 1 || val->type != XA_INTEGER)
		return BadMatch;

	data = (BOOL*)val->data;
	if (checkonly) {
		if (*data != 0 && *data != 1)
			return BadValue;

		if (!xf86libinput_check_device(dev, atom))
			return BadMatch;

		if (!libinput_device_config_middle_emulation_is_available(device))
			return BadMatch;
	} else {
		driver_data->options.middle_emulation = *data;
	}

	return Success;
}

static inline int
LibinputSetPropertyHalfkey(DeviceIntPtr dev,
			   Atom atom,
			   XIPropertyValuePtr val,
			   BOOL checkonly)
{
	InputInfoPtr pInfo = dev->public.devicePrivate;
	struct xf86libinput *driver_data = pInfo->private;
	struct libinput_device *device = driver_data->device;
	BOOL* data;

	if (val->format != 8 || val->size != 1 || val->type != XA_INTEGER)
		return BadMatch;

	data = (BOOL*)val->data;
	if (checkonly) {
		if (*data != 0 && *data != 1)
			return BadValue;

		if (!xf86libinput_check_device(dev, atom))
			return BadMatch;

		if (!libinput_device_config_halfkey_is_available(device))
			return BadMatch;
	} else {
		driver_data->options.halfkey = *data;
	}

	return Success;
}

static int
LibinputSetProperty(DeviceIntPtr dev, Atom atom, XIPropertyValuePtr val,
                 BOOL checkonly)
{
	int rc;

	if (atom == prop_tap)
		rc = LibinputSetPropertyTap(dev, atom, val, checkonly);
	else if (atom == prop_tap_drag_lock)
		rc = LibinputSetPropertyTapDragLock(dev, atom, val, checkonly);
	else if (atom == prop_calibration)
		rc = LibinputSetPropertyCalibration(dev, atom, val,
						    checkonly);
	else if (atom == prop_accel)
		rc = LibinputSetPropertyAccel(dev, atom, val, checkonly);
	else if (atom == prop_natural_scroll)
		rc = LibinputSetPropertyNaturalScroll(dev, atom, val, checkonly);
	else if (atom == prop_sendevents_available)
		return BadAccess; /* read-only */
	else if (atom == prop_sendevents_enabled)
		rc = LibinputSetPropertySendEvents(dev, atom, val, checkonly);
	else if (atom == prop_left_handed)
		rc = LibinputSetPropertyLeftHanded(dev, atom, val, checkonly);
	else if (atom == prop_scroll_methods_available)
		return BadAccess; /* read-only */
	else if (atom == prop_scroll_method_enabled)
		rc = LibinputSetPropertyScrollMethods(dev, atom, val, checkonly);
	else if (atom == prop_scroll_button)
		rc = LibinputSetPropertyScrollButton(dev, atom, val, checkonly);
	else if (atom == prop_click_methods_available)
		return BadAccess; /* read-only */
	else if (atom == prop_click_method_enabled)
		rc = LibinputSetPropertyClickMethod(dev, atom, val, checkonly);
	else if (atom == prop_middle_emulation)
		rc = LibinputSetPropertyMiddleEmulation(dev, atom, val, checkonly);
	else if (atom == prop_halfkey)
		rc = LibinputSetPropertyHalfkey(dev, atom, val, checkonly);
	else if (atom == prop_device || atom == prop_product_id ||
		 atom == prop_tap_default ||
		 atom == prop_tap_drag_lock_default ||
		 atom == prop_calibration_default ||
		 atom == prop_accel_default ||
		 atom == prop_natural_scroll_default ||
		 atom == prop_sendevents_default ||
		 atom == prop_left_handed_default ||
		 atom == prop_scroll_method_default ||
		 atom == prop_scroll_button_default ||
		 atom == prop_click_method_default ||
		 atom == prop_middle_emulation_default ||
		 atom == prop_halfkey_default)
		return BadAccess; /* read-only */
	else
		return Success;

	if (!checkonly && rc == Success)
		LibinputApplyConfig(dev);

	return rc;
}

static Atom
LibinputMakeProperty(DeviceIntPtr dev,
		     const char *prop_name,
		     Atom type,
		     int format,
		     int len,
		     void *data)
{
	int rc;
	Atom prop = MakeAtom(prop_name, strlen(prop_name), TRUE);

	rc = XIChangeDeviceProperty(dev, prop, type, format,
				    PropModeReplace,
				    len, data, FALSE);
	if (rc != Success)
		return None;

	XISetDevicePropertyDeletable(dev, prop, FALSE);

	return prop;
}

static void
LibinputInitTapProperty(DeviceIntPtr dev,
			struct xf86libinput *driver_data,
			struct libinput_device *device)
{
	BOOL tap = driver_data->options.tapping;

	if (libinput_device_config_tap_get_finger_count(device) == 0)
		return;

	prop_tap = LibinputMakeProperty(dev,
					LIBINPUT_PROP_TAP,
					XA_INTEGER,
					8,
					1,
					&tap);
	if (!prop_tap)
		return;

	tap = libinput_device_config_tap_get_default_enabled(device);
	prop_tap_default = LibinputMakeProperty(dev,
						LIBINPUT_PROP_TAP_DEFAULT,
						XA_INTEGER, 8,
						1, &tap);
}

static void
LibinputInitTapDragLockProperty(DeviceIntPtr dev,
				struct xf86libinput *driver_data,
				struct libinput_device *device)
{
	BOOL drag_lock = driver_data->options.tap_drag_lock;

	if (libinput_device_config_tap_get_finger_count(device) == 0)
		return;

	prop_tap_drag_lock = LibinputMakeProperty(dev,
						  LIBINPUT_PROP_TAP_DRAG_LOCK,
						  XA_INTEGER, 8,
						  1, &drag_lock);
	if (!prop_tap_drag_lock)
		return;

	drag_lock = libinput_device_config_tap_get_default_enabled(device);
	prop_tap_drag_lock_default = LibinputMakeProperty(dev,
							  LIBINPUT_PROP_TAP_DRAG_LOCK_DEFAULT,
							  XA_INTEGER, 8,
							  1, &drag_lock);
}

static void
LibinputInitCalibrationProperty(DeviceIntPtr dev,
				struct xf86libinput *driver_data,
				struct libinput_device *device)
{
	float calibration[9];

	if (!libinput_device_config_calibration_has_matrix(device))
		return;

	/* We use a 9-element matrix just to be closer to the X server's
	   transformation matrix which also has the full matrix */

	libinput_device_config_calibration_get_matrix(device, calibration);
	calibration[6] = 0;
	calibration[7] = 0;
	calibration[8] = 1;

	prop_calibration = LibinputMakeProperty(dev,
						LIBINPUT_PROP_CALIBRATION,
						prop_float, 32,
						9, calibration);
	if (!prop_calibration)
		return;

	libinput_device_config_calibration_get_default_matrix(device,
							      calibration);

	prop_calibration_default = LibinputMakeProperty(dev,
							LIBINPUT_PROP_CALIBRATION_DEFAULT,
							prop_float, 32,
							9, calibration);
}

static void
LibinputInitAccelProperty(DeviceIntPtr dev,
			  struct xf86libinput *driver_data,
			  struct libinput_device *device)
{
	float speed = driver_data->options.speed;

	if (!libinput_device_config_accel_is_available(device))
		return;

	prop_accel = LibinputMakeProperty(dev,
					  LIBINPUT_PROP_ACCEL,
					  prop_float, 32,
					  1, &speed);
	if (!prop_accel)
		return;

	speed = libinput_device_config_accel_get_default_speed(device);
	prop_accel_default = LibinputMakeProperty(dev,
						  LIBINPUT_PROP_ACCEL_DEFAULT,
						  prop_float, 32,
						  1, &speed);
}

static void
LibinputInitNaturalScrollProperty(DeviceIntPtr dev,
				  struct xf86libinput *driver_data,
				  struct libinput_device *device)
{
	BOOL natural_scroll = driver_data->options.natural_scrolling;

	if (!libinput_device_config_scroll_has_natural_scroll(device))
		return;

	prop_natural_scroll = LibinputMakeProperty(dev,
						   LIBINPUT_PROP_NATURAL_SCROLL,
						   XA_INTEGER, 8,
						   1, &natural_scroll);
	if (!prop_natural_scroll)
		return;

	natural_scroll = libinput_device_config_scroll_get_default_natural_scroll_enabled(device);
	prop_natural_scroll_default = LibinputMakeProperty(dev,
							   LIBINPUT_PROP_NATURAL_SCROLL_DEFAULT,
							   XA_INTEGER, 8,
							   1, &natural_scroll);
}

static void
LibinputInitSendEventsProperty(DeviceIntPtr dev,
			       struct xf86libinput *driver_data,
			       struct libinput_device *device)
{
	uint32_t sendevent_modes;
	uint32_t sendevents;
	BOOL modes[2] = {FALSE};

	sendevent_modes = libinput_device_config_send_events_get_modes(device);
	if (sendevent_modes == LIBINPUT_CONFIG_SEND_EVENTS_ENABLED)
		return;

	if (sendevent_modes & LIBINPUT_CONFIG_SEND_EVENTS_DISABLED)
		modes[0] = TRUE;
	if (sendevent_modes & LIBINPUT_CONFIG_SEND_EVENTS_DISABLED_ON_EXTERNAL_MOUSE)
		modes[1] = TRUE;

	prop_sendevents_available = LibinputMakeProperty(dev,
							 LIBINPUT_PROP_SENDEVENTS_AVAILABLE,
							 XA_INTEGER, 8,
							 2, modes);
	if (!prop_sendevents_available)
		return;

	memset(modes, 0, sizeof(modes));
	sendevents = driver_data->options.sendevents;

	switch(sendevents) {
	case LIBINPUT_CONFIG_SEND_EVENTS_DISABLED:
		modes[0] = TRUE;
		break;
	case LIBINPUT_CONFIG_SEND_EVENTS_DISABLED_ON_EXTERNAL_MOUSE:
		modes[1] = TRUE;
		break;
	}

	prop_sendevents_enabled = LibinputMakeProperty(dev,
						       LIBINPUT_PROP_SENDEVENTS_ENABLED,
						       XA_INTEGER, 8,
						       2, modes);

	if (!prop_sendevents_enabled)
		return;

	memset(modes, 0, sizeof(modes));
	sendevent_modes = libinput_device_config_send_events_get_default_mode(device);
	if (sendevent_modes & LIBINPUT_CONFIG_SEND_EVENTS_DISABLED)
		modes[0] = TRUE;
	if (sendevent_modes & LIBINPUT_CONFIG_SEND_EVENTS_DISABLED_ON_EXTERNAL_MOUSE)
		modes[1] = TRUE;

	prop_sendevents_default = LibinputMakeProperty(dev,
						       LIBINPUT_PROP_SENDEVENTS_ENABLED_DEFAULT,
						       XA_INTEGER, 8,
						       2, modes);
}

static void
LibinputInitLeftHandedProperty(DeviceIntPtr dev,
			       struct xf86libinput *driver_data,
			       struct libinput_device *device)
{
	BOOL left_handed = driver_data->options.left_handed;

	if (!libinput_device_config_left_handed_is_available(device))
		return;

	prop_left_handed = LibinputMakeProperty(dev,
						LIBINPUT_PROP_LEFT_HANDED,
						XA_INTEGER, 8,
						1, &left_handed);
	if (!prop_left_handed)
		return;

	left_handed = libinput_device_config_left_handed_get_default(device);
	prop_left_handed_default = LibinputMakeProperty(dev,
							LIBINPUT_PROP_LEFT_HANDED_DEFAULT,
							XA_INTEGER, 8,
							1, &left_handed);
}

static void
LibinputInitScrollMethodsProperty(DeviceIntPtr dev,
				  struct xf86libinput *driver_data,
				  struct libinput_device *device)
{
	uint32_t scroll_methods;
	enum libinput_config_scroll_method method;
	BOOL methods[3] = {FALSE};

	scroll_methods = libinput_device_config_scroll_get_methods(device);
	if (scroll_methods == LIBINPUT_CONFIG_SCROLL_NO_SCROLL)
		return;

	if (scroll_methods & LIBINPUT_CONFIG_SCROLL_2FG)
		methods[0] = TRUE;
	if (scroll_methods & LIBINPUT_CONFIG_SCROLL_EDGE)
		methods[1] = TRUE;
	if (scroll_methods & LIBINPUT_CONFIG_SCROLL_ON_BUTTON_DOWN)
		methods[2] = TRUE;

	prop_scroll_methods_available = LibinputMakeProperty(dev,
							     LIBINPUT_PROP_SCROLL_METHODS_AVAILABLE,
							     XA_INTEGER, 8,
							     ARRAY_SIZE(methods),
							     &methods);
	if (!prop_scroll_methods_available)
		return;

	memset(methods, 0, sizeof(methods));

	method = libinput_device_config_scroll_get_method(device);
	switch(method) {
	case LIBINPUT_CONFIG_SCROLL_2FG:
		methods[0] = TRUE;
		break;
	case LIBINPUT_CONFIG_SCROLL_EDGE:
		methods[1] = TRUE;
		break;
	case LIBINPUT_CONFIG_SCROLL_ON_BUTTON_DOWN:
		methods[2] = TRUE;
		break;
	default:
		break;
	}

	prop_scroll_method_enabled = LibinputMakeProperty(dev,
							  LIBINPUT_PROP_SCROLL_METHOD_ENABLED,
							  XA_INTEGER, 8,
							  ARRAY_SIZE(methods),
							  &methods);
	if (!prop_scroll_method_enabled)
		return;

	scroll_methods = libinput_device_config_scroll_get_default_method(device);
	if (scroll_methods & LIBINPUT_CONFIG_SCROLL_2FG)
		methods[0] = TRUE;
	if (scroll_methods & LIBINPUT_CONFIG_SCROLL_EDGE)
		methods[1] = TRUE;
	if (scroll_methods & LIBINPUT_CONFIG_SCROLL_ON_BUTTON_DOWN)
		methods[2] = TRUE;

	prop_scroll_method_default = LibinputMakeProperty(dev,
							  LIBINPUT_PROP_SCROLL_METHOD_ENABLED_DEFAULT,
							  XA_INTEGER, 8,
							  ARRAY_SIZE(methods),
							  &methods);
	/* Scroll button */
	if (libinput_device_config_scroll_get_methods(device) &
	    LIBINPUT_CONFIG_SCROLL_ON_BUTTON_DOWN) {
		CARD32 scroll_button = driver_data->options.scroll_button;

		prop_scroll_button = LibinputMakeProperty(dev,
							  LIBINPUT_PROP_SCROLL_BUTTON,
							  XA_CARDINAL, 32,
							  1, &scroll_button);
		if (!prop_scroll_button)
			return;

		scroll_button = libinput_device_config_scroll_get_default_button(device);
		prop_scroll_button_default = LibinputMakeProperty(dev,
								  LIBINPUT_PROP_SCROLL_BUTTON_DEFAULT,
								  XA_CARDINAL, 32,
								  1, &scroll_button);
	}
}

static void
LibinputInitClickMethodsProperty(DeviceIntPtr dev,
				 struct xf86libinput *driver_data,
				 struct libinput_device *device)
{
	uint32_t click_methods;
	enum libinput_config_click_method method;
	BOOL methods[2] = {FALSE};

	click_methods = libinput_device_config_click_get_methods(device);
	if (click_methods == LIBINPUT_CONFIG_CLICK_METHOD_NONE)
		return;

	if (click_methods & LIBINPUT_CONFIG_CLICK_METHOD_BUTTON_AREAS)
		methods[0] = TRUE;
	if (click_methods & LIBINPUT_CONFIG_CLICK_METHOD_CLICKFINGER)
		methods[1] = TRUE;

	prop_click_methods_available = LibinputMakeProperty(dev,
							    LIBINPUT_PROP_CLICK_METHODS_AVAILABLE,
							    XA_INTEGER, 8,
							    ARRAY_SIZE(methods),
							    &methods);
	if (!prop_click_methods_available)
		return;

	memset(methods, 0, sizeof(methods));

	method = libinput_device_config_click_get_method(device);
	switch(method) {
	case LIBINPUT_CONFIG_CLICK_METHOD_BUTTON_AREAS:
		methods[0] = TRUE;
		break;
	case LIBINPUT_CONFIG_CLICK_METHOD_CLICKFINGER:
		methods[1] = TRUE;
		break;
	default:
		break;
	}

	prop_click_method_enabled = LibinputMakeProperty(dev,
							 LIBINPUT_PROP_CLICK_METHOD_ENABLED,
							 XA_INTEGER, 8,
							 ARRAY_SIZE(methods),
							 &methods);

	if (!prop_click_method_enabled)
		return;

	memset(methods, 0, sizeof(methods));

	method = libinput_device_config_click_get_default_method(device);
	switch(method) {
	case LIBINPUT_CONFIG_CLICK_METHOD_BUTTON_AREAS:
		methods[0] = TRUE;
		break;
	case LIBINPUT_CONFIG_CLICK_METHOD_CLICKFINGER:
		methods[1] = TRUE;
		break;
	default:
		break;
	}

	prop_click_method_default = LibinputMakeProperty(dev,
							 LIBINPUT_PROP_CLICK_METHOD_ENABLED_DEFAULT,
							 XA_INTEGER, 8,
							 ARRAY_SIZE(methods),
							 &methods);
}

static void
LibinputInitMiddleEmulationProperty(DeviceIntPtr dev,
				    struct xf86libinput *driver_data,
				    struct libinput_device *device)
{
	BOOL middle = driver_data->options.middle_emulation;

	if (!libinput_device_config_middle_emulation_is_available(device))
		return;

	prop_middle_emulation = LibinputMakeProperty(dev,
						     LIBINPUT_PROP_MIDDLE_EMULATION_ENABLED,
						     XA_INTEGER,
						     8,
						     1,
						     &middle);
	if (!prop_middle_emulation)
		return;

	middle = libinput_device_config_middle_emulation_get_default_enabled(device);
	prop_middle_emulation_default = LibinputMakeProperty(dev,
							     LIBINPUT_PROP_MIDDLE_EMULATION_ENABLED_DEFAULT,
							     XA_INTEGER, 8,
							     1, &middle);
}

static void
LibinputInitHalfkeyProperty(DeviceIntPtr dev,
			    struct xf86libinput *driver_data,
			    struct libinput_device *device)
{
	BOOL halfkey = driver_data->options.halfkey;

	if (!libinput_device_config_halfkey_is_available(device))
		return;

	prop_halfkey = LibinputMakeProperty(dev,
					    LIBINPUT_PROP_HALFKEY_ENABLED,
					    XA_INTEGER,
					    8,
					    1,
					    &halfkey);
	if (!prop_halfkey)
		return;

	halfkey = libinput_device_config_halfkey_get_default_enabled(device);
	prop_halfkey_default = LibinputMakeProperty(dev,
						    LIBINPUT_PROP_HALFKEY_ENABLED_DEFAULT,
						    XA_INTEGER, 8,
						    1,
						    &halfkey);
}

static void
LibinputInitProperty(DeviceIntPtr dev)
{
	InputInfoPtr pInfo  = dev->public.devicePrivate;
	struct xf86libinput *driver_data = pInfo->private;
	struct libinput_device *device = driver_data->device;
	const char *device_node;
	CARD32 product[2];
	int rc;

	prop_float = XIGetKnownProperty("FLOAT");

	LibinputInitTapProperty(dev, driver_data, device);
	LibinputInitTapDragLockProperty(dev, driver_data, device);
	LibinputInitCalibrationProperty(dev, driver_data, device);
	LibinputInitAccelProperty(dev, driver_data, device);
	LibinputInitNaturalScrollProperty(dev, driver_data, device);
	LibinputInitSendEventsProperty(dev, driver_data, device);
	LibinputInitLeftHandedProperty(dev, driver_data, device);
	LibinputInitScrollMethodsProperty(dev, driver_data, device);
	LibinputInitClickMethodsProperty(dev, driver_data, device);
	LibinputInitMiddleEmulationProperty(dev, driver_data, device);
	LibinputInitHalfkeyProperty(dev, driver_data, device);

	/* Device node property, read-only  */
	device_node = driver_data->path;
	prop_device = MakeAtom(XI_PROP_DEVICE_NODE,
			       strlen(XI_PROP_DEVICE_NODE),
			       TRUE);
	rc = XIChangeDeviceProperty(dev, prop_device, XA_STRING, 8,
				    PropModeReplace,
				    strlen(device_node), device_node,
				    FALSE);
	if (rc != Success)
		return;

	XISetDevicePropertyDeletable(dev, prop_device, FALSE);

	prop_product_id = MakeAtom(XI_PROP_PRODUCT_ID,
				   strlen(XI_PROP_PRODUCT_ID),
				   TRUE);
	product[0] = libinput_device_get_id_vendor(device);
	product[1] = libinput_device_get_id_product(device);
	rc = XIChangeDeviceProperty(dev, prop_product_id, XA_INTEGER, 32,
				    PropModeReplace, 2, product, FALSE);
	if (rc != Success)
		return;

	XISetDevicePropertyDeletable(dev, prop_product_id, FALSE);
}
