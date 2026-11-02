#!/usr/bin/env python3
"""Build moonlight-common-c (+enet, nanors), mbedTLS crypto and libopus as arm64 static archives with the NDK.

Sources are fetched into the external cache (not vendored): moonlight-common-c at a pinned commit,
mbedTLS 3.6 LTS and libopus 1.5 release tarballs pinned by SHA-256 (opus matches Xiph's SHA256SUMS.txt).
"""
import argparse
import concurrent.futures
import hashlib
import pathlib
import re
import subprocess
import tarfile
import urllib.request

MOONLIGHT = ('https://github.com/moonlight-stream/moonlight-common-c.git', 'f050b13')
MBEDTLS = ('https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-3.6.7/mbedtls-3.6.7.tar.bz2',
           'a7e8bcbec0e6f761b4af24f25677626b35f762f68eef79c08677a363212d11f6')

OPUS = ('https://downloads.xiph.org/releases/opus/opus-1.5.2.tar.gz',
        '65c1d2f78b9f2fb20082c38cbe47c951ad5839345876e46941612ee87f9a7ce1')

def unpack(tb, dest):
    if (dest / 'include').exists(): return
    dest.mkdir(exist_ok=True)
    with tarfile.open(tb) as t:
        for m in t.getmembers():
            m.name = '/'.join(m.name.split('/')[1:])
            if m.name: t.extract(m, dest, filter='data')

def download(url, sha, path):
    if not path.exists(): path.write_bytes(urllib.request.urlopen(url, timeout=60).read())
    if hashlib.sha256(path.read_bytes()).hexdigest() != sha: raise SystemExit(path.name + ' checksum mismatch')

def mk_list(mk, name):
    """Source list `name` from one of opus's *_sources.mk files."""
    m = re.search(r'^' + name + r' = \\\n((?:.*\\\n)*.*)$', mk.read_text(), re.M)
    return m.group(1).replace('\\', ' ').split()

def run(args, **kw): subprocess.run([str(a) for a in args], check=True, **kw)

def fetch(cache):
    ml = cache / 'moonlight-common-c'
    if not ml.exists():
        run(['git', 'clone', '-q', '--recursive', MOONLIGHT[0], ml])
    run(['git', '-C', ml, 'checkout', '-q', MOONLIGHT[1]]); run(['git', '-C', ml, 'submodule', 'update', '-q', '--init', '--recursive'])
    tb = cache / 'mbedtls.tar.bz2'; download(*MBEDTLS, tb)
    mb = cache / 'mbedtls'; unpack(tb, mb)
    ot = cache / 'opus.tar.gz'; download(*OPUS, ot)
    op = cache / 'opus'; unpack(ot, op)
    return ml, mb, op

def build(ndk, cache, out):
    ml, mb, op = fetch(cache)
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
    # Float decoder, portable C (stereo 48 kHz decode is cheap; no RTCD/NEON variants needed).
    opus_src = (mk_list(op / 'celt_sources.mk', 'CELT_SOURCES') + mk_list(op / 'silk_sources.mk', 'SILK_SOURCES') +
                mk_list(op / 'silk_sources.mk', 'SILK_SOURCES_FLOAT') + mk_list(op / 'opus_sources.mk', 'OPUS_SOURCES') +
                mk_list(op / 'opus_sources.mk', 'OPUS_SOURCES_FLOAT'))
    opus_flags = common + ['-DOPUS_BUILD', '-DUSE_ALLOCA', '-DHAVE_LRINTF', '-DHAVE_LRINT', '-DNDEBUG',
                           '-DPACKAGE_VERSION="1.5.2"'] + ['-I' + str(op / d) for d in ['include', 'celt', 'silk', 'silk/float']]
    for c in opus_src: jobs.append(('opus', op / c, opus_flags))
    def compile(job):
        lib, src, flags = job
        o = objs / (lib + '_' + src.parent.parent.name + '_' + src.parent.name + '_' + src.stem + '.o')
        run([cc] + flags + ['-c', src, '-o', o]); return lib, o
    with concurrent.futures.ThreadPoolExecutor(8) as ex: built = list(ex.map(compile, jobs))
    for lib, name in [('mbed', 'libmbedcrypto.a'), ('ml', 'libmoonlight.a'), ('opus', 'libopus.a')]:
        a = out / name
        if a.exists(): a.unlink()
        run([ar, 'rcs', a] + [o for l, o in built if l == lib])
    print('BUILT', out / 'libmoonlight.a', out / 'libmbedcrypto.a', out / 'libopus.a')
    return ml, mb, op

if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--ndk', type=pathlib.Path, required=True); p.add_argument('--cache', type=pathlib.Path, required=True)
    p.add_argument('--out', type=pathlib.Path, required=True)
    a = p.parse_args(); build(a.ndk, a.cache, a.out)
