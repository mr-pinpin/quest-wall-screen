// The wall panel as an OpenXR quad composition layer (QUAD in the settings panel; docs/sharp-text.md).
// Drawn into the eye buffers, panel text is resampled twice (eye buffer, then the compositor's lens warp)
// and crawls with every head movement. As a quad layer the compositor samples the mipmapped panel image
// once, at display resolution, with Meta's super-sampling filter. The eye buffers leave the panel area
// transparent so the cursor, buttons and menu still draw on top.
#pragma once
#include <GLES3/gl3.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <algorithm>
#include <string>
#include <vector>

struct PanelLayer {
    bool enabled = true;        // QUAD toggle (default since the user preferred it); files/quad-on forces it on
    bool superSample = false;   // XR_FB_composition_layer_settings available
    bool cylinder = false;      // XR_KHR_composition_layer_cylinder available
    bool sharpen = false;       // SHARPEN: the compositor's quality sharpening on top of super-sampling
    bool curve = false;         // CURVE: a cylinder centred on the user instead of a flat quad
    bool autoFilter = false;    // AUTO: let the compositor choose the layer filter per frame (Meta)
    bool mipQuality = false;    // MIPQ: Lanczos-2 mip chain instead of glGenerateMipmap's 2x2 box (text stays crisper shrunk)
    bool mipBuilt = false;      // the mode the current image's mips were built with
    GLuint mipProgram = 0, mipVao = 0; GLint mipSrc = -1;
    float radius = 2.0f;        // cylinder radius, set from the head-to-panel distance when CURVE is switched on
    XrSwapchain chain = XR_NULL_HANDLE;
    std::vector<GLuint> images;
    GLuint fbo = 0;
    int w = 0, h = 0, levels = 1;   // swapchain size: the panel image plus a transparent margin
    int contentW = 0, contentH = 0;
    // Transparent margin around the panel image: the compositor cuts a layer's outer edge hard, which
    // crawls as a flickering dashed line with head motion. With the margin and source-alpha blending the
    // edge fades over a filtered pixel instead (still >= 1 px at mip level 4).
    int kPad = 0;
    bool margin = false;        // MARGIN flag: the transparent margin above (off: it shrank the panel and moved the line outward)
    bool super = true;          // SUPER flag: Meta's quality super-sampling for the layer
    long version = -1;          // VideoStream::mipVersion last copied
    bool hasImage = false;      // at least one released image to show
    long updates = 0;
    std::string error;
    XrCompositionLayerQuad quad{XR_TYPE_COMPOSITION_LAYER_QUAD};
    XrCompositionLayerCylinderKHR cyl{XR_TYPE_COMPOSITION_LAYER_CYLINDER_KHR};
    XrCompositionLayerSettingsFB settings{XR_TYPE_COMPOSITION_LAYER_SETTINGS_FB};

