#!/usr/bin/env python3
"""Prepare a glTF sample model for the headset (docs/3d-content.md): geometry-only GLB + a KTX2 base-colour
texture whose mip levels are stored smallest first, so the headset can show the coarse level as soon as its
bytes arrive and swap in finer ones as the download continues.

    python3 assets/make_asset.py IN.glb OUTDIR [--size 1024] [--format astc6x6|astc4x4|astc8x8|rgba8] [--astcenc PATH]
    (needs Pillow; ASTC needs ARM's astcenc: github.com/ARM-software/astc-encoder releases)

Output: OUTDIR/model.glb (the first mesh's first primitive, its node, a material whose baseColorTexture points at
basecolor.ktx2) and OUTDIR/basecolor.ktx2, full mip chain, no supercompression, uploaded as is (no transcoder):
ASTC 6x6 sRGB by default (3.56 bits/pixel, native on the Quest's Adreno: ~9x smaller than RGBA8 at 32), or RGBA8. Source models: Khronos glTF-Sample-Assets
(DamagedHelmet: CC BY 4.0, theblueturtle_; keep the attribution with the files).
"""
import io, json, os, struct, subprocess, sys, tempfile, pathlib
from PIL import Image

ASTC = {'astc4x4': (4, 158), 'astc6x6': (6, 166), 'astc8x8': (8, 172)}  # block size, VK_FORMAT_ASTC_NxN_SRGB_BLOCK

def read_glb(path):
    d = pathlib.Path(path).read_bytes()
    assert d[:4] == b'glTF', 'not a GLB'
    p, js, bin_ = 12, None, b''
    while p < len(d):
        ln, ty = struct.unpack_from('<II', d, p)
        chunk = d[p + 8:p + 8 + ln]
        if ty == 0x4E4F534A: js = json.loads(chunk)
        elif ty == 0x004E4942: bin_ = chunk
        p += 8 + ln
    return js, bin_

def astc_encode(img, block, astcenc):
    # One mip level through astcenc (sRGB, medium quality); its .astc output is a 16-byte header + the blocks.
    with tempfile.TemporaryDirectory() as t:
        src, dst = os.path.join(t, 'in.png'), os.path.join(t, 'out.astc')
        img.save(src)
        subprocess.run([astcenc, '-cs', src, dst, f'{block}x{block}', '-medium', '-silent'], check=True)
        return pathlib.Path(dst).read_bytes()[16:]

