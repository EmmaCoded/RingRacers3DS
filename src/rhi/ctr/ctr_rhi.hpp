// DR. ROBOTNIK'S RING RACERS
//-----------------------------------------------------------------------------
// Copyright (C) 2025 by Kart Krew
//
// This program is free software distributed under the
// terms of the GNU General Public License, version 2.
// See the 'LICENSE' file for more details.
//-----------------------------------------------------------------------------
// Nintendo 3DS RHI backend — citro3d/citro2d

#ifndef __SRB2_RHI_CTR_CTR_RHI_HPP__
#define __SRB2_RHI_CTR_CTR_RHI_HPP__

#ifdef __3DS__

#include <cstdint>
#include <vector>

#include <3ds.h>
#include <citro3d.h>
#include <citro2d.h>

#include "../rhi.hpp"
#include "../handle.hpp"

namespace srb2::rhi
{

// ---------------------------------------------------------------------------
// Internal resources
// ---------------------------------------------------------------------------

struct CtrTexture : public Texture
{
    C3D_Tex   tex    = {};
    uint32_t  width  = 0;
    uint32_t  height = 0;
    TextureFormat format = TextureFormat::kRGBA;
    bool      valid  = false;
};

struct CtrBuffer : public Buffer
{
    std::vector<uint8_t> data;
};

struct CtrRenderbuffer : public Renderbuffer
{
    uint32_t width  = 0;
    uint32_t height = 0;
};

struct CtrProgram : public Program {};

// ---------------------------------------------------------------------------
// CtrRhi - software backend on top of citro3d/citro2d
// ---------------------------------------------------------------------------

class CtrRhi : public Rhi
{
public:
    CtrRhi();
    ~CtrRhi() override;

    Handle<Program>      create_program(const ProgramDesc& desc) override;
    void                 destroy_program(Handle<Program> handle) override;

    Handle<Texture>      create_texture(const TextureDesc& desc) override;
    void                 destroy_texture(Handle<Texture> handle) override;

    Handle<Buffer>       create_buffer(const BufferDesc& desc) override;
    void                 destroy_buffer(Handle<Buffer> handle) override;

    Handle<Renderbuffer> create_renderbuffer(const RenderbufferDesc& desc) override;
    void                 destroy_renderbuffer(Handle<Renderbuffer> handle) override;

    TextureDetails get_texture_details(Handle<Texture> texture) override;
    Rect           get_renderbuffer_size(Handle<Renderbuffer> renderbuffer) override;
    uint32_t       get_buffer_size(Handle<Buffer> buffer) override;

    void update_buffer(Handle<Buffer> buffer, uint32_t offset,
                       tcb::span<const std::byte> data) override;
    void update_texture(Handle<Texture> texture, Rect region,
                        PixelFormat data_format,
                        tcb::span<const std::byte> data) override;
    void update_texture_settings(Handle<Texture> texture,
                                 TextureWrapMode u_wrap, TextureWrapMode v_wrap,
                                 TextureFilterMode min, TextureFilterMode mag) override;

    void push_default_render_pass(bool clear) override;
    void push_render_pass(const RenderPassBeginInfo& info) override;
    void pop_render_pass() override;

    void bind_program(Handle<Program> program) override {}
    void bind_vertex_attrib(const char*, Handle<Buffer>, VertexAttributeFormat,
                            uint32_t, uint32_t) override {}
    void bind_index_buffer(Handle<Buffer>) override {}

    void set_uniform(const char*, float)      override {}
    void set_uniform(const char*, int)        override {}
    void set_uniform(const char*, glm::vec2)  override {}
    void set_uniform(const char*, glm::vec3)  override {}
    void set_uniform(const char*, glm::vec4)  override {}
    void set_uniform(const char*, glm::ivec2) override {}
    void set_uniform(const char*, glm::ivec3) override {}
    void set_uniform(const char*, glm::ivec4) override {}
    void set_uniform(const char*, glm::mat2)  override {}
    void set_uniform(const char*, glm::mat3)  override {}
    void set_uniform(const char*, glm::mat4)  override {}

    void set_sampler(const char*, uint32_t, Handle<Texture>) override {}
    void set_rasterizer_state(const RasterizerStateDesc&) override {}
    void set_viewport(const Rect&) override {}

    void draw(uint32_t, uint32_t) override {}
    void draw_indexed(uint32_t, uint32_t) override {}
    void read_pixels(const Rect&, PixelFormat, tcb::span<std::byte>) override {}
    void copy_framebuffer_to_texture(Handle<Texture>, const Rect&, const Rect&) override {}
    void set_stencil_reference(CullMode, uint8_t)   override {}
    void set_stencil_compare_mask(CullMode, uint8_t) override {}
    void set_stencil_write_mask(CullMode, uint8_t)  override {}

    void present() override;
    void finish()  override;

private:
    // Resource slabs (id and generation managed by HandlePool)
    HandlePool<Texture>      tex_pool_;
    HandlePool<Buffer>       buf_pool_;
    HandlePool<Renderbuffer> rb_pool_;
    HandlePool<Program>      prog_pool_;

    Slab<CtrTexture>      textures_;
    Slab<CtrBuffer>       buffers_;
    Slab<CtrRenderbuffer> renderbuffers_;
    Slab<CtrProgram>      programs_;

    C3D_RenderTarget* top_target_ = nullptr;

    // Handle to the most recent texture uploaded as a framebuffer (kRGB8/kRGBA8)
    Handle<Texture> screen_tex_ = kNullHandle;

    bool frame_active_ = false;
    int  pass_depth_   = 0;

    void upload_rgba_to_c3d(C3D_Tex& tex, uint32_t tex_w, uint32_t tex_h,
                             uint32_t src_w, uint32_t src_h, const uint8_t* src);
    void upload_rgb_to_c3d(C3D_Tex& tex, uint32_t tex_w, uint32_t tex_h,
                            uint32_t src_w, uint32_t src_h, const uint8_t* src);
    void blit_screen_texture();
};

} // namespace srb2::rhi

#endif // __3DS__
#endif // __SRB2_RHI_CTR_CTR_RHI_HPP__
