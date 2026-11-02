#!/usr/bin/env python3
"""Build moonlight-common-c (+enet, nanors) and mbedTLS crypto as arm64 static archives with the NDK.

Sources are fetched into the external cache (not vendored): moonlight-common-c at a pinned commit,
mbedTLS 3.6 LTS release tarball pinned by SHA-256.
"""
import argparse
import concurrent.futures
import hashlib
import pathlib
import subprocess
import tarfile
import urllib.request

MOONLIGHT = ('https://github.com/moonlight-stream/moonlight-common-c.git', 'f050b13')
MBEDTLS = ('https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-3.6.7/mbedtls-3.6.7.tar.bz2',
           'a7e8bcbec0e6f761b4af24f25677626b35f762f68eef79c08677a363212d11f6')

def run(args, **kw): subprocess.run([str(a) for a in args], check=True, **kw)

def fetch(cache):
    ml = cache / 'moonlight-common-c'
    if not ml.exists():
        run(['git', 'clone', '-q', '--recursive', MOONLIGHT[0], ml])
    run(['git', '-C', ml, 'checkout', '-q', MOONLIGHT[1]]); run(['git', '-C', ml, 'submodule', 'update', '-q', '--init', '--recursive'])
    tb = cache / 'mbedtls.tar.bz2'
    if not tb.exists(): tb.write_bytes(urllib.request.urlopen(MBEDTLS[0], timeout=60).read())
    if hashlib.sha256(tb.read_bytes()).hexdigest() != MBEDTLS[1]: raise SystemExit('mbedTLS checksum mismatch')
    mb = cache / 'mbedtls'
    if not (mb / 'library').exists():
        mb.mkdir(exist_ok=True)
        with tarfile.open(tb) as t:
            for m in t.getmembers():
                m.name = '/'.join(m.name.split('/')[1:])
                if m.name: t.extract(m, mb, filter='data')
    return ml, mb

def build(ndk, cache, out):
    ml, mb = fetch(cache)
    bin = ndk / 'toolchains/llvm/prebuilt/linux-x86_64/bin'
    cc, ar = bin / 'aarch64-linux-android26-clang', bin / 'llvm-ar'
    objs = out / 'obj'; objs.mkdir(parents=True, exist_ok=True)
    common = ['-O2', '-fPIC', '-std=c11', '-w']
    enet_defs = ['-DHAS_FCNTL=1', '-DHAS_IOCTL=1', '-DHAS_POLL=1', '-DHAS_GETADDRINFO=1', '-DHAS_GETNAMEINFO=1',
                 '-DHAS_INET_PTON=1', '-DHAS_INET_NTOP=1', '-DHAS_MSGHDR_FLAGS=1', '-DHAS_SOCKLEN_T=1']
    jobs = []
    for c in sorted((mb / 'library').glob('*.c')):
        jobs.append(('mbed', c, common + ['-I' + str(mb / 'include'), '-I' + str(mb / 'library')]))
    inc = ['-I' + str(ml / 'enet/include'), '-I' + str(ml / 'nanors'), '-I' + str(ml / 'nanors/deps/obl'), '-I' + str(mb / 'include')]
    for c in sorted((ml / 'src').glob('*.c')):
        jobs.append(('ml', c, common + inc + ['-DUSE_MBEDTLS', '-DNDEBUG'] + enet_defs))
    for c in ['callbacks', 'compress', 'host', 'list', 'packet', 'peer', 'protocol', 'unix']:
        jobs.append(('ml', ml / 'enet' / (c + '.c'), common + inc + enet_defs))
    for c in ['nanors/rs.c', 'nanors/deps/obl/oblas_common.c', 'nanors/deps/obl/oblas_lite.c']:
        jobs.append(('ml', ml / c, common + inc + ['-I' + str(ml / 'nanors/deps')]))
    def compile(job):
        lib, src, flags = job
        o = objs / (lib + '_' + src.parent.name + '_' + src.stem + '.o')
        run([cc] + flags + ['-c', src, '-o', o]); return lib, o
    with concurrent.futures.ThreadPoolExecutor(8) as ex: built = list(ex.map(compile, jobs))
    for lib, name in [('mbed', 'libmbedcrypto.a'), ('ml', 'libmoonlight.a')]:
        a = out / name
        if a.exists(): a.unlink()
        run([ar, 'rcs', a] + [o for l, o in built if l == lib])
    print('BUILT', out / 'libmoonlight.a', out / 'libmbedcrypto.a')
    return ml, mb

if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--ndk', type=pathlib.Path, required=True); p.add_argument('--cache', type=pathlib.Path, required=True)
    p.add_argument('--out', type=pathlib.Path, required=True)
    a = p.parse_args(); build(a.ndk, a.cache, a.out)
