// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * hid-veikk.c -- Linux driver for VEIKK digitizers, 3.0.0a1
 *
 * This is intended to make Linux devices work with VEIKK digitizers
 * (drawing tablets). There are two modes, which can be toggled by a
 * sysfs parameter:
 *
 * - (Default): Requires accompanying Python veikk-config daemon. Allows
 *   pen and button configuration. https://github.com/jlam55555/veikk-config.
 * - Pen works OOTB, no button/gestures support. Similar to v2 driver
 *   functionality.
 *
 * Note that the v3 driver is incompatible with the v2 driver and its
 * configuration tool.
 *
 * Copyright (C) 2021  Jonathan Lam <jlam55555@gmail.com>
 */

#include <linux/hid.h>
#include <linux/module.h>
#include <linux/workqueue.h>
#include <asm/unaligned.h>

// comment the following line to disable debugging output in kernel logs
#define VEIKK_DEBUG_MODE

#define VEIKK_VENDOR_ID		0x2FEB
#define VEIKK_DRIVER_VERSION	"3.0.0a1"
#define VEIKK_DRIVER_DESC	"VEIKK digitizer driver"
#define VEIKK_DRIVER_AUTHOR	"Jonathan Lam <jlam55555@gmail.com>"
#define VEIKK_DRIVER_LICENSE	"GPL"

#define VEIKK_PEN_REPORT	0x41
#define VEIKK_BUTTON_REPORT	0x42
#define VEIKK_PAD_REPORT	0x43

struct veikk_report {
	u8 id;		// proprietary report id always is 9
	u8 type;	// see above macros for report types
	u8 data[7];	// data depends on the type
};

struct veikk_pen_report_data {
	u8 btns;
	u8 x[2], y[2], pressure[2];
};
struct veikk_buttons_report_data {
	u8 type;	// distinguishes between buttons and wheel
	u8 pressed;
	u8 btns[2];	// last 5 bytes are a bitmap of keys pressed; no tablet
	u8 unused2[3];	// has more than 12 buttons (only need 2 bytes)
};
struct veikk_pad_report_data {
	u8 pressed;
	u8 btns;	// last 6 bytes are a bitmap of keys pressed; no
	u8 unused[5];	// gesture pad has more than 8 keys (only need 1 byte)
};

// veikk model characteristics
struct veikk_model {
	const char *name;
	const int prod_id, x_max, y_max, pressure_max, has_buttons, has_pad;
};

// veikk device descriptor
struct veikk_device {
	const struct veikk_model *model;
	struct input_dev *input_dev;
	struct delayed_work setup_pen_work, setup_buttons_work, setup_pad_work;
	struct hid_device *hid_dev;

	// bitmaps holding the latest state of buttons; this is necessary
	// for keeping the modifier keys down until all buttons are released
	// (which is needed for proper softrepeats)
	u16 buttons_state;
	u8 pad_state, wheel_state;
};

// veikk_event and veikk_report are only used for debugging
#ifdef VEIKK_DEBUG_MODE
static int veikk_event(struct hid_device *hdev, struct hid_field *field,
	struct hid_usage *usage, __s32 value)
{
	hid_info(hdev, "in veikk_event: usage %x value %d", usage->hid, value);
	return 0;
}
void veikk_report(struct hid_device *hid_dev, struct hid_report *report)
{
	hid_info(hid_dev, "in veikk_report: report id %d", report->id);
}
#endif	// VEIKK_DEBUG_MODE

/*
 * We only use the proprietary veikk interface (usage 0xFF0A), since this emits
 * nicer events than the other interfaces and we don't have to worry about
 * the inconsistent grouping of events under different interfaces. In earlier
 * versions of the driver (v1.0-2.0), we used the generic interfaces, but this
 * caused many issues with button mapping and inconsistencies between
 * interfaces. The Windows/Mac drivers also use the proprietary interface only.
 */
