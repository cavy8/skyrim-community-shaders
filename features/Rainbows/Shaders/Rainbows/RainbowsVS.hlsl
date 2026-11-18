#include "Common/FrameBuffer.hlsli"
#include "Rainbows/Common.hlsli"

Rainbows::Interpolants main(uint vertexID : SV_VertexID)
{
	float2 uv = float2((vertexID << 1) & 2, vertexID & 2);
	float2 ndc = uv * float2(2.0, -2.0) + float2(-1.0, 1.0);

	Rainbows::Interpolants output;
	output.Position = float4(ndc, 0.0, 1.0);
	output.ViewPoint = mul(FrameBuffer::CameraViewProjInverse, float4(ndc, 0.5, 1.0));
	output.Eye = mul(FrameBuffer::CameraViewInverse, float4(0.0, 0.0, 0.0, 1.0)).xyz;
	return output;
}
