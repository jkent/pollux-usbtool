#!/usr/bin/env python
# vim: ai ts=4 sts=4 et sw=4

import sys, os
import time
import collections, struct

BUFFER_SIZE = 16*1024*1024

usbmon_dir = os.path.dirname(os.path.abspath(__file__))
pyusb_dir = os.path.join(usbmon_dir, 'pyusb')
sys.path.insert(1, pyusb_dir)
if sys.platform == 'win32':
	import platform
	bits = platform.architecture()[0][:2]
	lib_dir = os.path.join(pyusb_dir, 'lib' + bits)
	os.environ['PATH'] = lib_dir + ';' + os.environ['PATH']

import usb.core
import usb.util

def main():
    dev = usb.core.find(idVendor=0x0000, idProduct=0x7f21)

    if dev is None:
        raise ValueError('Device not found')

    dev.set_configuration()
    cfg = dev.get_active_configuration()
    interface_number = cfg[(0,0)].bInterfaceNumber
    alternate_setting = usb.control.get_interface(dev, interface_number)
    intf = usb.util.find_descriptor(
        cfg, bInterfaceNumber = interface_number,
        bAlternateSetting = alternate_setting
    )

    tx_ep = usb.util.find_descriptor(
        intf,
        # match the first OUT endpoint
        custom_match = \
        lambda e: \
            usb.util.endpoint_direction(e.bEndpointAddress) == \
            usb.util.ENDPOINT_OUT
    )

    rx_ep = usb.util.find_descriptor(
        intf,
        # match the first IN endpoint
        custom_match = \
        lambda e: \
            usb.util.endpoint_direction(e.bEndpointAddress) == \
            usb.util.ENDPOINT_IN
    )

    for chipnr in xrange(0, 2):
        info = nand_info(tx_ep, rx_ep, chipnr)
        if not info.valid:
            continue
        print "Chip %d: %d MB NAND" % (chipnr, (info.plane_size * info.planes) / 1024)

        last_bad = None    
        for i in xrange(0, 4096):
            if nand_block_bad(tx_ep, rx_ep, chipnr, info.block_size*i):
                if last_bad == None:
                    last_bad = i
            else:
                if last_bad != None:
                    if last_bad == i - 1:
                        print "  block %d is bad" % last_bad
                    else:
                        print "  blocks %d - %d is bad" % (last_bad, i-1)
                    last_bad = None
        if last_bad != None:
            if last_bad == i - 1:
                print "  block %d is bad" % last_bad
            else:
                print "  blocks %d - %d are bad" % (last_bad, i)
            last_bad = None


def write_all(ep, data):
    length = len(data)
    written = 0
    while written < length:
        chunk = data[written:written+64*1024]
        written += ep.write(chunk)

def read_all(ep, length):
    data = ep.read(length).tostring()
    return data

def buffer_write(ep, data, offset=0):
    offset &= ~1
    length = min(len(data), BUFFER_SIZE - offset)
    remainder = length % 2
    if remainder:
        data += '\0' * remainder
        length += remainder
    command = 'buffer write %08x %08x' % (offset, length)
    sys.stdout.write(command); sys.stdout.flush()
    write_all(ep, command)
    write_all(ep, data[:length])
    sys.stdout.write('\n')
    return length

def buffer_read(tx_ep, rx_ep, length, offset=0):
    offset &= ~1
    length = min(length, BUFFER_SIZE - offset)
    remainder = length % 2
    if remainder:
        length += remainder
    command = 'buffer read %08x %08x' % (offset, length)
    sys.stdout.write(command); sys.stdout.flush()
    write_all(tx_ep, command)
    data = read_all(rx_ep, length)
    sys.stdout.write('\n')
    return data

def nand_info(tx_ep, rx_ep, chipnr):
    command = 'nand info %08x' % (chipnr)
    write_all(tx_ep, command)
    data = rx_ep.read(512).tostring()
    NandChip = collections.namedtuple('NandChip', 'num valid badblockpos addr_cycles id page_shift page_size block_shift block_size pagemask oob_size planes plane_size')
    return NandChip._make(struct.unpack('<B?BB8sIIIIIIII', data))

def nand_block_bad(tx_ep, rx_ep, chipnr, ofs):
    command = 'nand bad %08x %08x %08x' % (chipnr, ofs >> 32, ofs & 0xFFFFFFFF)
    write_all(tx_ep, command)
    data = rx_ep.read(1)
    return (data[0] != 0)

if __name__ == '__main__':
    main()

