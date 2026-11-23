// DR. ROBOTNIK'S RING RACERS
//-----------------------------------------------------------------------------
// Copyright (C) 2025 by Ronald "Eidolon" Kinard
// Copyright (C) 2025 by Kart Krew
//
// This program is free software distributed under the
// terms of the GNU General Public License, version 2.
// See the 'LICENSE' file for more details.
//-----------------------------------------------------------------------------

#include "twodee.hpp"

#include "../w_wad.h"

using namespace srb2;
using namespace hwr2;

#ifdef __3DS__
#include "../doomdef.h"
#include "../command.h"
// [TWODEE] log only when a peak rises (verts/idx/cmds/lists per frame)
void srb2::hwr2::twodee_3ds_note(std::size_t lists, std::size_t verts, std::size_t idx, std::size_t cmds, std::size_t vsz, std::size_t csz)
{
	static std::size_t pl, pv, pi, pc;
	if (lists <= pl && verts <= pv && idx <= pi && cmds <= pc) return;
	if (lists > pl) pl = lists; if (verts > pv) pv = verts; if (idx > pi) pi = idx; if (cmds > pc) pc = cmds;
	CONS_Printf("[TWODEE] peak lists=%u verts=%u idx=%u cmds=%u (vert=%uB cmd=%uB; reserved %u/%u/%u)\n",
		(unsigned)pl, (unsigned)pv, (unsigned)pi, (unsigned)pc, (unsigned)vsz, (unsigned)csz,
		(unsigned)kRR3dsVertCap, (unsigned)kRR3dsIdxCap, (unsigned)kRR3dsCmdCap);
}
#endif

Twodee::Twodee() = default;
Twodee::Twodee(const Twodee&) = default;
Twodee::Twodee(Twodee&&) noexcept = default;
Twodee& Twodee::operator=(const Twodee&) = default;

// Will the default move prevent the vectors from losing their allocations? I guess it depends on the STL impl.
// It's probably worth optimizing around.
Twodee& Twodee::operator=(Twodee&&) noexcept = default;

void Draw2dQuadBuilder::done()
{
	if (ctx_.lists_.size() == 0)
	{
		ctx_.lists_.push_back({});
	}

	if (ctx_.lists_.rbegin()->vertices.size() >= (Draw2dList::kMaxVertices - 4))
	{
		// The current draw list has too many vertices to fit this command
		ctx_.lists_.push_back({});
	}

	auto& list = *ctx_.lists_.rbegin();
	quad_.begin_element = list.vertices.size();
	quad_.begin_index = list.vertices.size();

	list.vertices.push_back({quad_.xmin, quad_.ymin, 0.f, 0, 0, quad_.r, quad_.g, quad_.b, quad_.a});
	list.vertices.push_back({quad_.xmax, quad_.ymin, 0.f, 1, 0, quad_.r, quad_.g, quad_.b, quad_.a});
	list.vertices.push_back({quad_.xmax, quad_.ymax, 0.f, 1, 1, quad_.r, quad_.g, quad_.b, quad_.a});
	list.vertices.push_back({quad_.xmin, quad_.ymax, 0.f, 0, 1, quad_.r, quad_.g, quad_.b, quad_.a});

	list.indices.push_back(quad_.begin_element + 0);
	list.indices.push_back(quad_.begin_element + 1);
	list.indices.push_back(quad_.begin_element + 2);

	list.indices.push_back(quad_.begin_element + 0);
	list.indices.push_back(quad_.begin_element + 2);
	list.indices.push_back(quad_.begin_element + 3);

	list.cmds.push_back(quad_);
}

void Draw2dVerticesBuilder::done()
{
	if (ctx_.lists_.size() == 0)
	{
		ctx_.lists_.push_back({});
	}

	if (ctx_.lists_.rbegin()->vertices.size() >= (Draw2dList::kMaxVertices - 4))
	{
		// The current draw list has too many vertices to fit this command
		ctx_.lists_.push_back({});
	}

	auto& list = *ctx_.lists_.rbegin();
	tris_.begin_element = list.vertices.size();
	tris_.begin_index = list.indices.size();

	if (verts_.empty())
	{
		return;
	}

	std::size_t i = 0;
	for (auto& vert : verts_)
	{
		list.vertices.push_back({vert[0], vert[1], 0, vert[2], vert[3], r_, g_, b_, a_});
		list.indices.push_back(tris_.begin_element + i);
		i++;
	}

	list.cmds.push_back(tris_);
}

BlendMode srb2::hwr2::get_blend_mode(const Draw2dCmd& cmd) noexcept
{
	auto visitor = srb2::Overload {
		[&](const Draw2dPatchQuad& cmd) { return cmd.blend; },
		[&](const Draw2dVertices& cmd) { return cmd.blend; }};
	return std::visit(visitor, cmd);
}

bool srb2::hwr2::is_draw_lines(const Draw2dCmd& cmd) noexcept
{
	auto visitor = srb2::Overload {
		[&](const Draw2dPatchQuad& cmd) { return false; },
		[&](const Draw2dVertices& cmd) { return cmd.lines; }};
	return std::visit(visitor, cmd);
}

std::size_t srb2::hwr2::elements(const Draw2dCmd& cmd) noexcept
{
	auto visitor = srb2::Overload {
		[&](const Draw2dPatchQuad& cmd) -> std::size_t { return 6; },
		[&](const Draw2dVertices& cmd) -> std::size_t { return cmd.elements; }};
	return std::visit(visitor, cmd);
}
