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

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "asm/types.h"
#include "asm/io.h"
#include "mach/mcus.h"
#include "mach/nand.h"

#include "nand.h"

static void __iomem *mcus_regs = (void __iomem *) MCUS_BASE;
static void __iomem *nand_regs = (void __iomem *) NAND_BASE;

static struct nand_chip nand_chips[2] = {{0}};
struct nand_chip *nand_chip = NULL;

static inline void nand_clear_intpend()
{
	u32 ctrl;
	ctrl = readl(mcus_regs + MCUS_NFCONTROL);
	ctrl |= MCUS_NFCONTROL_INTPEND;
	writel(ctrl, mcus_regs + MCUS_NFCONTROL);
}

static inline void nand_wait_intpend()
{
	u32 ctrl;
	while (1) {
		ctrl = readl(mcus_regs + MCUS_NFCONTROL);
		if (ctrl & MCUS_NFCONTROL_INTPEND)
			break;
	}
	nand_clear_intpend();
}

static inline void nand_wait_busy()
{
	u32 ctrl;
	while (1) {
		ctrl = readl(mcus_regs + MCUS_NFCONTROL);
		if (ctrl & MCUS_NFCONTROL_RNB)
			break;
	}
	nand_clear_intpend();
}

static inline int nand_wait_status()
{
	int status;
	nand_wait_intpend();
	writeb(NAND_CMD_STATUS, nand_regs + NAND_CMD);
	status = (int)readb(nand_regs + NAND_DATA);
	return status;
}

static void nand_command(unsigned int command, int column, int page_addr)
{
	if (!nand_chip || !nand_chip->info.known)
		return;

	nand_wait_busy();

	if (nand_chip->info.page_size <= 512) {
		if (command == NAND_CMD_SEQIN) {
			if (column >= nand_chip->info.page_size) {
				column -= nand_chip->info.page_size;
				writeb(NAND_CMD_READOOB, nand_regs + NAND_CMD);
			} else if (column < 256) {
				writeb(NAND_CMD_READ0, nand_regs + NAND_CMD);
			} else {
				column -= 256;
				writeb(NAND_CMD_READ1, nand_regs + NAND_CMD);
			}
		}
		writeb(command, nand_regs + NAND_CMD);

		if (column != -1)
			writeb(column, nand_regs + NAND_ADDR);
		
		if (page_addr != -1) {
			writeb(page_addr, nand_regs + NAND_ADDR);
			writeb(page_addr >> 8, nand_regs + NAND_ADDR);
			if (nand_chip->chip_bits > 25) /* > 32 MiB */
				writeb(page_addr >> 16, nand_regs + NAND_ADDR);
		}

		switch (command) {
		case NAND_CMD_PAGEPROG:
		case NAND_CMD_ERASE1:
		case NAND_CMD_ERASE2:
		case NAND_CMD_SEQIN:
		case NAND_CMD_STATUS:
			return;
		}
	} else {
		if (command == NAND_CMD_READOOB) {
			column += nand_chip->info.page_size;
			command = NAND_CMD_READ0;
		}

		writeb(command, nand_regs + NAND_CMD);

		if (column != -1 || page_addr != -1) {
			if (column != -1) {
				writeb(column, nand_regs + NAND_ADDR);
				writeb(column >> 8, nand_regs + NAND_ADDR);
			}
			if (page_addr != -1) {
				writeb(page_addr, nand_regs + NAND_ADDR);
				writeb(page_addr >> 8, nand_regs + NAND_ADDR);
				if (nand_chip->chip_bits > 27) /* > 128 MiB */
					writeb(page_addr >> 16,
							nand_regs + NAND_ADDR);
			}
		}

		switch (command) {
		case NAND_CMD_CACHEDPROG:
		case NAND_CMD_PAGEPROG:
		case NAND_CMD_ERASE1:
		case NAND_CMD_ERASE2:
		case NAND_CMD_SEQIN:
		case NAND_CMD_RNDIN:
		case NAND_CMD_STATUS:
			return;

		case NAND_CMD_RNDOUT:
			writeb(NAND_CMD_RNDOUTSTART, nand_regs + NAND_CMD);
			return;

		case NAND_CMD_READ0:
			writeb(NAND_CMD_READSTART, nand_regs + NAND_CMD);
		}
	}

	nand_wait_intpend();
	return;
}

/* works only with old 5-byte IDs */
static void nand_decode_ext_id()
{
	struct nand_info *info;

	if (!nand_chip)
		return;

	info = &nand_chip->info;

	info->known = true;
	info->badblock_pos = 0;
	info->num_planes = 1 << ((info->id[4] >> 2) & 0x3);

	nand_chip->page_bits = 10 + (info->id[3] & 0x3);
	nand_chip->block_bits = 16 + ((info->id[3] >> 4) & 0x3);
	nand_chip->chip_bits = 23 + ((info->id[4] >> 4) & 0x7) +
			((info->id[4] >> 2) & 0x3);

	info->oob_size = 8 << (nand_chip->page_bits - 9 +
			((info->id[3] >> 2) & 1));
}

