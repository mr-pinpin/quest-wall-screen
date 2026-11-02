#!/usr/bin/env python3
"""Install/launch the room lab on the Quest through the mini's wireless ADB, or read its status.

  deploy.py install APK [--capture]   push, install, grant scene permission, launch
  deploy.py status                    print files/status.json and the event log
  deploy.py stop                      force-stop the app
  deploy.py shell 'CMD'               run a headset shell command, then print status
"""
import argparse
import base64
import os
import shlex
import subprocess

PKG='com.pinpin.wallscreen.roomlab'
SSH=['ssh','-C','-o','BatchMode=yes','-o','ConnectTimeout=15',os.environ.get('PINPIN_MINI_SSH','pinpin-mini')]  # Tailscale; the Mini is the house gateway since 2026-11-02
MINI=r'''
import sys,base64,pathlib,subprocess,time
home=pathlib.Path.home();adb=[str(home/'.local/share/pinpin-quest/platform-tools/adb'),'-s','127.0.0.1:26351']
def sh(c,t=40):r=subprocess.run(adb+['shell',c],capture_output=True,text=True,timeout=t);return (r.stdout+r.stderr).strip()
action,capture=sys.argv[1],sys.argv[2]=='1'
P='%s'
if action=='install':
    apk=home/'.local/share/pinpin-quest/wall-screen/room-lab.apk';apk.write_bytes(base64.b64decode(sys.stdin.read()))
    subprocess.run(adb+['push',str(apk),'/data/local/tmp/pinpin-room-lab.apk'],check=True,capture_output=True)
    # Root (Codex's recovery agent restores /debug_ramdisk/su after reboots) is optional: the adb shell user can install.
    root=sh('test -x /debug_ramdisk/su && echo yes')=='yes'
    su=(lambda c:"/debug_ramdisk/su -c '%%s'"%%c) if root else (lambda c:c)
    print('INSTALL',sh(su('pm install -r -g /data/local/tmp/pinpin-room-lab.apk')))
    print('GRANT',sh(su('pm grant %%s com.oculus.permission.USE_SCENE'%%P)) or 'ok','(root)' if root else '(no root)')
    sh('am force-stop '+P)
    print('LAUNCH',sh("am broadcast -a com.oculus.vrpowermanager.prox_close >/dev/null; input keyevent KEYCODE_WAKEUP; am start -n %%s/.MainActivity --ez capture %%s"%%(P,'true' if capture else 'false')))
    time.sleep(8)
if action=='shell':print('SHELL',sh(sys.stdin.read(),90))
if action=='stop':print(sh('am force-stop '+P) or 'stopped');sys.exit()
print('PID',sh('pidof '+P) or 'not running')
print('STATUS',sh('run-as %%s cat files/status.json'%%P))
print('EVENTS',sh('run-as %%s cat files/events.log'%%P))
print('LOGCAT',sh("logcat -d -t 400 | grep -E 'PinPinRoomLab|roomlab|AndroidRuntime|FATAL' | tail -25"))
'''%PKG

def main():
    p=argparse.ArgumentParser(description=__doc__,formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('action',choices=['install','status','stop','shell']);p.add_argument('apk',nargs='?');p.add_argument('--capture',action='store_true')
    a=p.parse_args()
    data=base64.b64encode(open(a.apk,'rb').read()).decode() if a.action=='install' else (a.apk or '')
    cmd='/usr/bin/python3 -c %s %s %s'%(shlex.quote(MINI),a.action,'1' if a.capture else '0')
    r=subprocess.run(SSH+[cmd],input=data,capture_output=True,text=True,timeout=180)
    print(r.stdout,r.stderr[-1500:])

if __name__=='__main__':main()