    void destroy() {
        if (chain) xrDestroySwapchain(chain);
        chain = XR_NULL_HANDLE; images.clear(); hasImage = false; version = -1;
    }
    bool ensure(XrSession session, int width, int height) {
        if (chain && width == w && height == h) return true;
        destroy();
        w = width; h = height; levels = 1; while ((w >> levels) > 0 || (h >> levels) > 0) levels++;
        XrSwapchainCreateInfo sc{XR_TYPE_SWAPCHAIN_CREATE_INFO};
        sc.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
        sc.format = GL_SRGB8_ALPHA8; sc.sampleCount = 1; sc.width = w; sc.height = h; sc.faceCount = 1; sc.arraySize = 1; sc.mipCount = levels;
        XrResult r = xrCreateSwapchain(session, &sc, &chain);
        if (XR_FAILED(r)) { error = "xrCreateSwapchain " + std::to_string((int)r); chain = XR_NULL_HANDLE; return false; }
        uint32_t n = 0; xrEnumerateSwapchainImages(chain, 0, &n, nullptr);
        std::vector<XrSwapchainImageOpenGLESKHR> imgs(n, {XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR});
        xrEnumerateSwapchainImages(chain, n, &n, reinterpret_cast<XrSwapchainImageBaseHeader*>(imgs.data()));
        for (auto& i : imgs) images.push_back(i.image);
        if (!fbo) glGenFramebuffers(1, &fbo);
        error.clear();
        return true;
    }
    // Lanczos-2 2:1 downsample in linear light (sRGB fetch/store), each axis as 4 bilinear taps that combine the
    // 8 Lanczos taps pairwise: weights 0.5508 at +-0.7116 texels and -0.0508 at +-2.675 (per side, sum 1).
    void buildMips(GLuint tex) {
        if (!mipProgram) {
            const char* vs = R"(#version 300 es
void main(){ vec2 p=vec2(float((gl_VertexID<<1)&2),float(gl_VertexID&2)); gl_Position=vec4(p*2.0-1.0,0.0,1.0); })";
            const char* fs = R"(#version 300 es
precision highp float;
uniform sampler2D uSrc; out vec4 o;
const vec2 tap[4] = vec2[4](vec2(-2.675,-0.0508), vec2(-0.7116,0.5508), vec2(0.7116,0.5508), vec2(2.675,-0.0508));
void main(){
  vec2 size=vec2(textureSize(uSrc,0)); vec2 c=gl_FragCoord.xy*2.0; vec4 acc=vec4(0.0);
  for(int j=0;j<4;j++) for(int i=0;i<4;i++)
    acc+=texture(uSrc,(c+vec2(tap[i].x,tap[j].x))/size)*(tap[i].y*tap[j].y);
  o=clamp(acc,0.0,1.0);
})";
            auto sh = [](GLenum t, const char* src) { GLuint s = glCreateShader(t); glShaderSource(s, 1, &src, nullptr); glCompileShader(s); return s; };
            mipProgram = glCreateProgram();
            glAttachShader(mipProgram, sh(GL_VERTEX_SHADER, vs)); glAttachShader(mipProgram, sh(GL_FRAGMENT_SHADER, fs));
            glLinkProgram(mipProgram); mipSrc = glGetUniformLocation(mipProgram, "uSrc");
            glGenVertexArrays(1, &mipVao);
        }
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glUseProgram(mipProgram); glBindVertexArray(mipVao); glUniform1i(mipSrc, 0); glActiveTexture(GL_TEXTURE0);
        glDisable(GL_BLEND); glDisable(GL_DEPTH_TEST);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        for (int l = 1; l < levels; l++) {
            // Sample only level l-1 while rendering level l, so there is no feedback loop.
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, l - 1); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, l - 1);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, l);
            glViewport(0, 0, std::max(1, w >> l), std::max(1, h >> l));
            glDrawArrays(GL_TRIANGLES, 0, 3);
        }
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, levels - 1);
        glBindFramebuffer(GL_FRAMEBUFFER, 0); glBindVertexArray(0); glUseProgram(0); glBindTexture(GL_TEXTURE_2D, 0);
    }
    // Copy the latest panel image (the video's mipmapped frame, exact tiles included) into the swapchain,
    // flipped to OpenXR's bottom-up image rows, and build its mip chain. Only when a new frame arrived.
    void update(XrSession session, GLuint srcFbo, int width, int height, long mipVersion, int srcX = 0, int srcY = 0) {
        if (!srcFbo || width <= 0 || height <= 0 || (mipVersion == version && mipBuilt == mipQuality && kPad == (margin ? 16 : 0))) return;
        kPad = margin ? 16 : 0;
        if (!ensure(session, width + 2 * kPad, height + 2 * kPad)) return;
        contentW = width; contentH = height;
        uint32_t idx = 0;
        XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
        if (XR_FAILED(xrAcquireSwapchainImage(chain, &ai, &idx))) return;
        XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO}; wi.timeout = XR_INFINITE_DURATION; xrWaitSwapchainImage(chain, &wi);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, srcFbo);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbo);
        glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, images[idx], 0);
        glViewport(0, 0, w, h); glClearColor(0, 0, 0, 0); glClear(GL_COLOR_BUFFER_BIT);  // the margin stays transparent
        glBlitFramebuffer(srcX, srcY, srcX + width, srcY + height, kPad, kPad + height, kPad + width, kPad, GL_COLOR_BUFFER_BIT, GL_NEAREST);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        if (mipQuality) buildMips(images[idx]);
        else { glBindTexture(GL_TEXTURE_2D, images[idx]); glGenerateMipmap(GL_TEXTURE_2D); glBindTexture(GL_TEXTURE_2D, 0); }
        mipBuilt = mipQuality;
        XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO}; xrReleaseSwapchainImage(chain, &ri);
        version = mipVersion; hasImage = true; updates++;
    }
    // flatPose: panel centre on the wall. axisPose: same orientation, `radius` in front of the wall, so the
    // cylinder's arc (centred on -Z) touches the wall at the panel centre and curves toward the user.
    XrCompositionLayerBaseHeader* layer(XrSpace space, const XrPosef& flatPose, const XrPosef& axisPose, float width, float height) {
        const void* next = nullptr;
        if (superSample && super) {
            settings = {XR_TYPE_COMPOSITION_LAYER_SETTINGS_FB};
            settings.layerFlags = XR_COMPOSITION_LAYER_SETTINGS_QUALITY_SUPER_SAMPLING_BIT_FB |
                                  (sharpen ? XR_COMPOSITION_LAYER_SETTINGS_QUALITY_SHARPENING_BIT_FB : 0) |
                                  (autoFilter ? XR_COMPOSITION_LAYER_SETTINGS_AUTO_LAYER_FILTER_BIT_META : 0);
            next = &settings;
        }
        XrSwapchainSubImage sub{}; sub.swapchain = chain; sub.imageRect = {{0, 0}, {w, h}}; sub.imageArrayIndex = 0;
        // Grow the layer by the margin so the panel content keeps its physical size.
        float lw = contentW > 0 ? width * w / contentW : width, lh = contentH > 0 ? height * h / contentH : height;
        if (curve && cylinder && radius > 0.1f) {
            cyl = {XR_TYPE_COMPOSITION_LAYER_CYLINDER_KHR};
            cyl.next = next; cyl.space = space; cyl.eyeVisibility = XR_EYE_VISIBILITY_BOTH; cyl.subImage = sub; cyl.pose = axisPose;
            cyl.layerFlags = margin ? XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT : 0;
            cyl.radius = radius; cyl.centralAngle = lw / radius; cyl.aspectRatio = lw / lh;
            return reinterpret_cast<XrCompositionLayerBaseHeader*>(&cyl);
        }
        quad = {XR_TYPE_COMPOSITION_LAYER_QUAD};
        quad.next = next; quad.space = space; quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH; quad.pose = flatPose; quad.size = {lw, lh};
        quad.layerFlags = margin ? XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT : 0;
        quad.subImage = sub;
        return reinterpret_cast<XrCompositionLayerBaseHeader*>(&quad);
    }
    std::string json() const {
        return "{\"enabled\":" + std::to_string(enabled ? 1 : 0) + ",\"image\":" + std::to_string(hasImage ? 1 : 0) + ",\"size\":[" + std::to_string(w) + "," +
               std::to_string(h) + "],\"levels\":" + std::to_string(levels) + ",\"superSample\":" + std::to_string(superSample ? 1 : 0) + ",\"sharpen\":" + std::to_string(sharpen ? 1 : 0) + ",\"auto\":" + std::to_string(autoFilter ? 1 : 0) + ",\"mipq\":" + std::to_string(mipBuilt ? 1 : 0) + ",\"margin\":" + std::to_string(margin ? 1 : 0) + ",\"super\":" + std::to_string(super ? 1 : 0) +
               ",\"curve\":" + std::to_string(curve && cylinder ? 1 : 0) + ",\"radius\":" + std::to_string(radius) +
               ",\"updates\":" + std::to_string(updates) + ",\"error\":\"" + error + "\"}";
    }
};
