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

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "asm/io.h"
#include "asm/types.h"
#include "baremetal/util.h"
#include "mach/udc.h"
#include "linux/list.h"
#include "linux/usb/ch9.h"

#include "udc.h"

#define ESHUTDOWN 108

static struct udc _udc;

#define ep_index(_ep)		((_ep)->address & USB_ENDPOINT_NUMBER_MASK)
#define ep_is_in(_ep)		((_ep)->address & USB_DIR_IN)

static inline void set_index(struct udc *udc, int addr)
{
	addr &= USB_ENDPOINT_NUMBER_MASK;
	writew(addr, udc->regs + UDC_IR);
}

static void udc_complete_req(struct udc_ep *ep,
		struct udc_req *req, int status)
{
	struct udc *udc = ep->dev;

	list_del_init(&req->queue);
	req->status = status;

	if (!ep_index(ep)) {
		udc->ep0_state = WAIT_FOR_SETUP;
		ep->address &= ~USB_DIR_IN;
	}

	if (req->complete)
		req->complete(ep, req);
}

static void udc_nuke_ep(struct udc_ep *ep, int status)
{
	struct udc_req *req;

	while (!list_empty(&ep->queue)) {
		req = list_entry(ep->queue.next,
				struct udc_req, queue);
		udc_complete_req(ep, req, status);
	}
}

static inline int udc_read_setup_pkt(struct udc *udc, u16 *buf)
{
	void __iomem *fifo = udc->ep[0].fifo;
	int read_count, count;
	u16 word;

	read_count = readw(udc->regs + UDC_BRCR);
	count = 0;
	while (read_count--) {
		word = readw(fifo);
		if (count++ < 4)
			*buf++ = word;
	}
	writew(UDC_EP0SR_RX_SUCCESS, udc->regs + UDC_EP0SR);

	return count;
}

static int udc_write_fifo(struct udc_ep *ep, struct udc_req *req)
{
	struct udc *udc = ep->dev;
	void __iomem *fifo = ep->fifo;
	u16 *buf;
	u32 max = ep->maxpacket;
	u32 count, length;
	bool is_last;

	buf = req->buf + req->actual;
	length = req->length - req->actual;
	length = min(length, max);
	req->actual += length;

	writew(length, udc->regs + UDC_BWCR);
	for (count = 0; count < length; count += 2)
		writew(*buf++, fifo);

	if (length != max) {
		is_last = true;
	} else {
		if (req->length != req->actual || req->zero)
			is_last = false;
		else
			is_last = true;
	}

	if (is_last)
		udc_complete_req(ep, req, 0);

	return is_last;
}

static int udc_read_fifo(struct udc_ep *ep, struct udc_req *req)
{
	struct udc *udc = ep->dev;
	void __iomem *fifo = ep->fifo;
	u16 *buf, word;
	int buflen, count, length, bytes;
	u32 offset;
	u16 esr;
	int is_last = 0;

	offset = ep_index(ep) ? UDC_ESR : UDC_EP0SR;
	esr = readw(udc->regs + offset);
	if (!(esr & UDC_ESR_RX_SUCCESS))
		return -EINVAL;

	buf = req->buf + req->actual;
	buflen = req->length - req->actual;

	count = readw(udc->regs + UDC_BRCR);
	length = count * 2;
	if (esr & (ep_index(ep) ? UDC_ESR_LWO : UDC_EP0SR_EP0_LWO))
		length -= 1;

	bytes = min(length, buflen);

	req->actual += bytes;
	is_last = (length < ep->maxpacket);

	while (count--) {
		word = readw(fifo);
		if (buflen) {
			*buf++ = word;
			buflen -= 2;
		} else {
			req->status = -EOVERFLOW;
		}
	}

	if (!ep_index(ep)) {
		writew(UDC_ESR_RX_SUCCESS, udc->regs + UDC_EP0SR);

		/* undocumented bits in ep0sr that signal last data */
		is_last |= (esr & (1 << 15)) || (esr & (1 << 12));
	}

	is_last |= (req->actual == req->length);

	if (is_last)
		udc_complete_req(ep, req, 0);

	return is_last;
}

static inline void udc_epin_intr(struct udc *udc, struct udc_ep *ep)
{
	struct udc_req *req;
	u16 esr;

	esr = readw(udc->regs + UDC_ESR);
	if (esr & UDC_ESR_STALL) {
		writew(UDC_ESR_STALL, udc->regs + UDC_ESR);
		return;
	}

	if (esr & UDC_ESR_TX_SUCCESS) {
		writew(UDC_ESR_TX_SUCCESS, udc->regs + UDC_ESR);
		if (list_empty(&ep->queue))
			return;

		req = list_entry(ep->queue.next,
				struct udc_req, queue);
		if (!udc_write_fifo(ep, req) && (esr & UDC_ESR_PSIF_TWO))
			udc_write_fifo(ep, req);
	}
}

