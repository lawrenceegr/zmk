/*
 * Copyright (c) 2020 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <zmk/keys.h>
#include <zmk/hid.h>

enum zmk_usb_conn_state {
    ZMK_USB_CONN_NONE,
    ZMK_USB_CONN_POWERED,
    ZMK_USB_CONN_HID,
};

enum zmk_usb_status {
    ZMK_USB_STATUS_UNKNOWN,
    ZMK_USB_STATUS_DISCONNECTED,
    ZMK_USB_STATUS_CONNECTED,
    ZMK_USB_STATUS_CONFIGURED,
    ZMK_USB_STATUS_SUSPENDED,
    ZMK_USB_STATUS_RESUMED,
    ZMK_USB_STATUS_RESET,
    ZMK_USB_STATUS_ERROR,
};

enum zmk_usb_status zmk_usb_get_status(void);
enum zmk_usb_conn_state zmk_usb_get_conn_state(void);

static inline bool zmk_usb_is_powered(void) {
    return zmk_usb_get_conn_state() != ZMK_USB_CONN_NONE;
}
bool zmk_usb_is_hid_ready(void);

#if IS_ENABLED(CONFIG_ZMK_USB_STACK_NEXT)
#include <zephyr/usb/usbd.h>
struct usbd_context *zmk_usb_get_usbd_context(void);
#endif
