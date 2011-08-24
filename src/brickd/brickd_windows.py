# -*- coding: utf-8 -*-  
"""
brickd (Brick Daemon)
Copyright (C) 2011 Olaf Lüke <olaf@tinkerforge.com>
              2011 Bastian Nordmeyer <bastian@tinkerforge.com>

brickd_linux.py: Brick Daemon starting point for windows

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License 
as published by the Free Software Foundation; either version 2 
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
General Public License for more details.

You should have received a copy of the GNU General Public
License along with this program; if not, write to the
Free Software Foundation, Inc., 59 Temple Place - Suite 330,
Boston, MA 02111-1307, USA.
"""

# Window service code is based on (service):
# http://book.opensourceproject.org.cn/lamp/python/pythonwin/opensource/pythonwin32_snode143.html
# and (usb notifier):
# http://timgolden.me.uk/python/win32_how_do_i/detect-device-insertion.html

import servicemanager
import win32serviceutil
import win32service
import win32event
import win32gui
import win32gui_struct
struct = win32gui_struct.struct
pywintypes = win32gui_struct.pywintypes
import win32con
import pythoncom
from win32api import OutputDebugString

import logging
import signal
import config
from twisted.internet import reactor
from usb_notifier import USBNotifier
from brick_protocol import BrickProtocolFactory, exit_brickd

# USB Raw
GUID_DEVINTERFACE_USB_DEVICE = "{a5dcbf10-6530-11d2-901f-00c04fb951ed}" 
DBT_DEVICEARRIVAL = 0x8000
DBT_DEVICEREMOVECOMPLETE = 0x8004

logging.basicConfig(
    level = config.LOGGING_LEVEL,
    format = config.LOGGING_FORMAT,
    datefmt = config.LOGGING_DATEFMT
)

class BrickLoggingHandler(logging.Handler):
    def __init__(self):
        
        logging.Handler.__init__(self)

        self.loga = open('Log.txt','w')
        self.loga.write("init")
        self.loga.flush()            
        
        self.setFormatter(logging.Formatter(fmt=config.LOGGING_FORMAT,
                                            datefmt=config.LOGGING_DATEFMT))
        
    def emit(self, record):

        self.loga.write(self.format(record))
        self.loga.flush()
        
        OutputDebugString(self.format(record))
        if record.levelno in [logging.ERROR, logging.WARN]:
            servicemanager.LogMsg (
                servicemanager.EVENTLOG_ERROR_TYPE,
                0xF000,
                ("%s" % self.format(record),)#.getMessage(),)
            )

class BrickdWindows(win32serviceutil.ServiceFramework):
    _svc_name_ = 'Brickd'
    _svc_display_name_ = 'Brickd 1.0'
    _svc_description_ = 'brickd is a bridge between usb devices ("Bricks")' + \
                        ' and tcp/ip sockets. It can be used to read out ' + \
                        ' and control bricks.'
    
    def __init__(self, args):
        win32serviceutil.ServiceFramework.__init__(self, args)
        self.hWaitStop = win32event.CreateEvent(None, 0, 0, None)

        fil = win32gui_struct.PackDEV_BROADCAST_DEVICEINTERFACE(
            GUID_DEVINTERFACE_USB_DEVICE
        )


        self.hDevNotify = win32gui.RegisterDeviceNotification(
            self.ssh, # copy of the service status handle
            fil,
            win32con.DEVICE_NOTIFY_SERVICE_HANDLE
        )

    # Add to the list of controls already handled by the underlying
    # ServiceFramework class. We're only interested in device events
    def GetAcceptedControls(self):
        rc = win32serviceutil.ServiceFramework.GetAcceptedControls (self)
        rc |= win32service.SERVICE_CONTROL_DEVICEEVENT
        return rc

    def SvcOtherEx(self, control, event_type, data):
        if event_type == DBT_DEVICEARRIVAL:
            logging.info("New USB device")
            self.usb_notifier.notify_added()
        elif event_type == DBT_DEVICEREMOVECOMPLETE:
            logging.info("Removed USB device")
            self.usb_notifier.notify_removed()

    def SvcStop(self):
        reactor.stop()
        
        self.ReportServiceStatus(win32service.SERVICE_STOP_PENDING)
        win32event.SetEvent(self.hWaitStop)

    def SvcDoRun(self):
        logging.getLogger().addHandler(BrickLoggingHandler())
        logging.info("brickd started")

#        for non service test purposes only
#        signal.signal(signal.SIGINT, lambda s, f: exit_brickd(s, f, reactor)) 
#        signal.signal(signal.SIGTERM, lambda s, f: exit_brickd(s, f, reactor)) 
        
        self.usb_notifier = USBNotifier()
        reactor.listenTCP(config.PORT, BrickProtocolFactory())
        
        try:
            reactor.run(installSignalHandlers = True)
        except KeyboardInterrupt:
            reactor.stop()

        win32event.WaitForSingleObject(self.hWaitStop, win32event.INFINITE)
        
if __name__=='__main__':
    win32serviceutil.HandleCommandLine(BrickdWindows)
    #BrickdWindows(None).SvcDoRun()
