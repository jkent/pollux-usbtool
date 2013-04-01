/*
 * Copyright (C) 2013 Jeff Kent <jeff@jkent.net>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include <stdio.h>
#include <string.h>

#include "asm/io.h"
#include "util.h"

#include "udc/udc.h"
#include "udc/stream_descriptors.h"

static struct udc_req _req;
static struct udc_req rx_req = {0};

static void rx_complete(struct udc_ep *ep, struct udc_req *req)
{
	if (req->status) {
		putchar('x'); fflush(stdout);
		return;
	}

#if 0
	char *p = (char *) req->buf;
	int i;
	for (i = 0; i < req->actual; i++)
		putchar(*p++);
	putchar('\n');
#endif

	ep->ops->queue(ep, req);
}

static void configured(struct udc *udc)
{
	//struct udc_ep *tx_ep = &udc->ep[1];
	struct udc_ep *rx_ep = &udc->ep[2];

	if (!rx_req.buf) {
		rx_req.buf = malloc(1024 * 128);
		rx_req.length = 1024 * 64;
		rx_req.complete = rx_complete;
	}
	if (list_empty(&rx_ep->queue)) {
		INIT_LIST_HEAD(&rx_req.queue);
		rx_ep->ops->queue(rx_ep, &rx_req);
	}
}

static inline int process_req_desc(struct udc *udc,
		struct usb_ctrlrequest *ctrl)
{
	struct udc_ep *ep = &udc->ep[0];
	struct udc_req *req = &_req;
	int i;

	bzero(req, sizeof(*req));

	switch (ctrl->wValue >> 8) {
	case USB_DT_DEVICE:
		if (udc->speed == USB_SPEED_HIGH) {
			req->buf = (u16 *)&stream_dths_dev;
			req->length = stream_dths_dev.bLength;
		} else {
			req->buf = (u16 *)&stream_dtfs_dev;
			req->length = stream_dtfs_dev.bLength;
		}
		break;

	case USB_DT_DEVICE_QUALIFIER:
		if (udc->speed == USB_SPEED_HIGH) {
			req->buf = (u16 *)&stream_dths_qual;
			req->length = stream_dths_qual.bLength;
		} else {
			req->buf = (u16 *)&stream_dtfs_qual;
			req->length = stream_dtfs_qual.bLength;
		}
		break;

	case USB_DT_OTHER_SPEED_CONFIG:
		if (udc->speed == USB_SPEED_HIGH) {
			stream_dtfs_config.cfg.bDescriptorType =
					USB_DT_OTHER_SPEED_CONFIG;
			req->buf = (u16 *)&stream_dtfs_dev;
			req->length = stream_dtfs_dev.bLength;
		} else {
			stream_dths_config.cfg.bDescriptorType =
					USB_DT_OTHER_SPEED_CONFIG;
			req->buf = (u16 *)&stream_dths_dev;
			req->length = stream_dths_dev.bLength;
		}
		break;

	case USB_DT_CONFIG:
		if (udc->speed == USB_SPEED_HIGH) {
			stream_dths_config.cfg.bDescriptorType = USB_DT_CONFIG;
			req->buf = (u16 *)&stream_dths_config;
			req->length = stream_dths_config.cfg.wTotalLength;
		} else {
			stream_dtfs_config.cfg.bDescriptorType = USB_DT_CONFIG;
			req->buf = (u16 *)&stream_dtfs_config;
			req->length = stream_dtfs_config.cfg.wTotalLength;
		}
		break;

	case USB_DT_STRING:
		i = ctrl->wValue & 0xFF;
		if (i >= NUM_STRING_DESC)
			return -1;
		req->buf = (u16 *)stream_dt_string[i];
		req->length = stream_dt_string[i]->bLength;
		break;

	default:
		return -1;
	}

	INIT_LIST_HEAD(&req->queue);
	req->length = min((u32)ctrl->wLength, req->length);
	ep->ops->queue(ep, req);
	return 0;
}

static inline void set_config(struct udc *udc, int config)
{
	struct udc_ep *ep1, *ep2;
	struct usb_endpoint_descriptor *desc1, *desc2;

	ep1 = &udc->ep[1];
	ep2 = &udc->ep[2];

	if (udc->speed == USB_SPEED_HIGH) {
		desc1 = &stream_dths_config.ep1;
		desc2 = &stream_dths_config.ep2;
	} else {
		desc1 = &stream_dtfs_config.ep1;
		desc2 = &stream_dtfs_config.ep2;
	}

	if (config) {
		ep1->ops->enable(ep1, desc1);
		ep2->ops->enable(ep2, desc2);
		ep1->ops->fifo_flush(ep1);
		ep2->ops->fifo_flush(ep2);
		configured(udc);
	} else {
		ep1->ops->disable(ep1);
		ep2->ops->disable(ep2);
	}

	udc->config = config;
}

static inline int process_req_config(struct udc *udc,
		struct usb_ctrlrequest *ctrl)
{
	struct udc_ep *ep = &udc->ep[0];
	struct udc_req *req = &_req;

	if (ctrl->bRequest == USB_REQ_SET_CONFIGURATION) {
		if (ctrl->wValue > NUM_CONFIG_DESC)
			return -1;
		set_config(udc, ctrl->wValue);
	} else {
		bzero(req, sizeof(*req));
		req->buf = &udc->config;
		req->length = 1;
		ep->ops->queue(ep, req);
	}
	return 0;
}

static inline int process_req_iface(struct udc *udc,
		struct usb_ctrlrequest *ctrl)
{
	struct udc_ep *ep = &udc->ep[0];
	struct udc_req *req = &_req;
	u8 interface = ctrl->wIndex & 0xff;
	u8 alternate = ctrl->wValue & 0xff;
	u16 reply;

	if (ctrl->bRequest == USB_REQ_SET_INTERFACE) {
		if (interface || alternate)
			return -1;
	} else {
		bzero(req, sizeof(*req));
		INIT_LIST_HEAD(&req->queue);
		reply = 0;
		req->buf = &reply;
		req->length = 1;
		ep->ops->queue(ep, req);
	}
	return 0;
}

static int process_setup(struct udc *udc, struct usb_ctrlrequest *ctrl)
{
	if ((ctrl->bRequestType & USB_TYPE_MASK) != USB_TYPE_STANDARD)
		return -1;

	switch (ctrl->bRequest) {
	case USB_REQ_GET_DESCRIPTOR:
		if ((ctrl->bRequestType & USB_RECIP_MASK) != USB_RECIP_DEVICE)
			break;
		return process_req_desc(udc, ctrl);

	case USB_REQ_GET_CONFIGURATION:
	case USB_REQ_SET_CONFIGURATION:
		if ((ctrl->bRequestType & USB_RECIP_MASK) != USB_RECIP_DEVICE)
			break;
		return process_req_config(udc, ctrl);

	case USB_REQ_GET_INTERFACE:
	case USB_REQ_SET_INTERFACE:
		if ((ctrl->bRequestType & USB_RECIP_MASK) != USB_RECIP_INTERFACE)
			break;
		return process_req_iface(udc, ctrl);
	}
	return -1;
}

struct udc_driver udc_stream_driver = {
	.setup = process_setup,
};

