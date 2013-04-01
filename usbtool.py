#!/usr/bin/env python
# vim: ai ts=4 sts=4 et sw=4

import sys, os
import time

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

    ep = usb.util.find_descriptor(
        intf,
        # match the first OUT endpoint
        custom_match = \
        lambda e: \
            usb.util.endpoint_direction(e.bEndpointAddress) == \
            usb.util.ENDPOINT_OUT
    )

    assert ep is not None

    #ep.write(' ' * 257);

    #data = ''.join([chr(x) for x in xrange(0, 256)])
    #data *= 4
    #ep.write(data)
    #if not len(data) % 512:
    #    ep.write('')

    test_txspeed(ep)
    

def test_txspeed(ep, megabytes=4):
    test_size = int(megabytes * 1024 * 1024)
    chunk_size = 64 * 1024

    #data = os.urandom(64)
    data = ''.join([chr(x) for x in xrange(0, 256)])
    data *= chunk_size / len(data)

    start_time = time.time()

    for chunk in xrange(test_size / chunk_size):
        try:
            ep.write(data)
            #if not chunk_size % 512:
            #    ep.write('')
            sys.stdout.write('.')
            sys.stdout.flush()
        except:
            print 'error sending chunk %d' % chunk
            return

    delta_time = time.time() - start_time

    bytes_per_second = test_size / delta_time
    print "\n%.2f seconds elapsed, %.2f KB/s" % (delta_time, bytes_per_second / 1024)


if __name__ == '__main__':
    main()

