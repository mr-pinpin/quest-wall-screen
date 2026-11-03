//! pinpin_render: a WebGPU (wgpu, Metal backend) renderer for the atlas sender's "surface" source
//! (docs/sources.md). The sender owns IOSurface-backed CVPixelBuffers at the size the layout wants for a screen
//! (its VT level) and asks this library to draw a frame into one; wgpu renders straight into the IOSurface (zero
//! copies), then the sender hands the buffer to the same deliver() path as screen captures.
//!
//! C ABI (loaded with dlopen by the sender, so the sender runs without it):
//!   void*  pp_render_new(void)                                           -> renderer, or NULL
//!   int    pp_render_frame(void* r, void* iosurface, uint32 w, uint32 h, uint32 screen, double t)  -> 0 ok
//!   void   pp_render_free(void* r)
use std::collections::HashMap;
use std::ffi::c_void;

use metal::foreign_types::ForeignType;
use objc::{msg_send, sel, sel_impl};

const SHADER: &str = r#"
struct U { t: f32, screen: f32, w: f32, h: f32 };
@group(0) @binding(0) var<uniform> u: U;
@vertex fn vs(@builtin(vertex_index) i: u32) -> @builtin(position) vec4<f32> {
    let p = array<vec2<f32>, 3>(vec2(-1.0, -3.0), vec2(-1.0, 1.0), vec2(3.0, 1.0));
    return vec4<f32>(p[i], 0.0, 1.0);
}
// A game-like scene: moving lit rings and bars, different per screen, every pixel changing every frame.
@fragment fn fs(@builtin(position) pos: vec4<f32>) -> @location(0) vec4<f32> {
    let uv = pos.xy / vec2<f32>(u.w, u.h);
    let c = vec2<f32>(0.5 + 0.3 * sin(u.t * 0.7 + u.screen), 0.5 + 0.3 * cos(u.t * 0.9 + u.screen * 1.3));
    let d = distance(uv * vec2<f32>(u.w / u.h, 1.0), c * vec2<f32>(u.w / u.h, 1.0));
    let rings = 0.5 + 0.5 * sin(d * 60.0 - u.t * 6.0);
    let bars = 0.5 + 0.5 * sin(uv.x * 40.0 + u.t * 3.0 + u.screen);
    let base = vec3<f32>(0.5 + 0.5 * sin(u.screen * 1.7 + u.t), 0.5 + 0.5 * sin(u.screen * 2.3 + 1.0), 0.6);
    let text = step(0.92, fract(uv.y * 48.0)) * step(0.3, fract(uv.x * 9.0 + u.t * 0.2));  // ruled "text" lines
    return vec4<f32>(mix(base * rings, vec3<f32>(bars), 0.3) * (1.0 - 0.6 * text), 1.0);
}
"#;

pub struct Renderer {
    device: wgpu::Device,
    queue: wgpu::Queue,
    pipeline: wgpu::RenderPipeline,
    uniforms: wgpu::Buffer,
    bind: wgpu::BindGroup,
    targets: HashMap<(usize, u32, u32), wgpu::Texture>, // IOSurface pointer + size -> wrapped texture
}

fn new_renderer() -> Option<Renderer> {
    let instance = wgpu::Instance::new(&wgpu::InstanceDescriptor { backends: wgpu::Backends::METAL, ..Default::default() });
    let adapter = pollster::block_on(instance.request_adapter(&wgpu::RequestAdapterOptions {
        power_preference: wgpu::PowerPreference::HighPerformance, ..Default::default() })).ok()?;
    let (device, queue) = pollster::block_on(adapter.request_device(&wgpu::DeviceDescriptor::default())).ok()?;
    let module = device.create_shader_module(wgpu::ShaderModuleDescriptor { label: Some("pp"), source: wgpu::ShaderSource::Wgsl(SHADER.into()) });
    let uniforms = device.create_buffer(&wgpu::BufferDescriptor { label: None, size: 16, usage: wgpu::BufferUsages::UNIFORM | wgpu::BufferUsages::COPY_DST, mapped_at_creation: false });
    let layout = device.create_bind_group_layout(&wgpu::BindGroupLayoutDescriptor { label: None, entries: &[wgpu::BindGroupLayoutEntry {
        binding: 0, visibility: wgpu::ShaderStages::FRAGMENT, ty: wgpu::BindingType::Buffer { ty: wgpu::BufferBindingType::Uniform, has_dynamic_offset: false, min_binding_size: None }, count: None }] });
    let bind = device.create_bind_group(&wgpu::BindGroupDescriptor { label: None, layout: &layout, entries: &[wgpu::BindGroupEntry { binding: 0, resource: uniforms.as_entire_binding() }] });
    let pl = device.create_pipeline_layout(&wgpu::PipelineLayoutDescriptor { label: None, bind_group_layouts: &[&layout], push_constant_ranges: &[] });
    let pipeline = device.create_render_pipeline(&wgpu::RenderPipelineDescriptor {
        label: Some("pp"), layout: Some(&pl),
        vertex: wgpu::VertexState { module: &module, entry_point: Some("vs"), buffers: &[], compilation_options: Default::default() },
        fragment: Some(wgpu::FragmentState { module: &module, entry_point: Some("fs"), compilation_options: Default::default(),
            targets: &[Some(wgpu::ColorTargetState { format: wgpu::TextureFormat::Bgra8Unorm, blend: None, write_mask: wgpu::ColorWrites::ALL })] }),
        primitive: Default::default(), depth_stencil: None, multisample: Default::default(), multiview: None, cache: None });
    Some(Renderer { device, queue, pipeline, uniforms, bind, targets: HashMap::new() })
}