def ktx2(img, fmt='rgba8', astcenc=None, levels_max=16):
    img = img.convert('RGBA')
    mips = [img]
    while mips[-1].width > 1 or mips[-1].height > 1:
        w, h = max(1, mips[-1].width // 2), max(1, mips[-1].height // 2)
        mips.append(mips[-1].resize((w, h), Image.LANCZOS))
        if len(mips) >= levels_max: break
    n = len(mips)
    if fmt in ASTC:
        bs, vk = ASTC[fmt]
        datas = [astc_encode(m, bs, astcenc) for m in mips]
        for m, d in zip(mips, datas): assert len(d) == -(-m.width // bs) * -(-m.height // bs) * 16, 'unexpected astcenc output size'
        # DFD: KHR_DF_MODEL_ASTC (162), sRGB, one 128-bit sample per block.
        samples = struct.pack('<HBBIII', 0, 127, 0, 0, 0, 0xFFFFFFFF)
        block = struct.pack('<IHH', 0, 2, 24 + len(samples)) + bytes([162, 1, 2, 0]) + bytes([bs - 1, bs - 1, 0, 0]) + bytes([16, 0, 0, 0, 0, 0, 0, 0]) + samples
        align = 16
    else:
        vk, datas = 43, [m.tobytes() for m in mips]
        # DFD: one basic block for RGBA8 sRGB (KDF 1.3).
        samples = b''.join(struct.pack('<HBBIII', i * 8, 7, ch | (0x10 if ch == 15 else 0), 0, 0, 255) for i, ch in enumerate([0, 1, 2, 15]))
        block = struct.pack('<IHH', 0, 2, 24 + len(samples)) + bytes([1, 1, 2, 0]) + bytes([0, 0, 0, 0]) + bytes([4, 0, 0, 0, 0, 0, 0, 0]) + samples
        align = 4
    dfd = struct.pack('<I', 4 + len(block)) + block
    header_len = 80 + 24 * n
    dfd_off = header_len
    data_off = (dfd_off + len(dfd) + 15) & ~15
    # Levels in file order smallest first; level index stays level 0 (largest) first.
    offsets, blobs, cur = [0] * n, [], data_off
    for i in reversed(range(n)):
        raw = datas[i]
        offsets[i] = cur; blobs.append(raw); cur += len(raw)
        cur = (cur + align - 1) & ~(align - 1)
    out = bytearray()
    out += bytes([0xAB, 0x4B, 0x54, 0x58, 0x20, 0x32, 0x30, 0xBB, 0x0D, 0x0A, 0x1A, 0x0A])
    out += struct.pack('<9I', vk, 0 if fmt in ASTC else 1, img.width, img.height, 0, 0, 1, n, 0)
    out += struct.pack('<IIIIQQ', dfd_off, len(dfd), 0, 0, 0, 0)
    for i in range(n):
        out += struct.pack('<QQQ', offsets[i], len(datas[i]), len(datas[i]))
    out += dfd
    out += b'\0' * (data_off - len(out))
    for i, raw in zip(reversed(range(n)), blobs):
        out += b'\0' * (offsets[i] - len(out))
        out += raw
    return bytes(out), [m.size for m in mips]

def main():
    src, outdir = sys.argv[1], pathlib.Path(sys.argv[2])
    arg = lambda k, d: sys.argv[sys.argv.index(k) + 1] if k in sys.argv else d
    size, fmt, astcenc = int(arg('--size', 1024)), arg('--format', 'astc6x6'), arg('--astcenc', 'astcenc')
    js, bin_ = read_glb(src)
    prim = js['meshes'][0]['primitives'][0]
    # Repack only the accessors the headset reads into a fresh buffer.
    newbin, views, accs = bytearray(), [], []
    def copy_acc(ai):
        a = js['accessors'][ai]; bv = js['bufferViews'][a['bufferView']]
        comps = {'SCALAR': 1, 'VEC2': 2, 'VEC3': 3, 'VEC4': 4}[a['type']]
        csize = {5126: 4, 5125: 4, 5123: 2, 5121: 1}[a['componentType']]
        stride = bv.get('byteStride', csize * comps)
        start = bv.get('byteOffset', 0) + a.get('byteOffset', 0)
        raw = b''.join(bin_[start + i * stride:start + i * stride + csize * comps] for i in range(a['count']))
        while len(newbin) % 4: newbin.append(0)
        views.append({'buffer': 0, 'byteOffset': len(newbin), 'byteLength': len(raw)})
        newbin.extend(raw)
        na = {k: a[k] for k in ('componentType', 'count', 'type', 'min', 'max') if k in a}
        na['bufferView'] = len(views) - 1; accs.append(na)
        return len(accs) - 1
    attrs = {k: copy_acc(prim['attributes'][k]) for k in ('POSITION', 'NORMAL', 'TEXCOORD_0') if k in prim['attributes']}
    newprim = {'attributes': attrs, 'material': 0, 'mode': 4}
    if 'indices' in prim: newprim['indices'] = copy_acc(prim['indices'])
    node = next((dict(n) for n in js.get('nodes', []) if n.get('mesh') == 0), {'mesh': 0})
    node = {k: v for k, v in node.items() if k in ('mesh', 'matrix', 'translation', 'rotation', 'scale')}; node['mesh'] = 0
    # Base colour image -> KTX2.
    mat = js['materials'][prim.get('material', 0)]
    ti = mat['pbrMetallicRoughness']['baseColorTexture']['index']
    im = js['images'][js['textures'][ti]['source']]
    bv = js['bufferViews'][im['bufferView']]
    img = Image.open(io.BytesIO(bin_[bv.get('byteOffset', 0):bv.get('byteOffset', 0) + bv['byteLength']]))
    if img.width > size: img = img.resize((size, size * img.height // img.width), Image.LANCZOS)
    ktx, mips = ktx2(img, fmt, astcenc)
    doc = {'asset': {'version': '2.0', 'generator': 'pinpin make_asset.py'}, 'scene': 0, 'scenes': [{'nodes': [0]}],
           'nodes': [node], 'meshes': [{'primitives': [newprim]}],
           'materials': [{'pbrMetallicRoughness': {'baseColorTexture': {'index': 0}, 'metallicFactor': 0, 'roughnessFactor': 1}}],
           'textures': [{'source': 0}], 'images': [{'uri': 'basecolor.ktx2', 'mimeType': 'image/ktx2'}],
           'accessors': accs, 'bufferViews': views, 'buffers': [{'byteLength': len(newbin)}]}
    jsb = json.dumps(doc, separators=(',', ':')).encode()
    jsb += b' ' * ((-len(jsb)) % 4)
    while len(newbin) % 4: newbin.append(0)
    glb = struct.pack('<III', 0x46546C67, 2, 12 + 8 + len(jsb) + 8 + len(newbin)) + struct.pack('<II', len(jsb), 0x4E4F534A) + jsb + struct.pack('<II', len(newbin), 0x004E4942) + bytes(newbin)
    outdir.mkdir(parents=True, exist_ok=True)
    (outdir / 'model.glb').write_bytes(glb); (outdir / 'basecolor.ktx2').write_bytes(ktx)
    (outdir / 'ATTRIBUTION.txt').write_text('DamagedHelmet: Khronos glTF-Sample-Assets, CC BY 4.0, by theblueturtle_. Converted by assets/make_asset.py.\n')
    print(f'model.glb {len(glb)} bytes ({accs[attrs["POSITION"]]["count"]} vertices), basecolor.ktx2 {fmt} {len(ktx)} bytes, mips {mips[0]}..{mips[-1]} ({len(mips)})')

if __name__ == '__main__':
    main()
