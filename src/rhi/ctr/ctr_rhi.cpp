// DR. ROBOTNIK'S RING RACERS
//-----------------------------------------------------------------------------
// Copyright (C) 2025 by Kart Krew
//
// This program is free software distributed under the
// terms of the GNU General Public License, version 2.
// See the 'LICENSE' file for more details.
//-----------------------------------------------------------------------------

#ifdef __3DS__

#include "ctr_rhi.hpp"

#include <algorithm>
#include <cstring>

using namespace srb2::rhi;

// ---------------------------------------------------------------------------
// Screen constants
// ---------------------------------------------------------------------------

static constexpr uint32_t kScreenW = 400;
static constexpr uint32_t kScreenH = 240;

static uint32_t next_pow2(uint32_t n)
{
    if (n == 0) return 1;
    n--;
    n |= n >> 1; n |= n >> 2; n |= n >> 4; n |= n >> 8; n |= n >> 16;
    return n + 1;
}

// ---------------------------------------------------------------------------
// Constructor / Destructor
// ---------------------------------------------------------------------------

CtrRhi::CtrRhi()
{
    // C3D_DEFAULT_CMDBUF_SIZE is 512 KB - unnecessary for the software renderer.
    // 128 KB is enough for the C2D blit we do.
    C3D_Init(0x20000); // 128 KB
    C2D_Init(C2D_DEFAULT_MAX_OBJECTS);
    C2D_Prepare();

    // C3D_RenderTargetCreate(height, width, ...) - screen rotated internally
    top_target_ = C3D_RenderTargetCreate(kScreenH, kScreenW,
                                         GPU_RB_RGBA8, GPU_RB_DEPTH24_STENCIL8);
    C3D_RenderTargetSetOutput(top_target_, GFX_TOP, GFX_LEFT,
        GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) |
        GX_TRANSFER_RAW_COPY(0) |
        GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) |
        GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB8) |
        GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO));
}

CtrRhi::~CtrRhi()
{
    // C3D textures live in linear VRAM; C3D_Fini() cleans them up on exit.
    // We do not iterate Slab<> because it has no standard range-for.

    if (top_target_)
    {
        C3D_RenderTargetDelete(top_target_);
        top_target_ = nullptr;
    }
    C2D_Fini();
    C3D_Fini();
}

// ---------------------------------------------------------------------------
// Resources — Programs
// ---------------------------------------------------------------------------

srb2::rhi::Handle<Program> CtrRhi::create_program(const ProgramDesc& /*desc*/)
{
    return programs_.insert(CtrProgram {});
}

void CtrRhi::destroy_program(Handle<Program> handle)
{
    programs_.remove(handle);
}

// ---------------------------------------------------------------------------
// Resources — Textures
// ---------------------------------------------------------------------------

srb2::rhi::Handle<Texture> CtrRhi::create_texture(const TextureDesc& desc)
{
    CtrTexture ct;
    ct.width  = desc.width;
    ct.height = desc.height;
    ct.format = desc.format;
    ct.valid  = false;

    uint32_t tw = next_pow2(desc.width);
    uint32_t th = next_pow2(desc.height);

    GPU_TEXCOLOR gpu_fmt;
    switch (desc.format)
    {
    case TextureFormat::kRGBA:           gpu_fmt = GPU_RGBA8; break;
    case TextureFormat::kRGB:            gpu_fmt = GPU_RGB8;  break;
    case TextureFormat::kLuminanceAlpha: gpu_fmt = GPU_LA8;   break;
    case TextureFormat::kLuminance:      gpu_fmt = GPU_L8;    break;
    default:                             gpu_fmt = GPU_RGBA8; break;
    }

    if (C3D_TexInit(&ct.tex, (uint16_t)tw, (uint16_t)th, gpu_fmt))
    {
        ct.valid = true;
        GPU_TEXTURE_WRAP_PARAM wu = desc.u_wrap == TextureWrapMode::kRepeat
                                    ? GPU_REPEAT : GPU_CLAMP_TO_BORDER;
        GPU_TEXTURE_WRAP_PARAM wv = desc.v_wrap == TextureWrapMode::kRepeat
                                    ? GPU_REPEAT : GPU_CLAMP_TO_BORDER;
        GPU_TEXTURE_FILTER_PARAM fmag = desc.mag == TextureFilterMode::kLinear
                                        ? GPU_LINEAR : GPU_NEAREST;
        GPU_TEXTURE_FILTER_PARAM fmin = desc.min == TextureFilterMode::kLinear
                                        ? GPU_LINEAR : GPU_NEAREST;
        C3D_TexSetWrap(&ct.tex, wu, wv);
        C3D_TexSetFilter(&ct.tex, fmag, fmin);
        std::memset(ct.tex.data, 0, ct.tex.size);
        GSPGPU_FlushDataCache(ct.tex.data, ct.tex.size);
    }

    return textures_.insert(std::move(ct));
}

