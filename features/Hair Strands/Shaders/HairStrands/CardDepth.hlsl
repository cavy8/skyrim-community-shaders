// Hair Strands: the depth prepass's pixel shader for the parts of a hair kept as cards. It runs
// after CardLighting.hlsl's vertex shader (it reads the position and texture coordinates, the
// first two outputs) and keeps only what the hair texture's alpha shows, as the game's own
// alpha-tested depth pass (Utility.hlsl, RENDER_DEPTH with ALPHA_TEST) does: with the texture,
// sampler, mip bias and thresholds that pass uses. Passes that do not alpha-test draw the cards
// without a pixel shader (StrandRenderer::DrawCards).
//
// The lighting pass shades the cards wherever its own alpha test (Lighting.hlsl) passes, so any
// texel kept here that it discards is depth without colour: the sky shows through. Sampling
// without the mip bias did that, as a blurrier mip keeps the gaps between painted strands.

#include "Common/SharedData.hlsli"

struct PS_INPUT
{
	float4 Position: SV_POSITION0;
	float2 TexCoord0: TEXCOORD0;
};

SamplerState SampBaseSampler : register(s0);
Texture2D<float4> TexBaseSampler : register(t0);

// Utility.hlsl's pixel PerGeometry constants, still bound for the hair's pass.
cbuffer PerGeometry : register(b2)
{
	float4 DebugColor : packoffset(c0);
	float4 PropertyColor : packoffset(c1);
	float4 AlphaTestRef : packoffset(c2);
}

// The render state's alpha reference, the one Lighting.hlsl tests against.
cbuffer AlphaTestRefCB : register(b11)
{
	float AlphaTestRefRS : packoffset(c0);
}

void main(PS_INPUT input)
{
	const float alpha = TexBaseSampler.SampleBias(SampBaseSampler, input.TexCoord0, SharedData::MipBias).w;
	if (alpha - AlphaTestRef.x < 0 || alpha - AlphaTestRefRS < 0)
		discard;
}
