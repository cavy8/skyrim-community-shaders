struct GS_INPUT
{
	float4 Position: SV_POSITION0;
	float2 TexCoord: TEXCOORD0;
};

struct GS_OUTPUT
{
	float4 Position: SV_POSITION0;
	float2 TexCoord: TEXCOORD0;
	uint Slice: SV_RenderTargetArrayIndex;
};

#if defined(GEOMETRYSHADER)
cbuffer LODShadowCaptureCB : register(b0)
{
	float4 CascadeRowX[3];
	float4 CascadeRowY[3];
	float4 DepthRow;
};

[maxvertexcount(9)] void main(triangle GS_INPUT input[3], inout TriangleStream<GS_OUTPUT> output) {
	float4 faceClip[3];
	float depth[3];
	[unroll] for (uint i = 0; i < 3; i++)
	{
		faceClip[i] = float4(input[i].Position.xyw, 1.0);
		depth[i] = dot(DepthRow, faceClip[i]);
	}

	[unroll] for (uint cascade = 0; cascade < 3; cascade++)
	{
		float2 position[3];
		[unroll] for (uint j = 0; j < 3; j++)
			position[j] = float2(dot(CascadeRowX[cascade], faceClip[j]), dot(CascadeRowY[cascade], faceClip[j]));

		float2 lower = min(position[0], min(position[1], position[2]));
		float2 upper = max(position[0], max(position[1], position[2]));
		if (any(lower > 1.0) || any(upper < -1.0))
			continue;

		[unroll] for (uint k = 0; k < 3; k++)
		{
			GS_OUTPUT vertex;
			vertex.Position = float4(position[k], depth[k], 1.0);
			vertex.TexCoord = input[k].TexCoord;
			vertex.Slice = cascade;
			output.Append(vertex);
		}
		output.RestartStrip();
	}
}
#endif

#if defined(PSHADER)
Texture2D<float4> TexDiffuse : register(t0);
SamplerState SampDiffuse : register(s0);

cbuffer AlphaTestRefCB : register(b11)
{
	float AlphaTestRefRS : packoffset(c0);
}

float4 main(GS_INPUT input) : SV_Target0
{
#	if defined(ALPHA_TEST)
	if (TexDiffuse.Sample(SampDiffuse, input.TexCoord).w - AlphaTestRefRS < 0)
		discard;
#	endif
	return 1.0;
}
#endif
