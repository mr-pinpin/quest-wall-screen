#!/usr/bin/env python3
"""Build the native room lab APK; SDK downloads and outputs stay outside Git."""
import argparse
import hashlib
import pathlib
import subprocess
import urllib.request
import zipfile

SRC=pathlib.Path(__file__).resolve().parent
URL='https://repo.maven.apache.org/maven2/org/khronos/openxr/openxr_loader_for_android/1.1.41/openxr_loader_for_android-1.1.41.aar'
SHA='151e96c6a8afec322769fb163f6311addda116719fc01d55cfbd268e7a67b00f'

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--ndk',type=pathlib.Path,required=True)
    p.add_argument('--out',type=pathlib.Path,required=True)
    p.add_argument('--key-store',type=pathlib.Path,required=True)
    p.add_argument('--key-pass',type=pathlib.Path,required=True)
    args=p.parse_args();out=args.out.resolve();out.mkdir(parents=True,exist_ok=True)
    aar=out/'openxr.aar'
    if not aar.exists():aar.write_bytes(urllib.request.urlopen(URL,timeout=30).read())
    if hashlib.sha256(aar.read_bytes()).hexdigest()!=SHA:raise SystemExit('OpenXR loader checksum mismatch')
    sdk=out/'sdk';assets=out/'assets';assets.mkdir(exist_ok=True)
    with zipfile.ZipFile(aar) as z:
        z.extractall(sdk);(assets/'OpenXR-LICENSE.txt').write_bytes(z.read('META-INF/LICENSE'))
    tool=args.ndk/'toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android26-clang++'
    library=sdk/'prefab/modules/openxr_loader/libs/android.arm64-v8a/libopenxr_loader.so'
    def run(values):subprocess.run([str(v) for v in values],check=True)
    run([tool,'-shared','-fPIC','-O2','-std=c++17','-Wall','-Wno-missing-braces','-static-libstdc++','-I'+str(sdk/'prefab/modules/headers/include'),
         SRC/'room_lab.cpp',library,'-lEGL','-lGLESv3','-llog','-landroid','-lmediandk','-lnativewindow','-Wl,-soname,libpinpin_room_lab.so','-o',out/'libpinpin_room_lab.so'])
    classes=out/'classes';classes.mkdir(exist_ok=True)
    android='/usr/lib/android-sdk/platforms/android-23/android.jar'
    run(['javac','-source','8','-target','8','-classpath',android,'-d',classes,SRC/'src/com/pinpin/wallscreen/roomlab/MainActivity.java'])
    run(['dalvik-exchange','--dex','--output='+str(out/'classes.dex'),classes])
    run(['aapt','package','-f','-M',SRC/'AndroidManifest.xml','-I',android,'-A',assets,'-F',out/'unsigned.apk'])
    with zipfile.ZipFile(out/'unsigned.apk','a') as z:
        z.write(out/'classes.dex','classes.dex');z.write(out/'libpinpin_room_lab.so','lib/arm64-v8a/libpinpin_room_lab.so');z.write(library,'lib/arm64-v8a/libopenxr_loader.so')
    run(['apksigner','sign','--ks',args.key_store,'--ks-key-alias','homedev','--ks-pass','file:'+str(args.key_pass),'--out',out/'room-lab.apk',out/'unsigned.apk'])
    run(['apksigner','verify',out/'room-lab.apk'])
    print('BUILT',out/'room-lab.apk')

if __name__=='__main__':main()