static inline void udc_epout_intr(struct udc *udc, struct udc_ep *ep)
{
	struct udc_req *req;
	u16 esr;
	u16 ecr;

	esr = readw(udc->regs + UDC_ESR);
	if (esr & UDC_ESR_STALL) {
		writew(UDC_ESR_STALL, udc->regs + UDC_ESR);
		return;
	}

	if (esr & UDC_ESR_FLUSH) {
		ecr = readw(udc->regs + UDC_ECR);
		ecr |= UDC_ECR_FLUSH;
		writew(ecr, udc->regs + UDC_ECR);
	}

	if (esr & UDC_ESR_RX_SUCCESS) {
		if (list_empty(&ep->queue))
			return;

		req = list_entry(ep->queue.next,
				struct udc_req, queue);
		if (!udc_read_fifo(ep, req) && (esr & UDC_ESR_PSIF_TWO))
			udc_read_fifo(ep, req);
	}	
}

static int udc_set_halt(struct udc_ep *ep, bool halt)
{
	struct udc *udc = ep->dev;
	struct udc_req *req;
	u16 ecr;
	u32 offset;

	if (halt && ep_is_in(ep) && !list_empty(&ep->queue))
		return -EAGAIN;

	set_index(udc, ep->address);
	offset = ep_index(ep) ? UDC_ECR : UDC_EP0CR;

	ecr = readw(udc->regs + offset);
	if (halt) {
		ecr |= UDC_ECR_STALL;
		if (ep_index(ep))
			ecr |= UDC_ECR_FLUSH;
		ep->stopped = 1;
	} else {
		ecr &= ~UDC_ECR_STALL;
		ep->stopped = 0;
	}
	writew(ecr, udc->regs + offset);

	if (ep_is_in(ep) && !list_empty(&ep->queue) && !halt) {
		req = list_entry(ep->queue.next,
			struct udc_req, queue);
		if (req)
			udc_write_fifo(ep, req);
	}
	return 0;
}

static inline int udc_process_req_feature(struct udc *udc,
		struct usb_ctrlrequest *ctrl)
{
	struct udc_ep *ep;
	bool set = (ctrl->bRequest == USB_REQ_SET_FEATURE);	
	u8 epnum = ctrl->wIndex & USB_ENDPOINT_NUMBER_MASK;

	if (ctrl->bRequestType == USB_RECIP_ENDPOINT) {
		switch (ctrl->wValue) {
		case USB_ENDPOINT_HALT:
			if (epnum > NUM_ENDPOINTS)
				return -1;
			ep = &udc->ep[epnum];
			udc_set_halt(ep, set);
			udc->ep0_state = WAIT_FOR_SETUP;
			return 0;
		}
	}

	return -1;
}

static inline int udc_process_req_status(struct udc *udc,
		struct usb_ctrlrequest *ctrl)
{
	struct udc_req req;
	struct udc_ep *ep0 = &udc->ep[0];
	u16 reply;
	u8 epnum;

	bzero(&req, sizeof(req));

	switch (ctrl->bRequestType & USB_RECIP_MASK) {
	case USB_RECIP_DEVICE:
		reply = (1 << USB_DEVICE_SELF_POWERED);
		break;

	case USB_RECIP_INTERFACE:
		reply = 0;
		break;

	case USB_RECIP_ENDPOINT:
		epnum = ctrl->wIndex & USB_ENDPOINT_NUMBER_MASK;
		if (epnum > NUM_ENDPOINTS)
			return -1;
		reply = udc->ep[epnum].stopped ? 1 : 0;
		break;
	}

	INIT_LIST_HEAD(&req.queue);
	req.length = 2;
	req.buf = &reply;
	udc_write_fifo(ep0, &req);
	return 0;
}

static inline void udc_process_setup_pkt(struct udc *udc,
		struct usb_ctrlrequest *ctrl)
{
	struct udc_ep *ep0 = &udc->ep[0];
	int ret = -1;

	if (ctrl->bRequestType & USB_DIR_IN) {
		ep0->address |= USB_DIR_IN;
		udc->ep0_state = DATA_STATE_XMIT;
	} else {
		ep0->address &= ~USB_DIR_IN;
		udc->ep0_state = DATA_STATE_RECV;
	}

	if ((ctrl->bRequestType & USB_TYPE_MASK) != USB_TYPE_STANDARD)
		goto unhandled;