void CtrRhi::destroy_texture(Handle<Texture> handle)
{
    if (textures_.is_valid(handle))
    {
        auto& ct = textures_[handle];
        if (ct.valid) C3D_TexDelete(&ct.tex);
        textures_.remove(handle);
    }
    if (screen_tex_ == handle)
        screen_tex_ = kNullHandle;
}

// ---------------------------------------------------------------------------
// Resources — Buffers
// ---------------------------------------------------------------------------

srb2::rhi::Handle<Buffer> CtrRhi::create_buffer(const BufferDesc& desc)
{
    CtrBuffer cb;
    cb.data.resize(desc.size, 0);
    return buffers_.insert(std::move(cb));
}

void CtrRhi::destroy_buffer(Handle<Buffer> handle)
{
    buffers_.remove(handle);
}

// ---------------------------------------------------------------------------
// Resources — Renderbuffers
// ---------------------------------------------------------------------------

srb2::rhi::Handle<Renderbuffer> CtrRhi::create_renderbuffer(const RenderbufferDesc& desc)
{
    CtrRenderbuffer cr;
    cr.width  = desc.width;
    cr.height = desc.height;
    return renderbuffers_.insert(std::move(cr));
}

void CtrRhi::destroy_renderbuffer(Handle<Renderbuffer> handle)
{
    renderbuffers_.remove(handle);
}

// ---------------------------------------------------------------------------
// Queries
// ---------------------------------------------------------------------------

TextureDetails CtrRhi::get_texture_details(Handle<Texture> texture)
{
    if (!textures_.is_valid(texture))
        return {0, 0, TextureFormat::kRGBA};
    auto& ct = textures_[texture];
    return {ct.width, ct.height, ct.format};
}

Rect CtrRhi::get_renderbuffer_size(Handle<Renderbuffer> renderbuffer)
{
    if (!renderbuffers_.is_valid(renderbuffer))
        return {0, 0, 0, 0};
    auto& cr = renderbuffers_[renderbuffer];
    return {0, 0, cr.width, cr.height};
}

uint32_t CtrRhi::get_buffer_size(Handle<Buffer> buffer)
{
    if (!buffers_.is_valid(buffer))
        return 0;
    return (uint32_t)buffers_[buffer].data.size();
}

// ---------------------------------------------------------------------------
// Upload — Buffer
// ---------------------------------------------------------------------------

void CtrRhi::update_buffer(Handle<Buffer> buffer, uint32_t offset,
                            tcb::span<const std::byte> data)
{
    if (!buffers_.is_valid(buffer)) return;
    auto& cb = buffers_[buffer];
    if (offset + data.size() > cb.data.size())
        cb.data.resize(offset + data.size(), 0);
    std::memcpy(cb.data.data() + offset, data.data(), data.size());
}

// ---------------------------------------------------------------------------
// Upload - Texture (Morton tiling for 3DS)
// ---------------------------------------------------------------------------

