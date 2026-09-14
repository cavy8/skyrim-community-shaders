#pragma once

#include <d3d11.h>

namespace NeuralRenderingCategories
{
	// Masks2 (R16G16_UNORM, RT7) keeps upstream's vertex AO in R and stores the Neural Rendering
	// material category in G, so the category never touches the AO the deferred composite reads.
	inline constexpr UINT8 kVertexAOWriteMask = D3D11_COLOR_WRITE_ENABLE_RED;
	inline constexpr UINT8 kCategoryWriteMask = D3D11_COLOR_WRITE_ENABLE_GREEN;

	/**
	 * @brief Masks2's write mask in the deferred blend table (Deferred::OverrideBlendStates).
	 *
	 * AO follows RT0 exactly as upstream. The category is written only by unblended draws: a
	 * blended draw's single source alpha drives both channels, and a lerp of two category ids is
	 * a third category. Upscaling redraws those passes into the category alone with binary
	 * coverage (MakeCategoryRedrawBlendDesc).
	 */
	inline UINT8 GetDeferredMasks2WriteMask(const D3D11_RENDER_TARGET_BLEND_DESC& a_rt0)
	{
		const UINT8 vertexAO = a_rt0.RenderTargetWriteMask & kVertexAOWriteMask;
		return a_rt0.BlendEnable ? vertexAO : static_cast<UINT8>(vertexAO | (vertexAO ? kCategoryWriteMask : 0));
	}

	/** @brief Blends Masks2's category so binary coverage in o7.a keeps exactly one surface's id. */
	inline void SetCategoryBlend(D3D11_RENDER_TARGET_BLEND_DESC& a_target, BOOL a_blendEnable, UINT8 a_writeMask)
	{
		a_target.BlendEnable = a_blendEnable;
		a_target.SrcBlend = D3D11_BLEND_SRC_ALPHA;
		a_target.DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
		a_target.BlendOp = D3D11_BLEND_OP_ADD;
		a_target.SrcBlendAlpha = D3D11_BLEND_ONE;
		a_target.DestBlendAlpha = D3D11_BLEND_ZERO;
		a_target.BlendOpAlpha = D3D11_BLEND_OP_ADD;
		a_target.RenderTargetWriteMask = a_writeMask;
	}

	/** @brief Copies RT0's rule to every target when a state does not blend targets independently. */
	inline D3D11_BLEND_DESC MakeIndependent(D3D11_BLEND_DESC a_desc)
	{
		if (!a_desc.IndependentBlendEnable) {
			for (auto& target : a_desc.RenderTarget)
				target = a_desc.RenderTarget[0];
		}
		a_desc.IndependentBlendEnable = true;
		return a_desc;
	}

	/**
	 * @brief Derives the state for a blended deferred pass's category redraw from its deferred state.
	 *
	 * Every GBuffer target and Masks2's AO are masked off, so the redraw changes nothing upstream
	 * reads; Masks2's category takes a source-alpha blend over the binary coverage Lighting.hlsl
	 * outputs under ExtraFlags::NeuralCategoryRedraw. Alpha-to-coverage is kept, since it shapes
	 * the coverage the first draw had.
	 */
	inline D3D11_BLEND_DESC MakeCategoryRedrawBlendDesc(D3D11_BLEND_DESC a_desc)
	{
		a_desc = MakeIndependent(a_desc);
		for (unsigned i = 0; i < 7; ++i)
			a_desc.RenderTarget[i].RenderTargetWriteMask = 0;
		SetCategoryBlend(a_desc.RenderTarget[7], true, kCategoryWriteMask);
		return a_desc;
	}

	/**
	 * @brief Derives a forward state that also writes Masks2's category, keeping RT0-6's rules.
	 *
	 * Used while Upscaling binds Masks2 around forward lighting draws (Deferred::ResetBlendStates).
	 * AO is masked off; colour blend modes such as ONE/ONE or DEST_COLOR/ZERO would do arithmetic
	 * on the category even at full coverage, so it takes a source-alpha blend when RT0 blends and an
	 * overwrite when RT0 is opaque.
	 */
	inline D3D11_BLEND_DESC MakeForwardCategoryBlendDesc(D3D11_BLEND_DESC a_desc)
	{
		a_desc = MakeIndependent(a_desc);
		const auto& rt0 = a_desc.RenderTarget[0];
		SetCategoryBlend(a_desc.RenderTarget[7], rt0.BlendEnable, (rt0.RenderTargetWriteMask & kVertexAOWriteMask) ? kCategoryWriteMask : 0);
		return a_desc;
	}
}
