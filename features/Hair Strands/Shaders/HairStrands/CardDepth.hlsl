// Hair Strands: the depth prepass's pixel shader for the parts of a hair kept as cards. It runs
// after CardLighting.hlsl's vertex shader (it reads the position and texture coordinates, the
// first two outputs) and keeps only what the hair texture's alpha shows, as the game's own
// alpha-tested depth pass (Utility.hlsl, RENDER_DEPTH with ALPHA_TEST) does: with the texture,
// sampler and threshold that pass binds. Passes that do not alpha-test draw the cards without a
// pixel shader (StrandRenderer::DrawCards).

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

void main(PS_INPUT input)
{
	if (TexBaseSampler.Sample(SampBaseSampler, input.TexCoord0).w - AlphaTestRef.x < 0)
		discard;
}