// Interleaves x/y bits in Morton order for the 3DS GPU's 8x8 tiles
static uint32_t morton_offset(uint32_t x, uint32_t y)
{
    // We only use the low 3 bits (8x8 tile)
    x &= 7; y &= 7;
    // Precomputed table equivalent to the bit interleave
    static const uint8_t kMorton[8] = {0, 1, 4, 5, 16, 17, 20, 21};
    return kMorton[x] + kMorton[y] * 2;
}

// Writes an RGBA8 pixel with correct tiling into the C3D texture
// Memory format: ABGR (big-endian per pixel, little-endian in the word)
static void write_pixel_rgba8(uint8_t* dst, uint32_t tex_w, uint32_t tex_h,
                               uint32_t px, uint32_t py,
                               uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    // The 3DS flips Y
    uint32_t fy = (tex_h - 1) - py;
    uint32_t tile_x = px >> 3;
    uint32_t tile_y = fy >> 3;
    uint32_t tiles_per_row = tex_w >> 3;
    uint32_t tile_idx = tile_y * tiles_per_row + tile_x;
    uint32_t in_tile = morton_offset(px & 7, fy & 7);
    uint32_t pixel_idx = tile_idx * 64 + in_tile;
    uint8_t* p = dst + pixel_idx * 4;
    p[0] = a; p[1] = b; p[2] = g; p[3] = r;
}

void CtrRhi::upload_rgba_to_c3d(C3D_Tex& tex, uint32_t tex_w, uint32_t tex_h,
                                  uint32_t src_w, uint32_t src_h, const uint8_t* src)
{
    uint32_t cw = std::min(src_w, tex_w);
    uint32_t ch = std::min(src_h, tex_h);
    uint8_t* d = (uint8_t*)tex.data;
    for (uint32_t y = 0; y < ch; y++)
        for (uint32_t x = 0; x < cw; x++)
        {
            const uint8_t* px = src + (y * src_w + x) * 4;
            write_pixel_rgba8(d, tex_w, tex_h, x, y, px[0], px[1], px[2], px[3]);
        }
    GSPGPU_FlushDataCache(tex.data, tex.size);
}

void CtrRhi::upload_rgb_to_c3d(C3D_Tex& tex, uint32_t tex_w, uint32_t tex_h,
                                 uint32_t src_w, uint32_t src_h, const uint8_t* src)
{
    uint32_t cw = std::min(src_w, tex_w);
    uint32_t ch = std::min(src_h, tex_h);
    uint8_t* d = (uint8_t*)tex.data;
    for (uint32_t y = 0; y < ch; y++)
        for (uint32_t x = 0; x < cw; x++)
        {
            const uint8_t* px = src + (y * src_w + x) * 3;
            write_pixel_rgba8(d, tex_w, tex_h, x, y, px[0], px[1], px[2], 0xFF);
        }
    GSPGPU_FlushDataCache(tex.data, tex.size);
}

void CtrRhi::update_texture(Handle<Texture> texture, Rect region,
                             PixelFormat data_format,
                             tcb::span<const std::byte> data)
{
    if (!textures_.is_valid(texture)) return;
    CtrTexture& ct = textures_[texture];
    if (!ct.valid) return;

    const uint8_t* src = reinterpret_cast<const uint8_t*>(data.data());
    uint32_t tw = (uint32_t)ct.tex.width;
    uint32_t th = (uint32_t)ct.tex.height;

    if (data_format == PixelFormat::kRGBA8)
    {
        upload_rgba_to_c3d(ct.tex, tw, th, (uint32_t)region.w, (uint32_t)region.h, src);
        screen_tex_ = texture;
    }
    else if (data_format == PixelFormat::kRGB8)
    {
        upload_rgb_to_c3d(ct.tex, tw, th, (uint32_t)region.w, (uint32_t)region.h, src);
        screen_tex_ = texture;
    }
    else
    {
        // kR8, kRG8: direct copy (palettes/colormaps, they do not go to the screen)
        size_t sz = std::min(data.size(), (size_t)ct.tex.size);
        std::memcpy(ct.tex.data, src, sz);
        GSPGPU_FlushDataCache(ct.tex.data, ct.tex.size);
    }
}