static void nand_identify()
{
	int i;
	u8 *id = nand_chip->info.id;
	struct nand_info *info;

	if (!nand_chip)
		return;

	info = &nand_chip->info;

	/* read id */
	writeb(NAND_CMD_READID, nand_regs + NAND_CMD);
	writeb(0, nand_regs + NAND_ADDR);
	for (i = 0; i < 8; i++)
		info->id[i] = readb(nand_regs + NAND_DATA);

	/* detect if id or floating bus */
	for (i = 3; i < 8; i++)
		if (info->id[i] != info->id[2])
			info->present = true;

	if (!info->present)
		return;

	switch (info->id[0]) {
	case 0x2C: /* Micron */
		switch (info->id[1]) {
		case 0xDA: /* 256MB/2k */
			info->known = true;
			info->badblock_pos = 0;
			info->oob_size = 64;
			info->num_planes = 2;

			nand_chip->page_bits = 11;
			nand_chip->block_bits = 17;
			nand_chip->chip_bits = 28;
			break;
		}
		break;
	case 0xAD: /* Hynix */
		switch (info->id[1]) {
		case 0xD3: /* 1GB/2k */
			nand_decode_ext_id();
			break;
		}
		break;

	case 0xEC: /* Samsung */
		switch (id[1]) {
		case 0x76: /* 64MB/512 */
			info->known = true;
			info->badblock_pos = 5;
			info->oob_size = 16;
			info->num_planes = 2;

			nand_chip->page_bits = 9;
			nand_chip->block_bits = 14;
			nand_chip->chip_bits = 26;
			break;

		case 0xD5: /* 2GB/4k */
			nand_decode_ext_id();
			break;
		}
		break;
	}

	if (!info->known) {
		fputs("unknown NAND: ", stdout);
		for (i = 0; i < 6; i++)
			iprintf("%02x", info->id[i]);
		putchar('\n');
		return;
	}

	info->page_size = 1U << nand_chip->page_bits;
	info->block_size = 1U << (nand_chip->block_bits - 10);
	info->chip_size = 1U << (nand_chip->chip_bits - 20);
	nand_chip->num_blocks = 1U << (nand_chip->chip_bits -
			nand_chip->block_bits);
	nand_chip->pages_per_block = 1U << (nand_chip->block_bits -
			nand_chip->page_bits);
	nand_chip->read_size = info->page_size + info->oob_size;
}

static void nand_scan_bad()
{
	int block, page, plane;
	int byte_num, shift;
	u8 bad;

	if (!nand_chip || !nand_chip->info.known)
		return;

	for (block = 0; block < nand_chip->num_blocks; block++) {
		page = block * nand_chip->pages_per_block;
		for (plane = 0; plane < nand_chip->info.num_planes; plane++) {
			nand_command(NAND_CMD_READOOB,
					nand_chip->info.badblock_pos, page);
	  		bad = readb(nand_regs + NAND_DATA);
			if (bad != 0xFF) {
				byte_num = block >> 2;
				shift = (block & 0x3) * 2;
				nand_chip->bbt[byte_num] |= 0x3 << shift;
			}
			page++;
		}
	}
}

static bool nand_block_is_bad(int block)
{
	int byte_num, shift;
	u8 entry;

	if (!nand_chip || !nand_chip->info.known)
		return true;

	byte_num = block >> 2;
	shift = (block & 0x3) * 2;

	entry = (nand_chip->bbt[byte_num] >> shift) & 0x03;
	return (entry != 0);
}

void nand_init(void)
{
	int chipnr;

	nand_clear_intpend();
	for (chipnr = 0; chipnr < NAND_MAX_CHIPS; chipnr++) {
		nand_select_chip(chipnr);
		nand_chip->num = chipnr;

		nand_identify();
		if (nand_chip->info.known) {
			nand_command(NAND_CMD_RESET, -1, -1);
			nand_scan_bad();
		}
	}
	nand_select_chip(-1);
}

void nand_select_chip(int chipnr)
{
	u32 val;

	if (chipnr < 0 || chipnr >= NAND_MAX_CHIPS) {
		nand_chip = NULL;
		return;
	}

	nand_chip = &nand_chips[chipnr];

	val = readl(mcus_regs + MCUS_NFCONTROL) & ~MCUS_NFCONTROL_INTPEND;
	switch (chipnr) {
	case 0:
		val &= ~MCUS_NFCONTROL_NFBANK;
		break;
	case 1:
		val |= MCUS_NFCONTROL_NFBANK;
		break;
	}
	writel(val, mcus_regs + MCUS_NFCONTROL);
}

void nand_read_page(int page, void *mem, int size)
{
	u32 *p = mem;
	int i;

	if (!nand_chip || !nand_chip->info.known)
		return;

	nand_command(NAND_CMD_READ0, 0, page);
	for (i = 0; i < size; i += 4)
		*p++ = readl(nand_regs + NAND_DATA);
}

void nand_read_block(int block, void *mem)
{
	int first_page, offset;

	if (!nand_chip || !nand_chip->info.known)
		return;

	first_page = block * nand_chip->pages_per_block;
	for (offset = 0; offset < nand_chip->pages_per_block; offset++) {
		nand_read_page(first_page + offset, mem, nand_chip->read_size);
		mem += nand_chip->read_size;
	}
}

int nand_erase_block(int block)
{
	int page, status;

	if (!nand_chip || !nand_chip->info.known)
		return -1;

	if (nand_block_is_bad(block))
		return -1;

	page = block * nand_chip->pages_per_block;
	nand_command(NAND_CMD_ERASE1, -1, page);
	nand_command(NAND_CMD_ERASE2, -1, -1);

	status = nand_wait_status();
	if (status & NAND_STATUS_FAIL)
		iprintf("error erasing block %d\n", block);

	return status;
}

