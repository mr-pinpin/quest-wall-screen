// The wall panel as an OpenXR quad composition layer (QUAD in the settings panel; docs/sharp-text.md).
// Drawn into the eye buffers, panel text is resampled twice (eye buffer, then the compositor's lens warp)
// and crawls with every head movement. As a quad layer the compositor samples the mipmapped panel image
// once, at display resolution, with Meta's super-sampling filter. The eye buffers leave the panel area
// transparent so the cursor, buttons and menu still draw on top.
#pragma once
#include <GLES3/gl3.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <string>
#include <vector>

struct PanelLayer {
    bool enabled = true;        // QUAD toggle (default since the user preferred it); files/quad-on forces it on
    bool superSample = false;   // XR_FB_composition_layer_settings available
    bool cylinder = false;      // XR_KHR_composition_layer_cylinder available
    bool sharpen = false;       // SHARPEN: the compositor's quality sharpening on top of super-sampling
    bool curve = false;         // CURVE: a cylinder centred on the user instead of a flat quad
    bool autoFilter = false;    // AUTO: let the compositor choose the layer filter per frame (Meta)
    float radius = 2.0f;        // cylinder radius, set from the head-to-panel distance when CURVE is switched on
    XrSwapchain chain = XR_NULL_HANDLE;
    std::vector<GLuint> images;
    GLuint fbo = 0;
    int w = 0, h = 0, levels = 1;
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
    // Copy the latest panel image (the video's mipmapped frame, exact tiles included) into the swapchain,
    // flipped to OpenXR's bottom-up image rows, and build its mip chain. Only when a new frame arrived.
    void update(XrSession session, GLuint srcFbo, int width, int height, long mipVersion) {
        if (!srcFbo || width <= 0 || height <= 0 || mipVersion == version) return;
        if (!ensure(session, width, height)) return;
        uint32_t idx = 0;
        XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
        if (XR_FAILED(xrAcquireSwapchainImage(chain, &ai, &idx))) return;
        XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO}; wi.timeout = XR_INFINITE_DURATION; xrWaitSwapchainImage(chain, &wi);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, srcFbo);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbo);
        glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, images[idx], 0);
        glBlitFramebuffer(0, 0, w, h, 0, h, w, 0, GL_COLOR_BUFFER_BIT, GL_NEAREST);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glBindTexture(GL_TEXTURE_2D, images[idx]); glGenerateMipmap(GL_TEXTURE_2D); glBindTexture(GL_TEXTURE_2D, 0);
        XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO}; xrReleaseSwapchainImage(chain, &ri);
        version = mipVersion; hasImage = true; updates++;
    }
    // flatPose: panel centre on the wall. axisPose: same orientation, `radius` in front of the wall, so the
    // cylinder's arc (centred on -Z) touches the wall at the panel centre and curves toward the user.
    XrCompositionLayerBaseHeader* layer(XrSpace space, const XrPosef& flatPose, const XrPosef& axisPose, float width, float height) {
        const void* next = nullptr;
        if (superSample) {
            settings = {XR_TYPE_COMPOSITION_LAYER_SETTINGS_FB};
            settings.layerFlags = XR_COMPOSITION_LAYER_SETTINGS_QUALITY_SUPER_SAMPLING_BIT_FB |
                                  (sharpen ? XR_COMPOSITION_LAYER_SETTINGS_QUALITY_SHARPENING_BIT_FB : 0) |
                                  (autoFilter ? XR_COMPOSITION_LAYER_SETTINGS_AUTO_LAYER_FILTER_BIT_META : 0);
            next = &settings;
        }
        XrSwapchainSubImage sub{}; sub.swapchain = chain; sub.imageRect = {{0, 0}, {w, h}}; sub.imageArrayIndex = 0;
        if (curve && cylinder && radius > 0.1f) {
            cyl = {XR_TYPE_COMPOSITION_LAYER_CYLINDER_KHR};
            cyl.next = next; cyl.space = space; cyl.eyeVisibility = XR_EYE_VISIBILITY_BOTH; cyl.subImage = sub; cyl.pose = axisPose;
            cyl.radius = radius; cyl.centralAngle = width / radius; cyl.aspectRatio = width / height;
            return reinterpret_cast<XrCompositionLayerBaseHeader*>(&cyl);
        }
        quad = {XR_TYPE_COMPOSITION_LAYER_QUAD};
        quad.next = next; quad.space = space; quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH; quad.pose = flatPose; quad.size = {width, height};
        quad.subImage = sub;
        return reinterpret_cast<XrCompositionLayerBaseHeader*>(&quad);
    }
    std::string json() const {
        return "{\"enabled\":" + std::to_string(enabled ? 1 : 0) + ",\"image\":" + std::to_string(hasImage ? 1 : 0) + ",\"size\":[" + std::to_string(w) + "," +
               std::to_string(h) + "],\"levels\":" + std::to_string(levels) + ",\"superSample\":" + std::to_string(superSample ? 1 : 0) + ",\"sharpen\":" + std::to_string(sharpen ? 1 : 0) + ",\"auto\":" + std::to_string(autoFilter ? 1 : 0) +
               ",\"curve\":" + std::to_string(curve && cylinder ? 1 : 0) + ",\"radius\":" + std::to_string(radius) +
               ",\"updates\":" + std::to_string(updates) + ",\"error\":\"" + error + "\"}";
    }
};
