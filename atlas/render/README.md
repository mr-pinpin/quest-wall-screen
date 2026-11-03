# pinpin_render: our own WebGPU renderer as an atlas source

Rust wgpu (Metal backend) renders each frame straight into an IOSurface owned by the atlas sender's `SurfaceSource`
(atlas/AtlasSender.swift, `--surface "S:WxH,..."`), zero copies; the sender loads `libpinpin_render.dylib` with
dlopen (so it runs without it) and hands the frame to the same `deliver()` path as screen captures. See
docs/sources.md, "Sender side".

Build on the Mini (it has Rust; don't build on the Air during benchmark runs), targeting the Air's macOS too:

    cd atlas/render && MACOSX_DEPLOYMENT_TARGET=15.1 cargo build --release
    clang -O2 -o rtest test.c -framework IOSurface -framework CoreFoundation && ./rtest target/release/libpinpin_render.dylib

then copy `target/release/libpinpin_render.dylib` to the Air's `~/.local/share/pinpin-atlas/` in a bench-lock slot.

Measured on the Mini (M4), 2026-11-03: RENDER OK, 1.48 ms per 1920x1200 frame after a 34 ms first frame (wrap +
pipeline setup).
