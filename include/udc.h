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

#ifndef __UDC_H__
#define __UDC_H__

#include <stdbool.h>

#include "asm/types.h"
#include "list.h"

#include "usb/ch9.h"

#define NUM_ENDPOINTS 3

struct udc;
struct udc_ep;
struct udc_ep_ops;
struct udc_req;
struct udc_driver;

enum ep0_state {
	WAIT_FOR_SETUP = 0,
	DATA_STATE_XMIT,
	DATA_STATE_RECV,
};

struct udc_driver {
	void			(*init)(struct udc *udc);
	int			(*setup)(struct udc *udc,
					struct usb_ctrlrequest *ctrl);
	void			(*vbuson)(struct udc *udc);
	void			(*vbusoff)(struct udc *udc);
};

struct udc_req {
	void			*buf;
	unsigned int		length;
	unsigned int		actual;
	bool			zero;
	void			(*complete)(struct udc_ep *ep,
					struct udc_req *req);
	int			status;
	struct list_head	queue;
};

struct udc_ep_ops {
	int			(*enable)(struct udc_ep *ep,
				const struct usb_endpoint_descriptor *desc);
	int			(*disable)(struct udc_ep *ep);
	struct udc_req *	(*alloc_req)(struct udc_ep *ep);
	void			(*free_req)(struct udc_ep *ep,
					struct udc_req *req);
	int			(*queue)(struct udc_ep *ep,
					struct udc_req *req);
	int			(*dequeue)(struct udc_ep *ep,
					struct udc_req *req);
	int			(*set_halt)(struct udc_ep *ep, bool halt);
	void 			(*fifo_flush)(struct udc_ep *ep);
};

struct udc_ep {
	void __iomem		*fifo;
	u8			address;
	u8			stopped;
	u16			maxpacket;
	struct udc		*dev;
	struct udc_ep_ops	*ops;
	struct list_head	queue;
};

struct udc {
	void __iomem		*regs;
	enum ep0_state		ep0_state;
	u8			speed;
	u8			config;
	u8			state;
	struct udc_driver	*driver;
	struct udc_ep		ep[NUM_ENDPOINTS];
};

int udc_init(struct udc_driver *driver);
void udc_task(void);

#endif /* __UDC_H__  */

