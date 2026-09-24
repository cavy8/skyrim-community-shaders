#include "Common/FrameBuffer.hlsli"
#include "Common/Math.hlsli"
#include "FootstepParticles/Common.hlsli"

StructuredBuffer<FootstepParticles::Particle> Particles : register(t0);

struct VS_OUTPUT
{
	float4 Position : SV_POSITION;
	float2 TexCoord : TEXCOORD0;
	nointerpolation float4 Color : COLOR0;
	float3 WorldPosition : POSITION1;
	float4 FogParam : COLOR1;
	nointerpolation uint Type : TEXCOORD1;
	nointerpolation float Seed : TEXCOORD2;
	nointerpolation float Softness : TEXCOORD3;
	float ViewDepth : TEXCOORD4;
};

VS_OUTPUT main(uint vertexID : SV_VertexID, uint instanceID : SV_InstanceID)
{
	VS_OUTPUT output = (VS_OUTPUT)0;
	output.Position = float4(2.0, 2.0, 2.0, 1.0);

	FootstepParticles::Particle particle = Particles[instanceID];
	if (particle.Lifetime <= 0.0)
		return output;

	uint type = particle.Type & 0xFFu;
	float life = saturate(particle.Age / particle.Lifetime);
	float eased = 1.0 - (1.0 - life) * (1.0 - life);
	float size = particle.Size * lerp(1.0, particle.Growth, eased);

	float fade;
	if (type == FootstepParticles::TypeClod || type == FootstepParticles::TypeDroplet)
		fade = 1.0 - smoothstep(0.7, 1.0, life);
	else
		fade = smoothstep(0.0, 0.08, life) * (1.0 - smoothstep(0.3, 1.0, life));

	float seed = FootstepParticles::Hash(instanceID, asuint(particle.GroundZ));
	float2 corner = float2(vertexID & 1u, vertexID >> 1u) * 2.0 - 1.0;
	float3 center = particle.Position - FrameBuffer::CameraPosAdjust.xyz;
	float3 right = FrameBuffer::ViewToWorld(float3(1.0, 0.0, 0.0), false);
	float3 up = FrameBuffer::ViewToWorld(float3(0.0, 1.0, 0.0), false);

	float3 offset = (right * corner.x + up * corner.y) * size;
	if (type == FootstepParticles::TypeDroplet) {
		float3 viewDirection = normalize(center);
		float3 axis = particle.Velocity - dot(particle.Velocity, viewDirection) * viewDirection;
		float axisLength = length(axis);
		if (axisLength > 1e-3) {
			axis /= axisLength;
			float3 side = normalize(cross(axis, viewDirection));
			float stretch = 1.0 + min(axisLength * 0.015, 5.0);
			offset = side * corner.x * size + axis * corner.y * size * stretch;
		}
	} else {
		float spin = type == FootstepParticles::TypeClod ? 6.0 : 0.6;
		float angle = seed * Math::TAU + particle.Age * spin * (seed > 0.5 ? 1.0 : -1.0);
		float s, c;
		sincos(angle, s, c);
		float2 rotated = float2(corner.x * c - corner.y * s, corner.x * s + corner.y * c);
		offset = (right * rotated.x + up * rotated.y) * size;
	}

	float3 positionWS = center + offset;
	output.Position = mul(FrameBuffer::CameraViewProj, float4(positionWS, 1.0));
	output.ViewDepth = output.Position.w;
	output.TexCoord = corner;
	output.Color = float4(particle.Color, particle.Opacity * fade * smoothstep(8.0, 48.0, output.Position.w));
	output.WorldPosition = positionWS;
	output.Type = particle.Type;
	output.Seed = seed;
	output.Softness = (type == FootstepParticles::TypeClod || type == FootstepParticles::TypeDroplet) ? max(size, 1.0) : FootstepParticles::SoftDistance * max(size / 6.0, 0.5);

	output.FogParam = 0.0;
	if (FootstepParticles::FrameFlags & FootstepParticles::FrameFlagFog) {
		float distanceFactor = saturate(length(positionWS) * FootstepParticles::FogParams.y - FootstepParticles::FogParams.x);
		float fogFactor = distanceFactor > 0.0 ? min(FootstepParticles::FogParams.w, exp2(FootstepParticles::FogParams.z * log2(distanceFactor))) : 0.0;
		output.FogParam = float4(lerp(FootstepParticles::FogNearColor.xyz, FootstepParticles::FogFarColor.xyz, fogFactor), fogFactor);
	}

	return output;
}