	switch (ctrl->bRequest) {
	case USB_REQ_SET_ADDRESS:
		if ((ctrl->bRequestType & USB_RECIP_MASK) != USB_RECIP_DEVICE)
			break;
		udc->state = USB_STATE_ADDRESS;
		ret = 0;
		goto handled;

	case USB_REQ_GET_STATUS:
		ret = udc_process_req_status(udc, ctrl);
		goto handled;

	case USB_REQ_SET_FEATURE:
	case USB_REQ_CLEAR_FEATURE:
		ret = udc_process_req_feature(udc, ctrl);
		goto handled;
	}

unhandled:
	/* pass it to our driver */
	if (udc->driver)
		ret = udc->driver->setup(udc, ctrl);

	if (ctrl->bRequestType == (USB_TYPE_STANDARD | USB_RECIP_DEVICE) &&
			ctrl->bRequest == USB_REQ_SET_CONFIGURATION) {
		udc_nuke_ep(ep0, -ECONNRESET);
		udc->state = (udc->config) ? USB_STATE_CONFIGURED :
				USB_STATE_ADDRESS;
	}

handled:
	if (ret < 0) {
		udc_set_halt(ep0, 1);
		ep0->address &= ~USB_DIR_IN;
		udc->ep0_state = WAIT_FOR_SETUP;
		return;
	}

	if (ctrl->wLength == 0) {
		ep0->address &= ~USB_DIR_IN;
		udc->ep0_state = WAIT_FOR_SETUP;
	}
}

static void udc_ep0_intr(struct udc *udc)
{
	struct udc_ep *ep0 = &udc->ep[0];
	struct usb_ctrlrequest ctrl;
	struct udc_req *req;
	u16 esr = readw(udc->regs + UDC_EP0SR);
	u16 ecr;

	set_index(udc, 0);

	if (esr & UDC_EP0SR_STALL) {
		ecr = readw(udc->regs + UDC_EP0CR);
		ecr &= ~(UDC_ECR_STALL | UDC_ECR_FLUSH);
		writew(ecr, udc->regs + UDC_EP0CR);

		writew(UDC_EP0SR_STALL, udc->regs + UDC_EP0SR);
		ep0->stopped = 0;

		udc_nuke_ep(ep0, -ECONNABORTED);
		udc->ep0_state = WAIT_FOR_SETUP;
		ep0->address &= ~USB_DIR_IN;
		return;
	}

	if (esr & UDC_EP0SR_TX_SUCCESS) {
		writew(UDC_EP0SR_TX_SUCCESS, udc->regs + UDC_EP0SR);
		if (ep_is_in(ep0)) {
			if (list_empty(&ep0->queue))
				return;
			req = list_entry(ep0->queue.next,
					struct udc_req, queue);
			udc_write_fifo(ep0, req);
		}
	}

	if (esr & UDC_EP0SR_RX_SUCCESS) {
		if (udc->ep0_state == WAIT_FOR_SETUP) {
			udc_nuke_ep(ep0, -EPROTO);
			if (udc_read_setup_pkt(udc, (u16 *)&ctrl))
				udc_process_setup_pkt(udc, &ctrl);
		}
		else if (!ep_is_in(ep0)) {
			if (list_empty(&ep0->queue))
				return;
			req = list_entry(ep0->queue.next,
					struct udc_req, queue);
			udc_read_fifo(ep0, req);
		}
	}
}

static int udc_enable_ep(struct udc_ep *ep,
		const struct usb_endpoint_descriptor *desc)
{
	struct udc *udc;
	u16 ecr;
	u16 eier;
	u16 edr;

	if (!ep || !desc || ep_index(ep) == 0
			|| desc->bDescriptorType != USB_DT_ENDPOINT
			|| ep_index(ep) != usb_endpoint_num(desc)
			|| ep->maxpacket < desc->wMaxPacketSize)
		return -EINVAL;

	if ((usb_endpoint_xfer_bulk(desc)
			&& ep->maxpacket != desc->wMaxPacketSize)
			|| !desc->wMaxPacketSize)
		return -ERANGE;

	udc = ep->dev;
	if (!udc->driver || udc->speed == USB_SPEED_UNKNOWN)
		return -ESHUTDOWN;

	set_index(udc, ep->address);
	edr = readw(udc->regs + UDC_EDR);
	if (usb_endpoint_dir_in(desc)) {
		ep->address |= USB_DIR_IN;
		edr |= 1 << ep_index(ep);
	} else {
		ep->address &= ~USB_DIR_IN;
		edr &= ~(1 << ep_index(ep));
	}
	writew(edr, udc->regs + UDC_EDR);

	ecr = usb_endpoint_xfer_int(desc) ? UDC_ECR_IEMS : UDC_ECR_DUEN;
	ecr |= UDC_ECR_CDP;
	writew(ecr, udc->regs + UDC_ECR);

	ep->maxpacket = desc->wMaxPacketSize;
	udc_set_halt(ep, 0);

	eier = readw(udc->regs + UDC_EIER);
	eier |= 1 << ep_index(ep);
	writew(eier, udc->regs + UDC_EIER);

	return 0;
}
	
