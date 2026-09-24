#include "FootstepParticles/Common.hlsli"

RWStructuredBuffer<FootstepParticles::Particle> Particles : register(u0);

static const uint kRestingFlag = 0x200u;

[numthreads(64, 1, 1)] void main(uint3 dispatchID : SV_DispatchThreadID) {
	uint index = dispatchID.x;
	if (index >= FootstepParticles::MaxParticles)
		return;

	FootstepParticles::Particle particle = Particles[index];
	if (particle.Lifetime <= 0.0)
		return;

	float deltaTime = FootstepParticles::DeltaTime;
	particle.Age += deltaTime;
	if (particle.Age >= particle.Lifetime || any(isnan(particle.Position))) {
		particle.Lifetime = 0.0;
		Particles[index] = particle;
		return;
	}

	uint type = particle.Type & 0xFFu;

	if (type == FootstepParticles::TypeClod || type == FootstepParticles::TypeDroplet) {
		if (!(particle.Type & kRestingFlag)) {
			particle.Velocity.z -= FootstepParticles::Gravity * deltaTime;
			particle.Velocity *= exp(-0.4 * deltaTime);
			particle.Position += particle.Velocity * deltaTime;

			if (particle.Position.z <= particle.GroundZ) {
				if (type == FootstepParticles::TypeDroplet) {
					particle.Lifetime = 0.0;
				} else {
					particle.Position.z = particle.GroundZ + particle.Size * 0.3;
					particle.Velocity = 0.0;
					particle.Type |= kRestingFlag;
					particle.Lifetime = min(particle.Lifetime, particle.Age + 0.6 * FootstepParticles::LifetimeScale);
				}
			}
		}
	} else {
		float drag = type == FootstepParticles::TypeSnow ? 2.4 : (type == FootstepParticles::TypeMist ? 3.5 : 2.8);
		float lift = type == FootstepParticles::TypeSnow ? -45.0 : (type == FootstepParticles::TypeMist ? -20.0 : 6.0);
		float damping = 1.0 - exp(-drag * deltaTime);

		particle.Velocity.xy = lerp(particle.Velocity.xy, FootstepParticles::Wind, damping);
		particle.Velocity.z = lerp(particle.Velocity.z, 0.0, damping) + lift * deltaTime;
		particle.Position += particle.Velocity * deltaTime;

		float floorHeight = particle.GroundZ + particle.Size * 0.35;
		if (particle.Position.z < floorHeight) {
			particle.Position.z = floorHeight;
			particle.Velocity.z = max(particle.Velocity.z, 0.0);
		}
	}

	Particles[index] = particle;
}
