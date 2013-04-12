#!/usr/bin/env python
# vim: ai ts=4 sts=4 et sw=4

import os
import sys
import struct
import time

root_dir = os.path.abspath(os.path.dirname(__file__))
pyusb_dir = os.path.join(root_dir, 'pyusb')
sys.path.insert(1, pyusb_dir)
if sys.platform == 'win32':
	import platform
	bits = platform.architecture()[0][:2]
	lib_dir = os.path.join(pyusb_dir, 'lib' + bits)
	os.environ['PATH'] = lib_dir + ';' + os.environ['PATH']

import usb.core
import usb.util


class UsbTool(object):
    def __init__(self, device):
        self.device = device

        self.device.set_configuration()
        config_descriptor = self.device.get_active_configuration()
        interface_number = config_descriptor[(0,0)].bInterfaceNumber
        alternate_setting = usb.control.get_interface(self.device,
                interface_number)

        interface_descriptor = usb.util.find_descriptor(
            config_descriptor,
            bInterfaceNumber = interface_number,
            bAlternateSetting = alternate_setting
        )

        self.tx_ep = usb.util.find_descriptor(
            interface_descriptor,
            custom_match = \
            lambda e:
                usb.util.endpoint_direction(e.bEndpointAddress) == \
                usb.util.ENDPOINT_OUT
        )

        self.rx_ep = usb.util.find_descriptor(
            interface_descriptor,
            custom_match = \
            lambda e:
                usb.util.endpoint_direction(e.bEndpointAddress) == \
                usb.util.ENDPOINT_IN
        )

    def write(self, data, chunk_size=64*1024):
        length = len(data)
        written = 0
        while written < length:
            chunk = data[written:written + chunk_size]
            written += self.tx_ep.write(chunk)

    def read(self, length=64*1024, convert=True):
        data = self.rx_ep.read(length)
        if convert:
            data = data.tostring()
        return data

    def command(self, *args):
        l = []
        for arg in args:
            if type(arg) is str:
                l.append(arg)
            elif type(arg) is int:
                l.append('%x' % arg)
            else:
                raise ValueError("invalid type")
        s = ' '.join(l)
        self.write(s)

    def get_buffer(self):
        return Buffer(self, 16*1024*1024)

    def get_nand(self, num):
        return NandChip(self, num)

class Buffer(object):
    def __init__(self, usbtool, size):
        self.usbtool = usbtool
        self.size = size

    def write(self, data, offset=0):
        offset &= ~1
        length = min(len(data), self.size - offset)
        remainder = length % 2
        if remainder:
            data += '\0' * remainder
            length += remainder
        self.usbtool.command('buffer write', offset, length)
        self.usbtool.write(data[:length])
        return length

    def read(self, length, offset=0):
        offset &= ~1
        length = min(length, self.size - offset)
        remainder = length % 2
        if remainder:
            length += remainder
        self.usbtool.command('buffer read', offset, length)
        data = self.usbtool.read(length)
        return data

class NandChip(object):
    selected = -1

    def __init__(self, usbtool, chip_num):
        self.usbtool = usbtool
        self.chip_num = chip_num

    def _select(self):
        if NandChip.selected != self.chip_num:
            self.usbtool.command('nand select', self.chip_num)

    def info(self):
        try:
            return self.info_dict
        except:
            pass

        self._select()
        self.usbtool.command('nand info')
        data = self.usbtool.read(20)
        keys = ['present', 'known', 'id', 'badblock_pos', 'num_planes',
                  'page_size', 'oob_size', 'block_size', 'chip_size']
        info = dict(zip(keys, struct.unpack('<??8sBBHHHH', data)))

        if info['known']:
            info['num_blocks'] = (info['chip_size'] * 1024) / info['block_size']
            info['num_pages'] = (info['block_size'] * 1024) / info['page_size']
            info['block_readsize'] = (info['block_size'] * 1024) + \
                    (info['oob_size'] * info['num_pages'])

        self.info_dict = info
        return info

    def bad_blocks(self):
        self._select()
        self.usbtool.command('nand bad')
        data = self.usbtool.read(1024, False)
        bad_blocks = []
        block_num = 0
        for byte in data:
            for i in xrange(4):
                if byte & (0x3 << (i * 2)):
                    bad_blocks.append(block_num)
                block_num += 1
        return bad_blocks

    def read_block(self, block_num, buffer_offset=0):
        self._select()
        self.usbtool.command('nand read', block_num, buffer_offset)

    def erase_block(self, block_num):
        self._select()
        self.usbtool.command('nand erase', block_num)
        data = self.usbtool.read(2)
        result = struct.unpack('<h', data)[0]
        if result == -1 or result & 1:
            return False
        return True

    def write_block(self, block_num, buffer_offset=0):
        self._select()
        self.usbtool.command('nand write', block_num, buffer_offset)
        data = self.usbtool.read(2)
        result = struct.unpack('<h', data)[0]
        if result == -1 or result & 1:
            return False
        return True

    def mark_block(self, block_num, mark):
        self._select()
        self.usbtool.command('nand mark', block_num, mark)

    def dump(self, filename):
        info = self.info()
        buf = self.usbtool.get_buffer()

        with open(filename + '.txt', 'w') as f:
            f.write('dump time:  %s\n' % time.asctime())
            f.write('chip num:   %d\n' % self.chip_num)
            f.write('page size:  %d B\n' % info['page_size'])
            f.write('oob size:   %d B\n' % info['oob_size'])
            f.write('block size: %d KB\n' % info['block_size'])
            f.write('chip size:  %d MB\n' % info['chip_size'])
            bad_blocks = ', '.join(map(str, self.bad_blocks()))
            if not bad_blocks:
                bad_blocks = '(none)'
            f.write('bad blocks: %s\n' % bad_blocks)

        with open(filename, 'wb') as f:
            print 'dumping NAND%d to %s' % (self.chip_num, filename)
            for block_num in xrange(info['num_blocks']):
                percent = (float(block_num) / info['num_blocks']) * 100
                sys.stdout.write('\x1b[2K\r%.1f%% complete' \
                        % percent)
                sys.stdout.flush()

                self.read_block(block_num)
                block_data = buf.read(info['block_readsize'])
                f.write(block_data)

            print '\x1b[2K\rcompleted'


if __name__ == '__main__':
    dev = usb.core.find(idVendor=0x0000, idProduct=0x7f21)

    if not dev:
        print "no device found"
        sys.exit(-1)

    usbtool = UsbTool(dev)

    for i in xrange(2):
        chip = usbtool.get_nand(i)
        info = chip.info()
        if not info['present']:
            continue

        if not info['known']:
            hex_id = ''
            for c in info['id']:
                hex_id += '%02x' % ord(c)
            print 'Unknown NAND with ID: %s' % hex_id
            continue

        print 'NAND%d: %d MB' % (i, info['chip_size'])


        chip.dump('test.bin')

"""
        buf = usbtool.get_buffer()
        with open('nc600-orig.bin', 'rb') as f:
            for block_num in xrange(info['num_blocks']):
                chip.mark_block(block_num,0)

                percent = (float(block_num) / info['num_blocks']) * 100
                sys.stdout.write('\x1b[2K\r%.1f%% complete' \
                        % percent)
                sys.stdout.flush()

                block = f.read(info['block_readsize'])
                if not chip.erase_block(block_num):
                    print "error erasing block %d" % block_num

                buf.write(block)
                if not chip.write_block(block_num):
                    print "error writing block %d" % block_num

        print '\x1b[2K\rcompleted'
"""