/// Wrap an IOSurface as a wgpu render target (Metal texture over the surface, then wgpu-hal -> wgpu).
unsafe fn wrap(r: &Renderer, surface: *mut c_void, w: u32, h: u32) -> Option<wgpu::Texture> {
    let hal_dev = r.device.as_hal::<wgpu_hal::api::Metal>()?;
    let mtl_device = hal_dev.raw_device().lock().clone();
    let desc = metal::TextureDescriptor::new();
    desc.set_texture_type(metal::MTLTextureType::D2);
    desc.set_pixel_format(metal::MTLPixelFormat::BGRA8Unorm);
    desc.set_width(w as u64); desc.set_height(h as u64);
    desc.set_storage_mode(metal::MTLStorageMode::Shared);
    desc.set_usage(metal::MTLTextureUsage::RenderTarget | metal::MTLTextureUsage::ShaderRead);
    let raw: *mut metal::MTLTexture = msg_send![mtl_device.as_ptr() as *mut objc::runtime::Object,
        newTextureWithDescriptor: desc.as_ptr() as *mut objc::runtime::Object iosurface: surface plane: 0usize];
    if raw.is_null() { return None; }
    let texture = metal::Texture::from_ptr(raw);
    let hal_tex = wgpu_hal::metal::Device::texture_from_raw(texture, wgpu::TextureFormat::Bgra8Unorm, metal::MTLTextureType::D2, 1, 1,
        wgpu_hal::CopyExtent { width: w, height: h, depth: 1 });
    drop(hal_dev);
    Some(r.device.create_texture_from_hal::<wgpu_hal::api::Metal>(hal_tex, &wgpu::TextureDescriptor {
        label: Some("iosurface"), size: wgpu::Extent3d { width: w, height: h, depth_or_array_layers: 1 }, mip_level_count: 1, sample_count: 1,
        dimension: wgpu::TextureDimension::D2, format: wgpu::TextureFormat::Bgra8Unorm,
        usage: wgpu::TextureUsages::RENDER_ATTACHMENT, view_formats: &[] }))
}

#[no_mangle]
pub extern "C" fn pp_render_new() -> *mut c_void {
    match new_renderer() { Some(r) => Box::into_raw(Box::new(r)) as *mut c_void, None => std::ptr::null_mut() }
}

#[no_mangle]
pub unsafe extern "C" fn pp_render_frame(r: *mut c_void, surface: *mut c_void, w: u32, h: u32, screen: u32, t: f64) -> i32 {
    if r.is_null() || surface.is_null() || w == 0 || h == 0 { return -1; }
    let r = &mut *(r as *mut Renderer);
    let key = (surface as usize, w, h);
    if !r.targets.contains_key(&key) {
        if r.targets.len() > 64 { r.targets.clear(); }  // surfaces come from a small pool; drop stale wrappers
        match wrap(r, surface, w, h) { Some(tex) => { r.targets.insert(key, tex); } None => return -2 }
    }
    let view = r.targets[&key].create_view(&Default::default());
    let u: [f32; 4] = [t as f32, screen as f32, w as f32, h as f32];
    r.queue.write_buffer(&r.uniforms, 0, std::slice::from_raw_parts(u.as_ptr() as *const u8, 16));
    let mut enc = r.device.create_command_encoder(&Default::default());
    {
        let mut pass = enc.begin_render_pass(&wgpu::RenderPassDescriptor {
            label: None, color_attachments: &[Some(wgpu::RenderPassColorAttachment { view: &view, resolve_target: None, depth_slice: None,
                ops: wgpu::Operations { load: wgpu::LoadOp::Clear(wgpu::Color::BLACK), store: wgpu::StoreOp::Store } })],
            depth_stencil_attachment: None, timestamp_writes: None, occlusion_query_set: None });
        pass.set_pipeline(&r.pipeline); pass.set_bind_group(0, &r.bind, &[]); pass.draw(0..3, 0..1);
    }
    let idx = r.queue.submit([enc.finish()]);
    // The sender copies the surface right after this returns: wait for the GPU to finish this frame.
    let _ = r.device.poll(wgpu::PollType::WaitForSubmissionIndex(idx));
    0
}

#[no_mangle]
pub unsafe extern "C" fn pp_render_free(r: *mut c_void) {
    if !r.is_null() { drop(Box::from_raw(r as *mut Renderer)); }
}
