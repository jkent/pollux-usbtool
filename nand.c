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

char nand_bbt[2][1024] = {{0}};


static void nand_wait(void)
{
	u32 val;

	while (!(readl(mcus_regs + MCUS_NFCONTROL) & MCUS_NFCONTROL_INTPEND));

	val = readl(mcus_regs + MCUS_NFCONTROL);
	val |= MCUS_NFCONTROL_INTPEND;
	writel(val, mcus_regs + MCUS_NFCONTROL);
}

static int nand_wait_status()
{
	int status = 0;

	writeb(NAND_CMD_STATUS, nand_regs + NAND_CMD);
	while (!(status & NAND_STATUS_READY))
		status = (int)readb(nand_regs + NAND_DATA);

	return status;
}

static void nand_command(unsigned int command, int column, int page_addr)
{
	if (!nand_chip || !nand_chip->valid)
		return;

	if (nand_chip->page_size <= 512) {
		if (command == NAND_CMD_SEQIN) {
			if (column >= nand_chip->page_size) {
				column -= nand_chip->page_size;
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
			if (nand_chip->addr_cycles >= 4)
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
			column += nand_chip->page_size;
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
				if (nand_chip->addr_cycles >= 5)
					writeb(page_addr >> 16, nand_regs + NAND_ADDR);
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

	nand_wait();

	return;
}

/* works only with old 5-byte IDs */
static void nand_decode_ext_id()
{
	int n;
	u8 *id = nand_chip->id;

	if (!nand_chip)
		return;

	nand_chip->badblockpos = 0;
	nand_chip->addr_cycles = 0;
	nand_chip->page_shift = 10 + (id[3] & 0x3);
	nand_chip->page_size = 1 << nand_chip->page_shift;
	nand_chip->block_shift = 16 + ((id[3] >> 4) & 0x3);
	nand_chip->block_size = 1 << nand_chip->block_shift;
	nand_chip->pagemask = (1 << (nand_chip->block_shift - nand_chip->page_shift)) - 1;
	nand_chip->oob_size = (nand_chip->page_size / 0x200) *
			(8 << ((id[3] >> 2) & 1));
	nand_chip->planes = 1 << ((id[4] >> 2) & 0x3);
	nand_chip->plane_size = 0x2000 << ((id[4] >> 4) & 0x7);

	/* columns */
	nand_chip->addr_cycles = (nand_chip->page_shift + 7) / 8;

	/* rows */
	n = 1 << (nand_chip->block_shift - nand_chip->page_shift); /* pages per block */
	n *= (nand_chip->planes * nand_chip->plane_size) /
			(nand_chip->block_size >> 10); /* blocks per device */
	while (n) {
		nand_chip->addr_cycles++;
		n >>= 8;
	}
}

static int nand_identify()
{
	int i;
	u8 *id = nand_chip->id;

	if (!nand_chip)
		return 0;

	writeb(NAND_CMD_READID, nand_regs + NAND_CMD);
	writeb(0, nand_regs + NAND_ADDR);

	for (i = 0; i < 8; i++)
		id[i] = readb(nand_regs + NAND_DATA);

	/* detect if resistors are driving bus */
	for (i = 3; i < 8; i++) {
		if (id[i] != id[2])
			break;
	}
	if (i == 8)
		return 0;

	switch (id[0]) {
	case 0xAD: /* Hynix */
		switch (id[1]) {
		case 0xD3: /* 1GB/2k */
			nand_decode_ext_id();
			return 1;
		}
		break;

	case 0xEC: /* Samsung */
		switch (id[1]) {
		case 0x76: /* 64MB/512 */
			nand_chip->badblockpos = 5;
			nand_chip->addr_cycles = 1 + 3;
			nand_chip->page_shift  = 9;
			nand_chip->page_size   = 1 << nand_chip->page_shift;
			nand_chip->block_shift = 14;
			nand_chip->block_size  = 1 << nand_chip->block_shift;
			nand_chip->pagemask    = (1 << (nand_chip->block_shift - nand_chip->page_shift)) - 1;
			nand_chip->oob_size    = 16;
			nand_chip->planes      = 2;
			nand_chip->plane_size  = 32768;
			return 1;

		case 0xD5: /* 2GB/4k */
			nand_decode_ext_id();
			return 1;
		}
		break;
	}

	iprintf("unknown chip: %02x%02x%02x%02x%02x%02x%02x%02x\n",
		id[0], id[1], id[2], id[3], id[4], id[5], id[6], id[7]);
	return 0;
}

static void nand_scan_bad()
{
	int block, page, plane;
	u8 bad;
	char *bbt = nand_bbt[nand_chip->num];
	int num_blocks = (nand_chip->plane_size * nand_chip->planes) /
			(nand_chip->block_size >> 10);

	if (!nand_chip || !nand_chip->valid)
		return -1;

	for (block = 0; block < num_blocks; block++) {
		page = block << (nand_chip->block_shift - nand_chip->page_shift);
		for (plane = 0; plane < nand_chip->planes; plane++) {
			nand_command(NAND_CMD_READOOB, nand_chip->badblockpos, page);		
	  		bad = readb(nand_regs + NAND_DATA);
			if (bad != 0xFF)
				bbt[block / 4] |= 0x1 << ((block & 0x3) * 2);
			page++;
		}
	}
}

void nand_init(void)
{
	int chipnr;

	for (chipnr = 0; chipnr < NAND_MAX_CHIPS; chipnr++) {
		nand_select_chip(chipnr);
		nand_chip->num = chipnr;
		nand_chip->valid = false;

		if (nand_identify()) {
			nand_chip->valid = true;
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

int nand_erase(u64 ofs)
{
	int page;

	if (!nand_chip || !nand_chip->valid)
		return -1;

	page = (int)(ofs >> nand_chip->page_shift) & ~nand_chip->pagemask;

	nand_command(NAND_CMD_ERASE1, -1, page);
	nand_command(NAND_CMD_ERASE2, -1, -1);

	return nand_wait_status();
}

