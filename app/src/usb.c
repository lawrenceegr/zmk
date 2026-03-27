/*
 * Copyright (c) 2020 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/device.h>
#include <zephyr/init.h>

#include <zmk/usb.h>
#include <zmk/hid.h>
#include <zmk/keymap.h>
#include <zmk/event_manager.h>
#include <zmk/events/usb_conn_state_changed.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

static enum zmk_usb_status usb_status = ZMK_USB_STATUS_UNKNOWN;
static bool is_configured;

static void raise_usb_status_changed_event(struct k_work *_work) {
    raise_zmk_usb_conn_state_changed(
        (struct zmk_usb_conn_state_changed){.conn_state = zmk_usb_get_conn_state()});
}

K_WORK_DEFINE(usb_status_notifier_work, raise_usb_status_changed_event);

enum zmk_usb_status zmk_usb_get_status(void) { return usb_status; }

enum zmk_usb_conn_state zmk_usb_get_conn_state(void) {
    LOG_DBG("state: %d", usb_status);
    switch (usb_status) {
    case ZMK_USB_STATUS_SUSPENDED:
    case ZMK_USB_STATUS_CONFIGURED:
    case ZMK_USB_STATUS_RESUMED:
        return ZMK_USB_CONN_HID;

    case ZMK_USB_STATUS_DISCONNECTED:
    case ZMK_USB_STATUS_UNKNOWN:
        return ZMK_USB_CONN_NONE;

    default:
        return ZMK_USB_CONN_POWERED;
    }
}

bool zmk_usb_is_hid_ready(void) {
    return zmk_usb_get_conn_state() == ZMK_USB_CONN_HID && is_configured;
}

#if IS_ENABLED(CONFIG_ZMK_USB_STACK_NEXT)

/* ===== New USB Device Stack Implementation ===== */

#include <zephyr/usb/usbd.h>

USBD_DEVICE_DEFINE(zmk_usbd,
                   DEVICE_DT_GET(DT_NODELABEL(zephyr_udc0)),
                   CONFIG_ZMK_USB_VID, CONFIG_ZMK_USB_PID);

USBD_DESC_LANG_DEFINE(zmk_lang);
USBD_DESC_MANUFACTURER_DEFINE(zmk_mfr, CONFIG_ZMK_USB_MANUFACTURER);
USBD_DESC_PRODUCT_DEFINE(zmk_product, CONFIG_ZMK_USB_PRODUCT);
USBD_DESC_SERIAL_NUMBER_DEFINE(zmk_sn);

USBD_DESC_CONFIG_DEFINE(zmk_fs_cfg_desc, "FS Configuration");
USBD_DESC_CONFIG_DEFINE(zmk_hs_cfg_desc, "HS Configuration");

static const uint8_t zmk_usb_attributes = USB_SCD_REMOTE_WAKEUP;

USBD_CONFIGURATION_DEFINE(zmk_fs_config, zmk_usb_attributes, 50, &zmk_fs_cfg_desc);
USBD_CONFIGURATION_DEFINE(zmk_hs_config, zmk_usb_attributes, 50, &zmk_hs_cfg_desc);

struct usbd_context *zmk_usb_get_usbd_context(void) { return &zmk_usbd; }

static void zmk_usbd_msg_cb(struct usbd_context *const usbd_ctx,
                            const struct usbd_msg *const msg) {
    LOG_INF("USBD message: %s", usbd_msg_type_string(msg->type));

    switch (msg->type) {
    case USBD_MSG_CONFIGURATION:
        LOG_INF("  Configuration value %d", msg->status);
        usb_status = ZMK_USB_STATUS_CONFIGURED;
        is_configured = (msg->status != 0);
        break;
    case USBD_MSG_SUSPEND:
        usb_status = ZMK_USB_STATUS_SUSPENDED;
        break;
    case USBD_MSG_RESUME:
        usb_status = ZMK_USB_STATUS_RESUMED;
        break;
    case USBD_MSG_RESET:
        usb_status = ZMK_USB_STATUS_RESET;
        is_configured = false;
        break;
    case USBD_MSG_UDC_ERROR:
    case USBD_MSG_STACK_ERROR:
        usb_status = ZMK_USB_STATUS_ERROR;
        is_configured = false;
        break;
    case USBD_MSG_VBUS_READY:
        usb_status = ZMK_USB_STATUS_CONNECTED;
        if (usbd_enable(usbd_ctx)) {
            LOG_ERR("Failed to enable device support");
        }
        return;
    case USBD_MSG_VBUS_REMOVED:
        usb_status = ZMK_USB_STATUS_DISCONNECTED;
        is_configured = false;
        if (usbd_disable(usbd_ctx)) {
            LOG_ERR("Failed to disable device support");
        }
        break;
    default:
        return;
    }

    k_work_submit(&usb_status_notifier_work);
}