static int udc_disable_ep(struct udc_ep *ep)
{
	struct udc *udc;
	u16 eier;

	if (!ep)
		return -EINVAL;

	udc = ep->dev;

	set_index(udc, ep->address);
	eier = readw(udc->regs + UDC_EIER);
	eier &= ~ep_index(ep);
	writew(eier, udc->regs + UDC_EIER);

	udc_nuke_ep(ep, -ESHUTDOWN);
	ep->stopped = 1;

	return 0;
}

static void udc_free_req(struct udc_ep *ep, struct udc_req *req)
{
	free(req);
}

static struct udc_req *udc_alloc_req(struct udc_ep *ep)
{
	struct udc_req *req;

	req = malloc(sizeof(*req));
	if (!req)
		return NULL;

	bzero(req, sizeof(*req));

	INIT_LIST_HEAD(&req->queue);
	req->complete = udc_free_req;

	return req;
}

static int udc_queue(struct udc_ep *ep, struct udc_req *req)
{
	struct udc *udc;
	u32 offset;
	u16 esr;

	if (!ep || !req || !req->buf ||	!list_empty(&req->queue))
		return -EINVAL;

	udc = ep->dev;
	if (!udc->driver || udc->speed == USB_SPEED_UNKNOWN)
		return -ESHUTDOWN;

	set_index(udc, ep->address);

	req->status = -EINPROGRESS;
	req->actual = 0;

	if (!ep_index(ep) && req->length == 0) {
		ep->address &= ~USB_DIR_IN;
		udc->ep0_state = WAIT_FOR_SETUP;
		udc_complete_req(ep, req, 0);
		return 0;
	}

	if (list_empty(&ep->queue) && !ep->stopped) {
		offset = ep_index(ep) ? UDC_ESR : UDC_EP0SR;
		esr = readw(udc->regs + offset);
		if (ep_is_in(ep)) {
			if (!(esr & UDC_ESR_TX_SUCCESS) &&
					(udc_write_fifo(ep, req) == 1))
				req = NULL;
		} else {
			if ((esr & UDC_ESR_RX_SUCCESS) &&
					(udc_read_fifo(ep, req) == 1))
				req = NULL;
		}
	}

	if (req)
		list_add_tail(&req->queue, &ep->queue);

	return 0;
}

void udc_fifo_flush(struct udc_ep *ep)
{
	struct udc *udc = ep->dev;
	u16 ecr;
	u8 count;

	set_index(udc, ep->address);
	if (ep_is_in(ep)) {
		ecr = readw(udc->regs + UDC_ECR);
		ecr |= UDC_ECR_FLUSH;
		writew(ecr, udc->regs + UDC_ECR);
	} else {
		while ((readw(udc->regs + UDC_ESR) >> 2) & 3) {
			count = readw(udc->regs + UDC_BRCR);
			while (count--)
				readw(ep->fifo);
		}
	}
}

static struct udc_ep_ops udc_ep_ops = {
	.enable = udc_enable_ep,
	.disable = udc_disable_ep,
	.alloc_req = udc_alloc_req,
	.free_req = udc_free_req,
	.queue = udc_queue,
	.set_halt = udc_set_halt,
	.fifo_flush = udc_fifo_flush,
};

static void udc_init_ep(struct udc *udc, u8 epnum)
{
	struct udc_ep *ep;

	if (epnum > NUM_ENDPOINTS)
		return;

	ep = &udc->ep[epnum];

	ep->address = epnum;

	INIT_LIST_HEAD(&ep->queue);

	ep->ops = &udc_ep_ops;
	ep->dev = udc;
	if (epnum)
		ep->maxpacket = (udc->speed == USB_SPEED_HIGH) ? 512 : 64;
	else
		ep->maxpacket = (udc->speed == USB_SPEED_HIGH) ? 64 : 8;

	ep->fifo = udc->regs + UDC_BR(epnum);
	ep->stopped = 0;

	set_index(udc, epnum);
	writew(ep->maxpacket, udc->regs + UDC_MPR);
}

