// The 3D view's depth pass (view3d_gpu.hpp): pipelines and buffers on the SDL_Renderer's own GPU
// device, and each frame's triangles drawn into a texture the renderer then shows.
#include "view3d_gpu.hpp"

#include <algorithm>
#include <cstring>

// The shaders, compiled to DXIL / SPIR-V at build time and linked in as byte arrays.
#include "gpu_shader_code.hpp"

namespace noxsdl {

namespace {

constexpr SDL_PixelFormat kSceneFormat = SDL_PIXELFORMAT_RGBA32;
constexpr float kOpaqueCut = 0.5f; // alpha under this isn't drawn (and doesn't hide what's behind)
constexpr float kBlendCut = 0.004f;  // the translucent draws drop only the nearly clear pixels

// The GPU texture behind a renderer texture (null for none).
SDL_GPUTexture* gpuTexture(SDL_Texture* texture)
{
    return texture ? static_cast<SDL_GPUTexture*>(
                         SDL_GetPointerProperty(SDL_GetTextureProperties(texture), SDL_PROP_TEXTURE_GPU_TEXTURE_POINTER, nullptr))
                   : nullptr;
}

} // namespace

// Null (the caller sorts back to front instead) without a GPU renderer that takes our shaders, a
// shader, a pipeline or a depth format.
std::unique_ptr<DepthPass> DepthPass::create(SDL_Renderer* renderer)
{
#if defined(NOXSDL_HAVE_DXIL) || defined(NOXSDL_HAVE_SPIRV)
    SDL_GPUDevice* device = SDL_GetGPURendererDevice(renderer);
    if (!device) {
        SDL_Log("view3d: no GPU renderer: the 3D view sorts back to front instead of a depth buffer");
        return nullptr;
    }
    const SDL_GPUShaderFormat format = embeddedShaderFormat(device);
    if (format == SDL_GPU_SHADERFORMAT_INVALID) {
        SDL_Log("view3d: %s takes none of our shaders' formats: the 3D view sorts back to front",
                SDL_GetGPUDeviceDriver(device));
        return nullptr;
    }
    std::unique_ptr<DepthPass> out(new DepthPass);
    out->device_ = device;
    const auto shader = [&](ShaderId id, SDL_GPUShaderStage stage, Uint32 samplers, Uint32 uniforms) {
        const ShaderCode code = embeddedShader(id, format);
        SDL_GPUShaderCreateInfo info{};
        info.code = code.code;
        info.code_size = code.size;
        info.entrypoint = "main";
        info.format = format;
        info.stage = stage;
        info.num_samplers = samplers;
        info.num_uniform_buffers = uniforms;
        SDL_GPUShader* s = SDL_CreateGPUShader(device, &info);
        if (!s) {
            SDL_Log("view3d: shader: %s", SDL_GetError());
        }
        return s;
    };
    out->vertex_ = shader(ShaderId::view3d_scene_vert, SDL_GPU_SHADERSTAGE_VERTEX, 0, 0);
    out->plain_ = shader(ShaderId::view3d_scene, SDL_GPU_SHADERSTAGE_FRAGMENT, 1, 1);
    out->material_ = shader(ShaderId::view3d_material, SDL_GPU_SHADERSTAGE_FRAGMENT, 2, 1);
    out->particle_ = shader(ShaderId::view3d_particle, SDL_GPU_SHADERSTAGE_FRAGMENT, 0, 0);
    if (!out->vertex_ || !out->plain_ || !out->material_ || !out->particle_) {
        return nullptr;
    }
    // 32-bit float depth if the device has it, else 24-bit.
    if (!SDL_GPUTextureSupportsFormat(device, out->depthFormat_, SDL_GPU_TEXTURETYPE_2D,
                                      SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET)) {
        out->depthFormat_ = SDL_GPU_TEXTUREFORMAT_D24_UNORM;
        if (!SDL_GPUTextureSupportsFormat(device, out->depthFormat_, SDL_GPU_TEXTURETYPE_2D,
                                          SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET)) {
            SDL_Log("view3d: no depth format");
            return nullptr;
        }
    }

    // One vertex buffer of Vertex: position (clip space, 4 floats), texture coordinates, colour.
    SDL_GPUVertexBufferDescription buffer{};
    buffer.slot = 0;
    buffer.pitch = sizeof(Vertex);
    buffer.input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;
    SDL_GPUVertexAttribute attributes[3]{};
    // (The casts: MSVC's offsetof isn't a constant expression to clang-cl, which then refuses the
    // narrowing in the braces.)
    attributes[0] = {0, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, static_cast<Uint32>(offsetof(Vertex, x))};
    attributes[1] = {1, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, static_cast<Uint32>(offsetof(Vertex, u))};
    attributes[2] = {2, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, static_cast<Uint32>(offsetof(Vertex, r))};
    // Straight alpha blending into the scene texture (every pipeline; the added ones change it below).
    SDL_GPUColorTargetDescription target{};
    target.format = SDL_GetGPUTextureFormatFromPixelFormat(kSceneFormat);
    target.blend_state.enable_blend = true;
    target.blend_state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_SRC_ALPHA;
    target.blend_state.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
    target.blend_state.color_blend_op = SDL_GPU_BLENDOP_ADD;
    target.blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
    target.blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
    target.blend_state.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
    // A pipeline for a fragment shader: depth tested always, written only by the opaque ones; no
    // culling (View3D culls the faces itself).
    const auto pipeline = [&](SDL_GPUShader* fragment, bool writeDepth, bool added = false) {
        SDL_GPUColorTargetDescription mine = target;
        if (added) {
            mine.blend_state.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE; // (light: added on)
            mine.blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ZERO;
            mine.blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
        }
        SDL_GPUGraphicsPipelineCreateInfo info{};
        info.vertex_shader = out->vertex_;
        info.fragment_shader = fragment;
        info.vertex_input_state.vertex_buffer_descriptions = &buffer;
        info.vertex_input_state.num_vertex_buffers = 1;
        info.vertex_input_state.vertex_attributes = attributes;
        info.vertex_input_state.num_vertex_attributes = 3;
        info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
        info.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
        info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
        info.rasterizer_state.front_face = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;
        info.depth_stencil_state.enable_depth_test = true;
        info.depth_stencil_state.enable_depth_write = writeDepth;
        info.depth_stencil_state.compare_op = SDL_GPU_COMPAREOP_LESS_OR_EQUAL;
        info.target_info.color_target_descriptions = &mine;
        info.target_info.num_color_targets = 1;
        info.target_info.has_depth_stencil_target = true;
        info.target_info.depth_stencil_format = out->depthFormat_;
        SDL_GPUGraphicsPipeline* p = SDL_CreateGPUGraphicsPipeline(device, &info);
        if (!p) {
            SDL_Log("view3d: pipeline: %s", SDL_GetError());
        }
        return p;
    };
    out->opaquePlain_ = pipeline(out->plain_, true);
    out->opaqueMaterial_ = pipeline(out->material_, true);
    out->blendPlain_ = pipeline(out->plain_, false);
    out->blendMaterial_ = pipeline(out->material_, false);
    out->particlesBlended_ = pipeline(out->particle_, false);
    out->particlesAdded_ = pipeline(out->particle_, false, true);
    if (!out->opaquePlain_ || !out->opaqueMaterial_ || !out->blendPlain_ || !out->blendMaterial_ ||
        !out->particlesBlended_ || !out->particlesAdded_) {
        return nullptr;
    }
    // Clamped at the edges; one mip level.
    const auto sampler = [&](SDL_GPUFilter filter) {
        SDL_GPUSamplerCreateInfo info{};
        info.min_filter = filter;
        info.mag_filter = filter;
        info.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
        info.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
        info.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
        info.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
        return SDL_CreateGPUSampler(device, &info);
    };
    out->nearest_ = sampler(SDL_GPU_FILTER_NEAREST);
    out->linear_ = sampler(SDL_GPU_FILTER_LINEAR);
    if (!out->nearest_ || !out->linear_) {
        return nullptr;
    }
    // The 1x1 target render() reads back to submit the renderer's queued work.
    out->sync_ = SDL_CreateTexture(renderer, kSceneFormat, SDL_TEXTUREACCESS_TARGET, 1, 1);
    if (!out->sync_) {
        return nullptr;
    }
    SDL_Log("view3d: the 3D view draws with a depth buffer (%s)", SDL_GetGPUDeviceDriver(device));
    return out;
#else
    (void)renderer;
    return nullptr;
#endif
}

DepthPass::~DepthPass()
{
    if (!device_) {
        return;
    }
    const auto release = [&](auto* p, auto fn) {
        if (p) {
            fn(device_, p);
        }
    };
    release(opaquePlain_, SDL_ReleaseGPUGraphicsPipeline);
    release(opaqueMaterial_, SDL_ReleaseGPUGraphicsPipeline);
    release(blendPlain_, SDL_ReleaseGPUGraphicsPipeline);
    release(blendMaterial_, SDL_ReleaseGPUGraphicsPipeline);
    release(particlesAdded_, SDL_ReleaseGPUGraphicsPipeline);
    release(particlesBlended_, SDL_ReleaseGPUGraphicsPipeline);
    release(particle_, SDL_ReleaseGPUShader);
    release(vertex_, SDL_ReleaseGPUShader);
    release(plain_, SDL_ReleaseGPUShader);
    release(material_, SDL_ReleaseGPUShader);
    release(nearest_, SDL_ReleaseGPUSampler);
    release(linear_, SDL_ReleaseGPUSampler);
    release(depth_, SDL_ReleaseGPUTexture);
    release(vertexBuffer_, SDL_ReleaseGPUBuffer);
    release(transfer_, SDL_ReleaseGPUTransferBuffer);
    if (scene_) {
        SDL_DestroyTexture(scene_);
    }
    if (sync_) {
        SDL_DestroyTexture(sync_);
    }
}

// The scene texture and depth buffer at the view's size, remade when it changes.
bool DepthPass::ensureTargets(SDL_Renderer* renderer, int width, int height)
{
    if (scene_ && width == width_ && height == height_) {
        return true;
    }
    if (scene_) {
        SDL_DestroyTexture(scene_);
        scene_ = nullptr;
    }
    if (depth_) {
        SDL_ReleaseGPUTexture(device_, depth_);
        depth_ = nullptr;
    }
    scene_ = SDL_CreateTexture(renderer, kSceneFormat, SDL_TEXTUREACCESS_TARGET, width, height);
    SDL_GPUTextureCreateInfo info{};
    info.type = SDL_GPU_TEXTURETYPE_2D;
    info.format = depthFormat_;
    info.usage = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET;
    info.width = static_cast<Uint32>(width);
    info.height = static_cast<Uint32>(height);
    info.layer_count_or_depth = 1;
    info.num_levels = 1;
    depth_ = SDL_CreateGPUTexture(device_, &info);
    if (!scene_ || !depth_) {
        SDL_Log("view3d: depth pass targets %dx%d: %s", width, height, SDL_GetError());
        return false;
    }
    SDL_SetTextureBlendMode(scene_, SDL_BLENDMODE_NONE);
    width_ = width;
    height_ = height;
    return true;
}

// Vertex and upload buffers of at least `bytes`, grown by half again (1 MB at the least).
bool DepthPass::ensureBuffers(std::uint32_t bytes)
{
    if (bytes <= bufferBytes_) {
        return true;
    }
    if (vertexBuffer_) {
        SDL_ReleaseGPUBuffer(device_, vertexBuffer_);
    }
    if (transfer_) {
        SDL_ReleaseGPUTransferBuffer(device_, transfer_);
    }
    bufferBytes_ = std::max<std::uint32_t>(bytes + bytes / 2, 1u << 20);
    SDL_GPUBufferCreateInfo b{};
    b.usage = SDL_GPU_BUFFERUSAGE_VERTEX;
    b.size = bufferBytes_;
    vertexBuffer_ = SDL_CreateGPUBuffer(device_, &b);
    SDL_GPUTransferBufferCreateInfo t{};
    t.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    t.size = bufferBytes_;
    transfer_ = SDL_CreateGPUTransferBuffer(device_, &t);
    if (!vertexBuffer_ || !transfer_) {
        bufferBytes_ = 0;
        return false;
    }
    return true;
}

SDL_Texture* DepthPass::render(SDL_Renderer* renderer, int width, int height, std::span<const Vertex> vertices,
                               std::vector<Draw>& draws)
{
    if (width <= 0 || height <= 0 || !ensureTargets(renderer, width, height)) {
        return nullptr;
    }
    const auto bytes = static_cast<std::uint32_t>(vertices.size_bytes());
    if (bytes > 0 && !ensureBuffers(bytes)) {
        return nullptr;
    }
    // The renderer's work so far (the floor texture, the sprites' uploads) submitted first.
    {
        // Reading a pixel back makes the renderer flush its queued commands, so this pass sees their
        // results on the shared device.
        SDL_Texture* const previous = SDL_GetRenderTarget(renderer);
        SDL_SetRenderTarget(renderer, sync_);
        const SDL_Rect one{0, 0, 1, 1};
        if (SDL_Surface* s = SDL_RenderReadPixels(renderer, &one)) {
            SDL_DestroySurface(s);
        }
        SDL_SetRenderTarget(renderer, previous);
    }
    SDL_GPUTexture* const colour = gpuTexture(scene_);
    if (!colour) {
        return nullptr;
    }
    // Upload this frame's vertices, then one render pass that clears colour and depth.
    SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(device_);
    if (!cmd) {
        return nullptr;
    }
    if (bytes > 0) {
        // Mapped with cycling (last frame's copy may still be in use on the GPU), then copied into the
        // vertex buffer.
        if (void* mapped = SDL_MapGPUTransferBuffer(device_, transfer_, true)) {
            std::memcpy(mapped, vertices.data(), bytes);
            SDL_UnmapGPUTransferBuffer(device_, transfer_);
        }
        SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(cmd);
        SDL_GPUTransferBufferLocation from{transfer_, 0};
        SDL_GPUBufferRegion to{vertexBuffer_, 0, bytes};
        SDL_UploadToGPUBuffer(copy, &from, &to, true);
        SDL_EndGPUCopyPass(copy);
    }
    SDL_GPUColorTargetInfo target{};
    target.texture = colour;
    target.clear_color = {0, 0, 0, 1};
    target.load_op = SDL_GPU_LOADOP_CLEAR;
    target.store_op = SDL_GPU_STOREOP_STORE;
    SDL_GPUDepthStencilTargetInfo depth{};
    depth.texture = depth_;
    depth.clear_depth = 1;
    depth.load_op = SDL_GPU_LOADOP_CLEAR;
    depth.store_op = SDL_GPU_STOREOP_DONT_CARE;  // the depth isn't needed after the pass
    depth.stencil_load_op = SDL_GPU_LOADOP_DONT_CARE;
    depth.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;
    depth.cycle = true;
    SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(cmd, &target, 1, &depth);
    if (pass && bytes > 0) {
        SDL_GPUBufferBinding binding{vertexBuffer_, 0};
        SDL_BindGPUVertexBuffers(pass, 0, &binding, 1);
        // Opaque first; then the translucent, far to near; then the particles, the blended far to
        // near and the added (in any order) last.
        const auto rank = [](const Draw& d) {
            return d.kind == Kind::ParticlesAdded ? 3 : d.kind == Kind::ParticlesBlended ? 2 : d.blend ? 1 : 0;
        };
        std::stable_sort(draws.begin(), draws.end(), [&](const Draw& a, const Draw& b) {
            const int ra = rank(a);
            const int rb = rank(b);
            if (ra != rb) {
                return ra < rb;
            }
            // Within a rank: the translucent and the blended particles far to near; the rest keep their order.
            return (ra == 1 || ra == 2) && a.depth > b.depth;
        });
        SDL_GPUGraphicsPipeline* bound = nullptr;
        for (const Draw& d : draws) {
            if (d.count <= 0) {
                continue;
            }
            // Particles: no texture, their look comes from their coordinates (view3d_particle).
            if (d.kind != Kind::Scene) {
                SDL_GPUGraphicsPipeline* p = d.kind == Kind::ParticlesAdded ? particlesAdded_ : particlesBlended_;
                if (p != bound) {
                    SDL_BindGPUGraphicsPipeline(pass, p);
                    bound = p;
                }
                SDL_DrawGPUPrimitives(pass, static_cast<Uint32>(d.count), 1, static_cast<Uint32>(d.first), 0);
                continue;
            }
            SDL_GPUTexture* texture = gpuTexture(d.texture);
            if (!texture) {
                continue;
            }
            // A material sprite (tint) has its colours applied by the material shader.
            const bool material = d.sprite && d.sprite->tint;
            SDL_GPUGraphicsPipeline* p = material ? (d.blend ? blendMaterial_ : opaqueMaterial_)
                                                  : (d.blend ? blendPlain_ : opaquePlain_);
            if (p != bound) {
                SDL_BindGPUGraphicsPipeline(pass, p);
                bound = p;
            }
            // A material sprite's material bytes: beside its image in the same texture, or (a BC7
            // image) on a page of their own.
            const SDL_GPUTextureSamplerBinding samplers[2] = {
                {texture, d.linear && !material ? linear_ : nearest_},
                {material && d.sprite->labelTexture ? d.sprite->labelTexture : texture, nearest_}};
            SDL_BindGPUFragmentSamplers(pass, 0, samplers, material ? 2 : 1);
            const float cut = d.blend ? kBlendCut : kOpaqueCut;
            if (material) {
                // The material shader's uniforms (view3d_material.frag.hlsl): the 7 material colours, the
                // image's width and height and the cut-off, its place in the texture and the texture's
                // size, and its material bytes' place.
                struct {
                    float colour[7][4];
                    float params[4];
                    float place[4];
                    float labels[4];
                } u{};
                for (std::size_t i = 0; i < 7; ++i) {
                    for (std::size_t c = 0; c < 3; ++c) {
                        u.colour[i][c] = static_cast<float>((*d.sprite->tint)[i][c]);
                    }
                }
                float tw = 0, th = 0;
                SDL_GetTextureSize(d.texture, &tw, &th);
                u.params[1] = d.sprite->imageWidth();
                u.params[2] = d.sprite->rect.h;
                u.params[3] = cut;
                u.place[0] = d.sprite->rect.x;
                u.place[1] = d.sprite->rect.y;
                u.place[2] = tw;
                u.place[3] = th;
                u.labels[0] = d.sprite->labelTexture ? d.sprite->labels.x : d.sprite->rect.x + d.sprite->imageWidth();
                u.labels[1] = d.sprite->labelTexture ? d.sprite->labels.y : d.sprite->rect.y;
                SDL_PushGPUFragmentUniformData(cmd, 0, &u, sizeof(u));
            } else {
                const float params[4] = {cut, 0, 0, 0};
                SDL_PushGPUFragmentUniformData(cmd, 0, params, sizeof(params));
            }
            SDL_DrawGPUPrimitives(pass, static_cast<Uint32>(d.count), 1, static_cast<Uint32>(d.first), 0);
        }
    }
    if (pass) {
        SDL_EndGPURenderPass(pass);
    }
    SDL_SubmitGPUCommandBuffer(cmd);
    return scene_;
}

} // namespace noxsdl
