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
#include "baremetal/util.h"

#include "udc.h"
#include "usbtool_descriptors.h"


static struct udc_req setup_req = {0};
static struct udc_req command_req = {0};
static struct udc_req buffer_req = {0};

static u16 command_buf[256];


static void configured(struct udc *udc)
{
	struct udc_ep *rx_ep = &udc->ep[2];

	if (list_empty(&rx_ep->queue))
		rx_ep->ops->queue(rx_ep, &command_req);
}

static inline int process_req_desc(struct udc *udc,
		struct usb_ctrlrequest *ctrl)
{
	struct udc_ep *ep = &udc->ep[0];
	struct udc_req *req = &setup_req;
	int i;

	switch (ctrl->wValue >> 8) {
	case USB_DT_DEVICE:
		if (udc->speed == USB_SPEED_HIGH) {
			req->buf = (u16 *)&usbtool_dths_dev;
			req->length = usbtool_dths_dev.bLength;
		} else {
			req->buf = (u16 *)&usbtool_dtfs_dev;
			req->length = usbtool_dtfs_dev.bLength;
		}
		break;

	case USB_DT_DEVICE_QUALIFIER:
		if (udc->speed == USB_SPEED_HIGH) {
			req->buf = (u16 *)&usbtool_dths_qual;
			req->length = usbtool_dths_qual.bLength;
		} else {
			req->buf = (u16 *)&usbtool_dtfs_qual;
			req->length = usbtool_dtfs_qual.bLength;
		}
		break;

	case USB_DT_OTHER_SPEED_CONFIG:
		if (udc->speed == USB_SPEED_HIGH) {
			usbtool_dtfs_config.cfg.bDescriptorType =
					USB_DT_OTHER_SPEED_CONFIG;
			req->buf = (u16 *)&usbtool_dtfs_dev;
			req->length = usbtool_dtfs_dev.bLength;
		} else {
			usbtool_dths_config.cfg.bDescriptorType =
					USB_DT_OTHER_SPEED_CONFIG;
			req->buf = (u16 *)&usbtool_dths_dev;
			req->length = usbtool_dths_dev.bLength;
		}
		break;

	case USB_DT_CONFIG:
		if (udc->speed == USB_SPEED_HIGH) {
			usbtool_dths_config.cfg.bDescriptorType = USB_DT_CONFIG;
			req->buf = (u16 *)&usbtool_dths_config;
			req->length = usbtool_dths_config.cfg.wTotalLength;
		} else {
			usbtool_dtfs_config.cfg.bDescriptorType = USB_DT_CONFIG;
			req->buf = (u16 *)&usbtool_dtfs_config;
			req->length = usbtool_dtfs_config.cfg.wTotalLength;
		}
		break;

	case USB_DT_STRING:
		i = ctrl->wValue & 0xFF;
		if (i >= NUM_STRING_DESC)
			return -1;
		req->buf = (u16 *)usbtool_dt_string[i];
		req->length = usbtool_dt_string[i]->bLength;
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
		desc1 = &usbtool_dths_config.ep1;
		desc2 = &usbtool_dths_config.ep2;
	} else {
		desc1 = &usbtool_dtfs_config.ep1;
		desc2 = &usbtool_dtfs_config.ep2;
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
	struct udc_req *req = &setup_req;

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
	struct udc_req *req = &setup_req;
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

static void handle_command(struct udc_ep *ep, struct udc_req *req)
{
	if (req->status) {
		iprintf("cmd req error #%d\n", req->status);
		return;
	}

	struct udc *udc = ep->dev;
	struct udc_ep *tx_ep = &udc->ep[1];
	struct udc_ep *rx_ep = &udc->ep[2];

	char *buf = (char *)req->buf;
	buf[req->actual] = '\0';

	char group[9], command[9];
	u32 n1, n2, n3, n4;
	int ret;

	ret = sscanf(buf, "%8s %8s %8x %8x %8x %8x", group, command, &n1, &n2, &n3, &n4);
	if (ret < 2)
		goto requeue;

	if (strcmp(group, "buffer") == 0) {
		if (strcmp(command, "read") == 0) {
			if (ret != 4)
				goto requeue;

			u32 offset = n1 & 0xFFFFFE;
			buffer_req.buf = (u16 *)(0x1000000 + offset);
			buffer_req.length = min(0x1000000 - offset,
					n2 & ~1);

			ep->ops->queue(tx_ep, &buffer_req);
			return;
		} else if (strcmp(command, "write") == 0) {
			if (ret != 4)
				goto requeue;

			u32 offset = n1 & 0xFFFFFE;
			buffer_req.buf = (u16 *)(0x1000000 + offset);
			buffer_req.length = min(0x1000000 - offset,
					n2 & ~1);

			ep->ops->queue(rx_ep, &buffer_req);
			return;
		}
	}

requeue:
	ep->ops->queue(ep, req);
}

static void buffer_req_complete(struct udc_ep *ep, struct udc_req *req)
{
	struct udc *udc = ep->dev;
	struct udc_ep *tx_ep = &udc->ep[1];
	struct udc_ep *rx_ep = &udc->ep[2];

	if (req->status) {
		iprintf("buf req error #%d\n", req->status);
		return;
	}

	ep->ops->queue(rx_ep, &command_req);
}

static void init(struct udc *udc)
{
	command_req.buf = command_buf;
	command_req.length = sizeof(command_buf) - 2;
	command_req.complete = handle_command;
	INIT_LIST_HEAD(&command_req.queue);

	buffer_req.complete = buffer_req_complete;
	INIT_LIST_HEAD(&buffer_req.queue);
}

struct udc_driver usbtool_udc_driver = {
	.setup = process_setup,
	.init = init,
};