static void udc_reconfig(struct udc *udc)
{
	int epnum;

	writew(UDC_EP0, udc->regs + UDC_EIER);
	writew(0, udc->regs + UDC_TR);
	writew(UDC_SCR_DTZIEN_EN | UDC_SCR_RRD_EN | UDC_SCR_SUS_EN |
			UDC_SCR_RST_EN, udc->regs + UDC_SCR);
	writew(0, udc->regs + UDC_EP0CR);

	for (epnum = 0; epnum < NUM_ENDPOINTS; epnum++)
		udc_init_ep(udc, epnum);

	udc->ep0_state = WAIT_FOR_SETUP;
	udc->speed = USB_SPEED_UNKNOWN;
}

void udc_task(void)
{
	struct udc *udc = &_udc;
	struct udc_ep *ep;
	u16 ep_intr;
	u16 sys_status;
	u8 epnum;

	sys_status = readw(udc->regs + UDC_SSR);
	ep_intr = readw(udc->regs + UDC_EIR);

	if (!ep_intr && !(sys_status & UDC_SSR_FLAGS))
		return;

	if (sys_status) {
		if (sys_status & UDC_SSR_VBUSON) {
			writew(UDC_SSR_VBUSON, udc->regs + UDC_SSR);
			udc->state = USB_STATE_ATTACHED;
			if (udc->driver && udc->driver->vbuson)
				udc->driver->vbuson(udc);
		}

		if (sys_status & UDC_SSR_VBUSOFF) {
			writew(UDC_SSR_VBUSON, udc->regs + UDC_SSR);
			udc->state = USB_STATE_NOTATTACHED;
			if (udc->driver && udc->driver->vbusoff)
				udc->driver->vbusoff(udc);
		}			

		if (sys_status & UDC_SSR_ERR)
			writew(UDC_SSR_ERR, udc->regs + UDC_SSR);

		if (sys_status & UDC_SSR_SDE) {
			writew(UDC_SSR_SDE, udc->regs + UDC_SSR);
			udc->speed = (sys_status & UDC_SSR_HSP) ?
				USB_SPEED_HIGH : USB_SPEED_FULL;
			udc_init_ep(udc, 0);
			udc->state = USB_STATE_DEFAULT;
		}

		if (sys_status & UDC_SSR_SUSPEND)
			writew(UDC_SSR_SUSPEND, udc->regs + UDC_SSR);

		if (sys_status & UDC_SSR_RESUME)
			writew(UDC_SSR_RESUME, udc->regs + UDC_SSR);

		if (sys_status & UDC_SSR_RESET) {
			writew(UDC_SSR_RESET, udc->regs + UDC_SSR);
			udc_reconfig(udc);
			udc->state = USB_STATE_ATTACHED;
		}
	}

	if (ep_intr & UDC_EP0) {
		writew(UDC_EP0, udc->regs + UDC_EIR);
		udc_ep0_intr(udc);
	}

	for (epnum = 1; epnum < NUM_ENDPOINTS; epnum++) {
		if (!(ep_intr & (1 << epnum)))
			continue;

		ep = &udc->ep[epnum];
		writew(1 << epnum, udc->regs + UDC_EIR);
		set_index(udc, epnum);
		if (ep_is_in(ep))
			udc_epin_intr(udc, ep);
		else
			udc_epout_intr(udc, ep);
	}
}

int udc_init(struct udc_driver *driver)
{
	struct udc *udc = &_udc;
	u16 cfg;
	volatile int delay;

	if (!driver)
		return -EINVAL;

	udc->regs = (void __iomem *) UDC_BASE;
	udc->speed = USB_SPEED_UNKNOWN;
	udc->state = USB_STATE_NOTATTACHED;
	udc->driver = driver;

	/* enable clock */
	writel(UDC_CLKGEN_CLKSRC_EXT | UDC_CLKGEN_CLKDIV(0),
			udc->regs + UDC_CLKGEN);
	writel(UDC_CLKENB_PCLK_ALWAYS | UDC_CLKENB_CLKGENENB
			| UDC_CLKENB_USBD_ALWAYS, udc->regs + UDC_CLKENB);

	/* reset/enable PHY */
	cfg = readw(udc->regs + UDC_PCR);
	writew(cfg | UDC_PCR_PCE, udc->regs + UDC_PCR);
	for (delay = 0; delay < 100000; delay++);
	writew(cfg & ~UDC_PCR_PCE, udc->regs + UDC_PCR);

	udc_reconfig(udc);

	/* enable VBUS detection */
	writew(UDC_USER1_VBUSENB, udc->regs + UDC_USER1);

	if (udc->driver->init)
		udc->driver->init(udc);

	return 0;
}