void CtrRhi::update_texture_settings(Handle<Texture> texture,
                                      TextureWrapMode u_wrap, TextureWrapMode v_wrap,
                                      TextureFilterMode min_f, TextureFilterMode mag_f)
{
    if (!textures_.is_valid(texture)) return;
    C3D_Tex& tex = textures_[texture].tex;
    C3D_TexSetWrap(&tex,
        u_wrap == TextureWrapMode::kRepeat ? GPU_REPEAT : GPU_CLAMP_TO_BORDER,
        v_wrap == TextureWrapMode::kRepeat ? GPU_REPEAT : GPU_CLAMP_TO_BORDER);
    C3D_TexSetFilter(&tex,
        mag_f == TextureFilterMode::kLinear ? GPU_LINEAR : GPU_NEAREST,
        min_f == TextureFilterMode::kLinear ? GPU_LINEAR : GPU_NEAREST);
}

// ---------------------------------------------------------------------------
// Render pass
// ---------------------------------------------------------------------------

void CtrRhi::push_default_render_pass(bool clear)
{
    if (!frame_active_)
    {
        C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
        frame_active_ = true;
    }
    C3D_FrameDrawOn(top_target_);
    if (clear)
        C3D_RenderTargetClear(top_target_, C3D_CLEAR_ALL, 0x000000FF, 0);
    pass_depth_++;
}

void CtrRhi::push_render_pass(const RenderPassBeginInfo& /*info*/)
{
    // Offscreen render pass: ignored - the software renderer draws on the CPU
    pass_depth_++;
}

void CtrRhi::pop_render_pass()
{
    if (pass_depth_ > 0) pass_depth_--;
}

// ---------------------------------------------------------------------------
// Final blit to the screen
// ---------------------------------------------------------------------------

void CtrRhi::blit_screen_texture()
{
    if (screen_tex_ == kNullHandle) return;
    if (!textures_.is_valid(screen_tex_)) return;
    CtrTexture& ct = textures_[screen_tex_];
    if (!ct.valid) return;

    C2D_Image img;
    img.tex = &ct.tex;

    Tex3DS_SubTexture sub;
    sub.width  = (uint16_t)ct.width;
    sub.height = (uint16_t)ct.height;
    sub.left   = 0.0f;
    sub.top    = 0.0f;
    sub.right  = (float)ct.width  / (float)ct.tex.width;
    sub.bottom = (float)ct.height / (float)ct.tex.height;
    img.subtex = &sub;

    float sx = (float)kScreenW / (float)ct.width;
    float sy = (float)kScreenH / (float)ct.height;
    C2D_DrawImageAt(img, 0.0f, 0.0f, 0.5f, nullptr, sx, sy);
}

// ---------------------------------------------------------------------------
// Present / Finish
// ---------------------------------------------------------------------------

void CtrRhi::present()
{
    if (!frame_active_) return;

    C2D_SceneBegin(top_target_);
    blit_screen_texture();
    C2D_Flush();

    C3D_FrameEnd(0);
    frame_active_ = false;
    pass_depth_   = 0;

    // Must call aptMainLoop() once per frame so the 3DS OS can process
    // Home button, sleep, etc. Without this the process hangs after boot.
    if (!aptMainLoop())
    {
        // User pressed Home / system wants to exit — clean shutdown
        extern void I_Quit(void);
        I_Quit();
    }
}

void CtrRhi::finish()
{
    // C3D_FRAME_SYNCDRAW already syncs with VBlank in present()
}

#endif // __3DS__
