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

#include "asm/io.h"
#include "mach/clkpwr.h"
#include "mach/mcuy.h"
#include "uart.h"

#include "udc/udc.h"
#include "udc/stream_driver.h"


static void ddr_init(void)
{
	u16 cfg;
	void __iomem *mcuy = (void __iomem *) MCUY_BASE;

	cfg = readw(mcuy + MCUY_CONTROL);
	cfg &= 0xFF3E;
	cfg |= 1 << 6;   // RD LAT
	writew(cfg, mcuy + MCUY_CONTROL);

	cfg = readw(mcuy + MCUY_TIME0);
	cfg &= 0xF0FF;
	cfg |=  0 << 12; // TMRD
	cfg |=  2 <<  4; // TRP
	cfg |=  2 <<  0; // TRCD
	writew(cfg, mcuy + MCUY_TIME0);

	cfg = readw(mcuy + MCUY_TIME1);
	cfg &= 0x4000;
	cfg |=  1 << 12; // CAS LAT
	cfg |=  8 <<  8; // TRC
	cfg |=  5 <<  4; // TRAS
	cfg |=  2 <<  0; // TWR
	writew(cfg, mcuy + MCUY_TIME1);

	writew(1035, mcuy + MCUY_REFRESH);

	writew(0x0000, mcuy + MCUY_CLKDELAY);
	writew(0x0000, mcuy + MCUY_DQSOUTDELAY);
	writew(0x0011, mcuy + MCUY_DQSINDELAY);

	cfg = readw(mcuy + MCUY_TIME1);
	cfg |= (1 << 15);
	writew(cfg, mcuy + MCUY_TIME1);

	while (readw(mcuy + MCUY_TIME1) & 0x8000);
}

static void pll_init(void)
{
	u32 cfg;
	void __iomem *clkpwr = (void __iomem *) CLKPWR_BASE;

	cfg = readl(clkpwr + CLKPWR_BUSDRV);
	cfg |= 0xFF000000;
	writel(cfg, clkpwr + CLKPWR_BUSDRV);
	
	/* 533 MHz PLL */
	writel(CLKPWR_PLL(24, 948, 1), clkpwr + CLKPWR_PLL0);

	/* 533 MHz CPU, 178 MHz AHB, 89 MHz APB */
	cfg = readl(clkpwr + CLKPWR_CLKMODE);
	cfg &= ~(CLKPWR_CLKMODE_BCLKDIV_MASK | CLKPWR_CLKMODE_AHBDIV_MASK
			| CLKPWR_CLKMODE_CPUDIV_MASK);
	cfg |= CLKPWR_CLKMODE_BCLKDIV(2) | CLKPWR_CLKMODE_AHBDIV(2)
			| CLKPWR_CLKMODE_CPUDIV(1);
	writel(cfg, clkpwr + CLKPWR_CLKMODE);

	cfg = readl(clkpwr + CLKPWR_PWRMODE);
	cfg |= CLKPWR_PWRMODE_CHGPLL;
	writel(cfg, clkpwr + CLKPWR_PWRMODE);

	while (readl(clkpwr + CLKPWR_PWRMODE) & CLKPWR_PWRMODE_CHGPLL);
}

int main(void)
{
	uart_init();

	fputs("init: ddr", stdout); fflush(stdout);
	ddr_init();

	fputs(" pll", stdout); fflush(stdout);
	pll_init();

	fputs(" udc", stdout); fflush(stdout);
	udc_init(&udc_stream_driver);

	puts("\nready!");
	
	while (1) {
		udc_task();
	}

	return 0;
}

