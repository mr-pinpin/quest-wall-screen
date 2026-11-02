#!/usr/bin/env python3
"""Build a read-only native OpenXR runtime probe; outputs stay outside Git."""
import argparse
import hashlib
import pathlib
import subprocess
import urllib.request
import zipfile

SRC=pathlib.Path(__file__).resolve().parent
URL='https://repo.maven.apache.org/maven2/org/khronos/openxr/openxr_loader_for_android/1.0.34/openxr_loader_for_android-1.0.34.aar'
SHA='c8b50603ad3b81756c6bd203ab9c9d19341326e14f6cbae87ee894137ee5e256'

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
    sdk=out/'sdk'
    with zipfile.ZipFile(aar) as z:
        z.extractall(sdk);assets=out/'assets';assets.mkdir(exist_ok=True)
        (assets/'OpenXR-LICENSE.txt').write_bytes(z.read('META-INF/LICENSE'))
    tool=args.ndk/'toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android26-clang++'
    library=sdk/'prefab/modules/openxr_loader/libs/android.arm64-v8a/libopenxr_loader.so'
    def run(values):subprocess.run([str(v) for v in values],check=True)
    run([tool,'-shared','-fPIC','-O2','-std=c++17','-static-libstdc++','-I'+str(sdk/'prefab/modules/headers/include'),SRC/'probe.cpp',library,'-Wl,-soname,libpinpin_wall_probe.so','-o',out/'libpinpin_wall_probe.so'])
    classes=out/'classes';classes.mkdir(exist_ok=True)
    android='/usr/lib/android-sdk/platforms/android-23/android.jar'
    run(['javac','-source','8','-target','8','-classpath',android,'-d',classes,SRC/'src/com/pinpin/wallscreen/probe/MainActivity.java'])
    run(['dalvik-exchange','--dex','--output='+str(out/'classes.dex'),classes])
    run(['aapt','package','-f','-M',SRC/'AndroidManifest.xml','-I',android,'-A',assets,'-F',out/'unsigned.apk'])
    with zipfile.ZipFile(out/'unsigned.apk','a') as z:
        z.write(out/'classes.dex','classes.dex');z.write(out/'libpinpin_wall_probe.so','lib/arm64-v8a/libpinpin_wall_probe.so');z.write(library,'lib/arm64-v8a/libopenxr_loader.so')
    run(['apksigner','sign','--ks',args.key_store,'--ks-key-alias','homedev','--ks-pass','file:'+str(args.key_pass),'--out',out/'wall-capabilities.apk',out/'unsigned.apk'])
    run(['apksigner','verify',out/'wall-capabilities.apk'])
    print('BUILT',out/'wall-capabilities.apk')

if __name__=='__main__':main()
