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
        nand_select(tx_ep, rx_ep, chipnr)
        info = nand_info(tx_ep, rx_ep)
        if not info['present']:
            continue

        if not info['known']:
            from binascii import hexlify
            print 'Unknown NAND with ID: %s' % hexlify(info['id'])
            continue

        print 'Chip %d: %d MB NAND' % (chipnr, info['chip_size'])

        bad_blocks = nand_bad(tx_ep, rx_ep)
        print '  bad blocks: %s' % ', '.join(map(str, bad_blocks))

        filename = 'nand%d.bin' % chipnr
        with open(filename, 'wb') as f:
            for block_num in xrange(info['num_blocks']):
                percent = (float(block_num) / info['num_blocks']) * 100
                sys.stdout.write('\x1b[2K\r  dumping to "%s", %.1f%% complete' \
                        % (filename, percent))
                sys.stdout.flush()
                nand_read(tx_ep, rx_ep, block_num)
                block = buffer_read(tx_ep, rx_ep, info['block_readsize'])
                f.write(block)

            print '\x1b[2K\r  dumped to "%s"' % (filename)

def write_all(ep, data):
    length = len(data)
    written = 0
    while written < length:
        chunk = data[written:written+64*1024]
        written += ep.write(chunk)

def buffer_write(ep, data, offset=0):
    offset &= ~1
    length = min(len(data), BUFFER_SIZE - offset)
    remainder = length % 2
    if remainder:
        data += '\0' * remainder
        length += remainder
    command = 'buffer write %08x %08x' % (offset, length)
    write_all(ep, command)
    write_all(ep, data[:length])
    return length

def buffer_read(tx_ep, rx_ep, length, offset=0):
    offset &= ~1
    length = min(length, BUFFER_SIZE - offset)
    remainder = length % 2
    if remainder:
        length += remainder
    command = 'buffer read %08x %08x' % (offset, length)
    write_all(tx_ep, command)
    data = rx_ep.read(length).tostring()
    return data

def nand_select(tx_ep, rx_ep, chipnr):
    command = 'nand select %08x' % (chipnr & 0xffffffff)
    write_all(tx_ep, command)

def nand_info(tx_ep, rx_ep):
    command = 'nand info'
    write_all(tx_ep, command)
    data = rx_ep.read(20).tostring()
    keys = ['present', 'known', 'id', 'badblock_pos', 'num_planes',
              'page_size', 'oob_size', 'block_size', 'chip_size']
    info = dict(zip(keys, struct.unpack('<??8sBBHHHH', data)))

    if info['known']:
        info['num_blocks'] = (info['chip_size'] * 1024) / info['block_size']
        info['num_pages'] = (info['block_size'] * 1024) / info['page_size']
        info['block_readsize'] = (info['block_size'] * 1024) + \
                (info['oob_size'] * info['num_pages'])
    return info

def nand_bad(tx_ep, rx_ep):
    command = 'nand bad'
    write_all(tx_ep, command)
    data = rx_ep.read(1024)
    bad_blocks = []
    block = 0
    for byte in data:
        for i in xrange(4):
            if byte & (0x3 << (i * 2)):
                bad_blocks.append(block)
            block += 1
    return bad_blocks

def nand_read(tx_ep, rx_ep, block, offset=0):
    command = 'nand read %08x %08x' % (block, offset)
    write_all(tx_ep, command)

def nand_erase(tx_ep, rx_ep, block):
    command = 'nand erase %08x' % (block)
    write_all(tx_ep, command)
    data = rx_ep.read(2)
    result = struct.unpack('<h', data)[0]
    if result == -1 or result & 1:
        return False
    return True

if __name__ == '__main__':
    main()

