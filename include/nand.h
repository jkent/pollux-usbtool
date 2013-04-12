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

#ifndef _NAND_H
#define _NAND_H

#include <stdbool.h>

#include "asm/types.h"

#define NAND_MAX_CHIPS (2)
#define NAND_MAX_BLOCKS (4096)

struct nand_info {
	bool present;
	bool known;
	u8 id[8];
	u8 badblock_pos;
	u8 num_planes;
	u16 page_size;  /* B */
	u16 oob_size;   /* B */
	u16 block_size; /* KiB */
	u16 chip_size;  /* MiB */
};

struct nand_chip {
	u8 num;
	struct nand_info info;
	u8 page_bits;
	u8 block_bits;
	u16 chip_bits;
	u16 num_blocks;
	u16 pages_per_block;
	u16 read_size;  /* B */
	u8 bbt[NAND_MAX_BLOCKS / 4];
};

extern struct nand_chip *nand_chip;

void nand_init(void);
void nand_select_chip(int chipnr);
int nand_erase_block(int block);
void nand_read_page(int page, void *mem, int size);
int nand_write_page(int page, void *mem, int size);
void nand_read_block(int block, void *mem);
int nand_write_block(int block, void *mem);

#endif /* _NAND_H */

