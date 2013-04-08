/*
 * Copyright (C) 2013 Jeff Kent <jeff@jkent.net>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
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

#ifndef _NAND_H_
#define _NAND_H_

#include <stdbool.h>

#include "asm/types.h"

#define NAND_MAX_CHIPS		(2)

#define BADBLOCK_1ST_PG		(0x01)
#define BADBLOCK_LAST_PG	(0x04)
#define BADBLOCK_1ST_BYTE	(0x10)
#define BADBLOCK_6TH_BYTE	(0x20)

struct nand_chip {
	u8 num;
	bool valid;
	u8 badblock_type;
	u8 addr_cycles;
	u8 id[8];
	u32 page_size;     /* bytes */
	u32 block_size;    /* kilobytes */
	u32 oob_size;      /* bytes */
	u32 planes;        /* count */
	u32 plane_size;    /* kilobytes */
};

extern struct nand_chip nand_chips[2];

void nand_init(void);
void nand_select(struct nand_chip *chip);

#endif /* _NAND_H_ */

