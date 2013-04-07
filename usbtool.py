#!/usr/bin/env python
# vim: ai ts=4 sts=4 et sw=4

import sys, os
import time

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
   
    """
    test_size = 1024

    for i in xrange(1,256):
        test_size = i * 4

        print "Generating"
        data_out = os.urandom(test_size) * 1024
        buffer_write(tx_ep, data_out)
        data_in = buffer_read(tx_ep, rx_ep, test_size * 1024)
        if data_in != data_out:
            print "error!"
            print len(data_out), repr(data_out)
            print len(data_in), repr(data_in)
            break
    """

    #test_txspeed(ep)

    import binascii

    nand_select(tx_ep, 0)
    _id = nand_readid(tx_ep, rx_ep)
    print "NAND(0): %s" % binascii.hexlify(_id)

    nand_select(tx_ep, 1)
    _id = nand_readid(tx_ep, rx_ep)
    print "NAND(1): %s" % binascii.hexlify(_id)


def write_all(ep, data):
    length = len(data)
    written = 0
    while written < length:
        chunk = data[written:written+64*1024]
        written += ep.write(chunk)

def read_all(ep, length):
    data = ep.read(length)
    data = ''.join(map(chr, data))
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

def nand_select(tx_ep, chipnr):
    command = 'nand select %08x' % (chipnr)
    write_all(tx_ep, command)

def nand_readid(tx_ep, rx_ep):
    command = 'nand id'
    write_all(tx_ep, command)
    data = read_all(rx_ep, 8)
    return data

def test_txspeed(ep, megabytes=4):
    test_size = int(megabytes * 1024 * 1024)
    chunk_size = 512 * 128

    data = os.urandom(64)
    #data = ''.join([chr(x) for x in xrange(0, 256)])
    data *= chunk_size / len(data)

    start_time = time.time()

    for chunk in xrange(test_size / chunk_size):
        try:
            ep.write(data)
            #sys.stdout.write('.')
            #sys.stdout.flush()
        except:
            print 'error sending chunk %d' % chunk
            return

    delta_time = time.time() - start_time

    bytes_per_second = test_size / delta_time
    print "\n%.2f seconds elapsed, %.2f KB/s" % (delta_time, bytes_per_second / 1024)


if __name__ == '__main__':
    main()

