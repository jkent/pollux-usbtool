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

#include "asm/types.h"
#include "asm/io.h"
#include "mach/mcus.h"
#include "mach/nand.h"

#include "nand.h"

static void *mcus = (void *) MCUS_BASE;
static void *nand = (void *) NAND_BASE;

static void wait(void)
{
	u32 n;

	while (!(readl(mcus + MCUS_NFCONTROL) & MCUS_NFCONTROL_INTPEND));

	n = readl(mcus + MCUS_NFCONTROL);
	n &= ~MCUS_NFCONTROL_INTPEND;
	writel(n, mcus + MCUS_NFCONTROL);
}

void nand_init(void)
{
	u32 n;

	n = readl(mcus + MCUS_NFCONTROL);
	n |= MCUS_NFCONTROL_INTPEND;
	writel(n, mcus + MCUS_NFCONTROL);
	
	nand_select(0);
	nand_reset();
}

void nand_select(int chipnr)
{
	u32 n;

	n = readl(mcus + MCUS_NFCONTROL);
	n &= ~MCUS_NFCONTROL_INTPEND;
	if (chipnr)
		n |= MCUS_NFCONTROL_NFBANK;
	else
		n &= ~MCUS_NFCONTROL_NFBANK;
	writel(n, mcus + MCUS_NFCONTROL);
}

void nand_reset(void)
{
	writeb(NAND_CMD_RESET, nand + NAND_CMD);
	wait();
}

void nand_readid(unsigned char *id)
{
	u32 n;

	writeb(NAND_CMD_READID, nand + NAND_CMD);
	writeb(0, nand + NAND_ADDR);

	*((u32 *)id) = readl(nand + NAND_DATA);
	*((u32 *)id + 1) = readl(nand + NAND_DATA);
}

