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

#include "udc.h"
#include "nand.h"
#include "usbtool_udc_driver.h"

int main(void)
{
	int i;

	nand_init();
	for (i = 0; i < NAND_MAX_CHIPS; i++) {
		nand_select_chip(i);
		if (nand_chip->info.known)
			iprintf("%d MB NAND\n",	nand_chip->info.chip_size);
	}
	nand_select_chip(-1);

	udc_init(&usbtool_udc_driver);
		
	fputs("\nReady!\n", stdout);

	while (1) {
		udc_task();
	}

	return 0;
}