static int veikk_is_proprietary(struct hid_device *hid_dev)
{
	// dev_rdesc and dev_rsize are the device report descriptor and its
	// length, respectively
	u8 *rdesc = hid_dev->dev_rdesc;
	unsigned int rsize = hid_dev->dev_rsize;

	return rsize >= 3
		&& rdesc[0] == 0x06 && rdesc[1] == 0x0A && rdesc[2] == 0xFF;
}

/*
 * This sends the magic bytes to the VEIKK tablets that enable the proprietary
 * interfaces for the pen, buttons, and gesture pad. It seems that sending
 * them in two short of a time interval doesn't enable them all, so these
 * are sent out using delayed workqueue tasks with different delays.
 *
 * Device types: 0 = pen, 1 = buttons, 2 = gesture pad
 *
 * TODO: return code of this is never used
 * TODO: make sure device exists, since this happens asynchronously. (E.g., if
 * 	user quickly plugs/unplugs device)
 */
static const u8 pen_output_report[9] =
	{ 0x09, 0x01, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
static const u8 buttons_output_report[9] =
	{ 0x09, 0x02, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
static const u8 pad_output_report[9] =
	{ 0x09, 0x03, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
static int veikk_setup_feature(struct work_struct *work, int device_type) {
	struct delayed_work *dwork;
	struct veikk_device *veikk_dev;
	struct hid_device *hid_dev;
	u8 *buf;
	const u8 *output_report;
	int buf_len = sizeof(pen_output_report);

	dwork = container_of(work, struct delayed_work, work);

	switch (device_type) {
	case 0:
		veikk_dev = container_of(dwork, struct veikk_device,
			setup_pen_work);
		output_report = pen_output_report;
		break;
	case 1:
		veikk_dev = container_of(dwork, struct veikk_device,
			setup_buttons_work);
		output_report = buttons_output_report;
		break;
	case 2:
		veikk_dev = container_of(dwork, struct veikk_device,
			setup_pad_work);
		output_report = pad_output_report;
		break;
	default:
		return -EINVAL;
	}

	hid_dev = veikk_dev->hid_dev;
	if (!(buf = devm_kzalloc(&hid_dev->dev, buf_len, GFP_KERNEL))) {
		return -ENOMEM;
	}

	memcpy(buf, output_report, buf_len);
	hid_dev->ll_driver->output_report(hid_dev, buf, buf_len);
	return 0;
}
static void veikk_setup_pen(struct work_struct *work)
{
	veikk_setup_feature(work, 0);
}
static void veikk_setup_buttons(struct work_struct *work)
{
	veikk_setup_feature(work, 1);
}
static void veikk_setup_pad(struct work_struct *work)
{
	veikk_setup_feature(work, 2);
}

static int veikk_pen_event(struct veikk_pen_report_data *evt,
	struct hid_device *hid_dev)
{
	struct veikk_device *veikk_dev = hid_get_drvdata(hid_dev);
	struct input_dev *input = veikk_dev->input_dev;

	input_report_abs(input, ABS_X, get_unaligned_le16(evt->x));
	input_report_abs(input, ABS_Y, get_unaligned_le16(evt->y));
	input_report_abs(input, ABS_PRESSURE,
		get_unaligned_le16(evt->pressure));

	input_report_key(input, BTN_TOUCH, evt->btns & 0x01);
	input_report_key(input, BTN_STYLUS, evt->btns & 0x02);
	input_report_key(input, BTN_STYLUS2, evt->btns & 0x04);
	return 0;
}

// TODO: fix this -- does this require state?
static int veikk_buttons_event(struct veikk_buttons_report_data *evt,
	struct hid_device *hid_dev)
{
	struct veikk_device *veikk_dev = hid_get_drvdata(hid_dev);
	struct input_dev *input = veikk_dev->input_dev;
	u16 event_buttons = get_unaligned_le16(evt->btns), buttons_state;
	u8 wheel_state;

	// first byte: 1 = button, 3 = wheel left/right
	if (evt->type == 1) {
		buttons_state = evt->pressed
			? (veikk_dev->buttons_state |= event_buttons)
			: (veikk_dev->buttons_state &= ~event_buttons);
		wheel_state = veikk_dev->wheel_state;
	} else {
		buttons_state = veikk_dev->buttons_state;
		wheel_state = evt->pressed
			? (veikk_dev->wheel_state |= event_buttons)
			: (veikk_dev->wheel_state &= ~event_buttons);
	}

	input_report_key(input, BTN_0, buttons_state & 0x001);
	input_report_key(input, BTN_1, buttons_state & 0x002);
	input_report_key(input, BTN_2, buttons_state & 0x004);
	input_report_key(input, BTN_3, buttons_state & 0x008);
	input_report_key(input, BTN_4, buttons_state & 0x010);
	input_report_key(input, BTN_5, buttons_state & 0x020);
	input_report_key(input, BTN_6, buttons_state & 0x040);
	input_report_key(input, BTN_7, buttons_state & 0x080);
	input_report_key(input, BTN_8, buttons_state & 0x100);
	input_report_key(input, BTN_9, buttons_state & 0x200);

	// after BTN_9 are other buttons with assigned meanings, so we choose
	// 0x118 and 0x119, which are unassigned in input-event-codes
	input_report_key(input, 0x118, buttons_state & 0x400);
	input_report_key(input, 0x119, buttons_state & 0x800);

	// wheel and gear buttons
	input_report_key(input, BTN_WHEEL, buttons_state & 0x1000);
	input_report_key(input, BTN_GEAR_DOWN, wheel_state & 0x1);
	input_report_key(input, BTN_GEAR_UP, wheel_state & 0x2);
	return 0;
}

static int veikk_pad_event(struct veikk_pad_report_data *evt,
			   struct hid_device *hid_dev)
{
	struct veikk_device *veikk_dev = hid_get_drvdata(hid_dev);
	struct input_dev *input = veikk_dev->input_dev;
	u8 state = evt->pressed
		? (veikk_dev->pad_state |= evt->btns)
		: (veikk_dev->pad_state &= ~evt->btns);

	// gesture pad swipe up, down, left, right (respectively)
	input_report_key(input, BTN_NORTH, state & 0x01);
	input_report_key(input, BTN_SOUTH, state & 0x02);
	input_report_key(input, BTN_WEST, state & 0x04);
	input_report_key(input, BTN_EAST, state & 0x08);

	// double-tap
	input_report_key(input, BTN_TOOL_DOUBLETAP, state & 0x10);
	return 0;
}

static int veikk_raw_event(struct hid_device *hid_dev,
	struct hid_report *report, u8 *data, int size)
{
	struct veikk_device *veikk_dev = hid_get_drvdata(hid_dev);
	struct input_dev *input = veikk_dev->input_dev;
	struct veikk_report *veikk_report;

#ifdef VEIKK_DEBUG_MODE
	hid_info(hid_dev, "raw report size: %d", size);
#endif	// VEIKK_DEBUG_MODE

	// only report ID for proprietary device has ID 9
	if (report->id != 9 || size != sizeof(struct veikk_report))
		return -EINVAL;

	veikk_report = (struct veikk_report *) data;

	switch (veikk_report->type) {
	case VEIKK_PEN_REPORT:
		if (veikk_pen_event((struct veikk_pen_report_data *)
				veikk_report->data, hid_dev))
			return -EINVAL;
		break;
	case VEIKK_BUTTON_REPORT:
		if (veikk_buttons_event((struct veikk_buttons_report_data *)
				veikk_report->data, hid_dev))
			return -EINVAL;
		break;
	case VEIKK_PAD_REPORT:
		if (veikk_pad_event((struct veikk_pad_report_data *)
				veikk_report->data, hid_dev))
			return -EINVAL;
		break;
	default:
		hid_info(hid_dev, "unknown report with id %d", report->id);
		return 0;
	}
	input_sync(input);
	return 1;
}

// called by struct input_dev instances, not called directly
static int veikk_input_open(struct input_dev *dev)
{
	return hid_hw_open((struct hid_device *) input_get_drvdata(dev));
}
static void veikk_input_close(struct input_dev *dev)
{
	hid_hw_close((struct hid_device *) input_get_drvdata(dev));
}

// used by second mode with pen passthrough only
static int veikk_setup_pen_input(struct input_dev *input,
	struct hid_device *hid_dev)
{
	struct veikk_device *veikk_dev = hid_get_drvdata(hid_dev);
	const struct veikk_model *model = veikk_dev->model;
	char *input_name;

	// input name = model name + " Pen"
	if (!(input_name = devm_kzalloc(&input->dev, strlen(model->name)+5,
			GFP_KERNEL)))
		return -ENOMEM;
	sprintf(input_name, "%s Pen", model->name);
	input->name = input_name;

	__set_bit(INPUT_PROP_POINTER, input->propbit);

	__set_bit(EV_KEY, input->evbit);
	__set_bit(EV_ABS, input->evbit);

	__set_bit(BTN_TOUCH, input->keybit);
	__set_bit(BTN_STYLUS, input->keybit);
	__set_bit(BTN_STYLUS2, input->keybit);

	// TODO: not sure what resolution, fuzz values should be;
	// 	these ones seem to work fine for now
	input_set_abs_params(input, ABS_X, 0, model->x_max, 0, 0);
	input_set_abs_params(input, ABS_Y, 0, model->y_max, 0, 0);
	input_set_abs_params(input, ABS_PRESSURE, 0, model->pressure_max, 0, 0);
	input_abs_set_res(input, ABS_X, 100);
	input_abs_set_res(input, ABS_Y, 100);
	return 0;
}

// used by default mode with all events in a single input_dev
static int veikk_setup_bundled_inputs(struct input_dev *input,
	struct hid_device *hid_dev)
{
	struct veikk_device *veikk_dev = hid_get_drvdata(hid_dev);
	const struct veikk_model *model = veikk_dev->model;
	char *input_name;

	// input name = model name + " Bundled"
	if (!(input_name = devm_kzalloc(&input->dev, strlen(model->name)+9,
					GFP_KERNEL)))
		return -ENOMEM;
	sprintf(input_name, "%s Bundled", model->name);
	input->name = input_name;

	__set_bit(EV_KEY, input->evbit);
	__set_bit(EV_ABS, input->evbit);
	__set_bit(EV_REP, input->evbit);
	__set_bit(MSC_SCAN, input->mscbit);

	// stylus capabilities
	__set_bit(BTN_TOUCH, input->keybit);
	__set_bit(BTN_STYLUS, input->keybit);
	__set_bit(BTN_STYLUS2, input->keybit);

	input_set_abs_params(input, ABS_X, 0, model->x_max, 0, 0);
	input_set_abs_params(input, ABS_Y, 0, model->y_max, 0, 0);
	input_set_abs_params(input, ABS_PRESSURE, 0, model->pressure_max, 0, 0);
	input_abs_set_res(input, ABS_X, 100);
	input_abs_set_res(input, ABS_Y, 100);

	// buttonpad capabilities
	// buttons 1-12
	__set_bit(BTN_0, input->keybit);
	__set_bit(BTN_1, input->keybit);
	__set_bit(BTN_2, input->keybit);
	__set_bit(BTN_3, input->keybit);
	__set_bit(BTN_4, input->keybit);
	__set_bit(BTN_5, input->keybit);
	__set_bit(BTN_6, input->keybit);
	__set_bit(BTN_7, input->keybit);
	__set_bit(BTN_8, input->keybit);
	__set_bit(BTN_9, input->keybit);
	__set_bit(0x118, input->keybit);
	__set_bit(0x119, input->keybit);

	// wheel center, left, right
	__set_bit(BTN_WHEEL, input->keybit);
	__set_bit(BTN_GEAR_DOWN, input->keybit);
	__set_bit(BTN_GEAR_UP, input->keybit);

	input_enable_softrepeat(input, 100, 33);

	// gesture pad capabilities
	__set_bit(BTN_TOOL_DOUBLETAP, input->keybit);
	__set_bit(BTN_NORTH, input->keybit);
	__set_bit(BTN_SOUTH, input->keybit);
	__set_bit(BTN_WEST, input->keybit);
	__set_bit(BTN_EAST, input->keybit);
	return 0;
}

static int veikk_register_input(struct input_dev *input,
	struct hid_device *hid_dev)
{
	input->open = veikk_input_open;
	input->close = veikk_input_close;
	input->phys = hid_dev->phys;
	input->uniq = hid_dev->uniq;
	input->id.bustype = hid_dev->bus;
	input->id.vendor = hid_dev->vendor;
	input->id.product = hid_dev->product;
	input->id.version = hid_dev->version;

	input_set_drvdata(input, hid_dev);
	return input_register_device(input);
}

static int veikk_allocate_setup_register_inputs(struct hid_device *hid_dev)
{
	struct veikk_device *veikk_dev = hid_get_drvdata(hid_dev);
	int err;

	// allocate struct input_devs
	// TODO: fill this out
	//devres_open_group();
	if (!(veikk_dev->input_dev
			= devm_input_allocate_device(&hid_dev->dev))) {
		err = -ENOMEM;
		goto bad_alloc;
	}
	// TODO: fill this out
	//devres_close_group();

	// setup struct input_devs
	// bundled (default) mode
	if ((err = veikk_setup_bundled_inputs(veikk_dev->input_dev, hid_dev)))
		goto bad_setup;

	// pen-only mode
	// TODO: properly switch on this
//	if ((err = veikk_setup_pen_input(veikk_dev->pen_input, hid_dev))
//		goto bad_setup;

	// register struct input_devs
	// TODO: refactor like the above
	if ((err = veikk_register_input(veikk_dev->input_dev, hid_dev)))
		goto bad_register;

	// setup workqueues to initialize proprietary devices correctly
	// TODO: make the delays customizable (e.g., put in a macro)
	INIT_DELAYED_WORK(&veikk_dev->setup_pen_work, veikk_setup_pen);
	schedule_delayed_work(&veikk_dev->setup_pen_work, 100);
	if (veikk_dev->model->has_buttons) {
		INIT_DELAYED_WORK(&veikk_dev->setup_buttons_work,
				  veikk_setup_buttons);
		schedule_delayed_work(&veikk_dev->setup_buttons_work, 200);
	}
	if (veikk_dev->model->has_pad) {
		INIT_DELAYED_WORK(&veikk_dev->setup_pad_work,
				  veikk_setup_pad);
		schedule_delayed_work(&veikk_dev->setup_pad_work, 300);
	}
	return 0;

bad_register:
	// input_unregister_device()
bad_setup:
	// no special unwinding needed here
bad_alloc:
	// TODO: fill this out
	//devres_release_group();
fail:
	return err;
}

// new device inserted
static int veikk_probe(struct hid_device *hid_dev,
	const struct hid_device_id *id)
{
	struct veikk_device *veikk_dev;
	int err;

	// ignore the generic HID interfaces
	if (!veikk_is_proprietary(hid_dev))
		return 0;

	if (!id->driver_data) {
		hid_err(hid_dev, "veikk model descriptor missing");
		err = -EINVAL;
		goto fail;
	}

	// allocate, fill out veikk device descriptor
	if (!(veikk_dev = devm_kzalloc(&hid_dev->dev,
			sizeof(struct veikk_device), GFP_KERNEL))) {
		hid_err(hid_dev, "error allocating veikk device descriptor");
		err = -ENOMEM;
		goto fail;
	}
	veikk_dev->model = (struct veikk_model *) id->driver_data;
	veikk_dev->hid_dev = hid_dev;
	hid_set_drvdata(hid_dev, veikk_dev);

	// parse report descriptor
	if ((err = hid_parse(hid_dev))) {
		hid_err(hid_dev, "hid_parse error");
		goto fail;
	}

	if ((err = veikk_allocate_setup_register_inputs(hid_dev))) {
		hid_err(hid_dev, "error allocating or registering inputs");
		goto fail;
	}

	if ((err = hid_hw_start(hid_dev, HID_CONNECT_HIDRAW
			| HID_CONNECT_DRIVER))) {
		hid_err(hid_dev, "error signaling hardware start");
		goto fail;
	}

#ifdef VEIKK_DEBUG_MODE
	hid_info(hid_dev, "%s probed successfully.", veikk_dev->model->name);
#endif	// VEIKK_DEBUG_MODE
	return 0;

fail:
	return err;
}

// TODO: be more careful with resources if unallocated
static void veikk_remove(struct hid_device *hid_dev) {
	struct veikk_device *veikk_dev;

	// didn't set up the generic devices, so don't need to clean them up
	if (!veikk_is_proprietary(hid_dev))
		return;

	veikk_dev = hid_get_drvdata(hid_dev);

	if (veikk_dev)
		hid_hw_stop(hid_dev);

#ifdef VEIKK_DEBUG_MODE
	hid_info(hid_dev, "device removed successfully.");
#endif	// VEIKK_DEBUG_MODE
}

// List of VEIKK models
static struct veikk_model veikk_model_0x0001 = {
	.name = "VEIKK S640", .prod_id = 0x0001,
	.x_max = 30480, .y_max = 20320, .pressure_max = 8192,
};
static struct veikk_model veikk_model_0x0002 = {
	.name = "VEIKK A30", .prod_id = 0x0002,
	.x_max = 32768, .y_max = 32768, .pressure_max = 8192,
	.has_buttons = 1, .has_pad = 1
};
static struct veikk_model veikk_model_0x0003 = {
	.name = "VEIKK A50", .prod_id = 0x0003,
	.x_max = 50800, .y_max = 30480, .pressure_max = 8192,
	.has_buttons = 1, .has_pad = 1
};
static struct veikk_model veikk_model_0x0004 = {
	.name = "VEIKK A15", .prod_id = 0x0004,
	.x_max = 32768, .y_max = 32768, .pressure_max = 8192,
	.has_buttons = 1, .has_pad = 1
};
static struct veikk_model veikk_model_0x0006 = {
	.name = "VEIKK A15 Pro", .prod_id = 0x0006,
	.x_max = 32768, .y_max = 32768, .pressure_max = 8192,
	.has_buttons = 1, .has_pad = 1
};
static struct veikk_model veikk_model_0x1001 = {
	.name = "VEIKK VK1560", .prod_id = 0x1001,
	.x_max = 34420, .y_max = 19360, .pressure_max = 8192,
	.has_buttons = 1
};
// TODO: add more tablets

// register models for hotplugging
#define VEIKK_MODEL(prod)\
	HID_USB_DEVICE(VEIKK_VENDOR_ID, prod),\
	.driver_data = (u64) &veikk_model_##prod
static const struct hid_device_id veikk_model_ids[] = {
	{ VEIKK_MODEL(0x0001) },	// S640
	{ VEIKK_MODEL(0x0002) },	// A30
	{ VEIKK_MODEL(0x0003) },	// A50
	{ VEIKK_MODEL(0x0004) },	// A15
	{ VEIKK_MODEL(0x0006) },	// A15 Pro
	{ VEIKK_MODEL(0x1001) },	// VK1560
	{ }
};
MODULE_DEVICE_TABLE(hid, veikk_model_ids);

// register driver
static struct hid_driver veikk_driver = {
	.name = "veikk",
	.id_table = veikk_model_ids,
	.probe = veikk_probe,
	.remove = veikk_remove,
	.raw_event = veikk_raw_event,

	/*
	 * the following are for debugging, .raw_event is the only one used
	 * for real event reporting
	 */
#ifdef VEIKK_DEBUG_MODE
	.report = veikk_report,
	.event = veikk_event,
#endif	// VEIKK_DEBUG_MODE
};
module_hid_driver(veikk_driver);

MODULE_VERSION(VEIKK_DRIVER_VERSION);
MODULE_AUTHOR(VEIKK_DRIVER_AUTHOR);
MODULE_DESCRIPTION(VEIKK_DRIVER_DESC);
MODULE_LICENSE(VEIKK_DRIVER_LICENSE);