static int zmk_usb_init(void) {
    int err;

    err = usbd_add_descriptor(&zmk_usbd, &zmk_lang);
    if (err) {
        LOG_ERR("Failed to initialize language descriptor (%d)", err);
        return err;
    }

    err = usbd_add_descriptor(&zmk_usbd, &zmk_mfr);
    if (err) {
        LOG_ERR("Failed to initialize manufacturer descriptor (%d)", err);
        return err;
    }

    err = usbd_add_descriptor(&zmk_usbd, &zmk_product);
    if (err) {
        LOG_ERR("Failed to initialize product descriptor (%d)", err);
        return err;
    }

    err = usbd_add_descriptor(&zmk_usbd, &zmk_sn);
    if (err) {
        LOG_ERR("Failed to initialize SN descriptor (%d)", err);
        return err;
    }

    /* Add high-speed configuration if supported */
    if (usbd_caps_speed(&zmk_usbd) == USBD_SPEED_HS) {
        err = usbd_add_configuration(&zmk_usbd, USBD_SPEED_HS, &zmk_hs_config);
        if (err) {
            LOG_ERR("Failed to add High-Speed configuration");
            return err;
        }

        err = usbd_register_all_classes(&zmk_usbd, USBD_SPEED_HS, 1, NULL);
        if (err) {
            LOG_ERR("Failed to register HS classes");
            return err;
        }

        usbd_device_set_code_triple(&zmk_usbd, USBD_SPEED_HS, 0, 0, 0);
    }

    /* Add full-speed configuration */
    err = usbd_add_configuration(&zmk_usbd, USBD_SPEED_FS, &zmk_fs_config);
    if (err) {
        LOG_ERR("Failed to add Full-Speed configuration");
        return err;
    }

    err = usbd_register_all_classes(&zmk_usbd, USBD_SPEED_FS, 1, NULL);
    if (err) {
        LOG_ERR("Failed to register FS classes");
        return err;
    }

    usbd_device_set_code_triple(&zmk_usbd, USBD_SPEED_FS, 0, 0, 0);

    err = usbd_msg_register_cb(&zmk_usbd, zmk_usbd_msg_cb);
    if (err) {
        LOG_ERR("Failed to register message callback");
        return err;
    }

    err = usbd_init(&zmk_usbd);
    if (err) {
        LOG_ERR("Failed to initialize device support");
        return err;
    }

    if (!usbd_can_detect_vbus(&zmk_usbd)) {
        err = usbd_enable(&zmk_usbd);
        if (err) {
            LOG_ERR("Failed to enable device support");
            return err;
        }
    }

    return 0;
}

#else /* Legacy USB Device Stack */

#include <zephyr/usb/usb_device.h>
#include <zephyr/usb/class/usb_hid.h>
#include <zmk/usb_hid.h>

static void usb_status_cb(enum usb_dc_status_code status, const uint8_t *params) {
    // Start-of-frame events are too frequent and noisy to notify
    if (status == USB_DC_SOF) {
        return;
    }

#if IS_ENABLED(CONFIG_ZMK_USB_BOOT)
    if (status == USB_DC_RESET) {
        zmk_usb_hid_set_protocol(HID_PROTOCOL_REPORT);
    }
#endif

    switch (status) {
    case USB_DC_SUSPEND:
        usb_status = ZMK_USB_STATUS_SUSPENDED;
        break;
    case USB_DC_CONFIGURED:
        usb_status = ZMK_USB_STATUS_CONFIGURED;
        break;
    case USB_DC_RESUME:
    case USB_DC_CLEAR_HALT:
    case USB_DC_SOF:
        usb_status = ZMK_USB_STATUS_RESUMED;
        break;
    case USB_DC_DISCONNECTED:
        usb_status = ZMK_USB_STATUS_DISCONNECTED;
        break;
    case USB_DC_ERROR:
        usb_status = ZMK_USB_STATUS_ERROR;
        break;
    case USB_DC_RESET:
        usb_status = ZMK_USB_STATUS_RESET;
        break;
    default:
        usb_status = ZMK_USB_STATUS_UNKNOWN;
        break;
    }

    if (zmk_usb_get_conn_state() == ZMK_USB_CONN_HID) {
        is_configured |= usb_status == ZMK_USB_STATUS_CONFIGURED;
    } else {
        is_configured = false;
    }
    k_work_submit(&usb_status_notifier_work);
}

static int zmk_usb_init(void) {
    int usb_enable_ret;

    usb_enable_ret = usb_enable(usb_status_cb);

    if (usb_enable_ret != 0) {
        LOG_ERR("Unable to enable USB");
        return -EINVAL;
    }

    return 0;
}

#endif /* CONFIG_ZMK_USB_STACK_NEXT */

SYS_INIT(zmk_usb_init, APPLICATION, CONFIG_ZMK_USB_INIT_PRIORITY);
