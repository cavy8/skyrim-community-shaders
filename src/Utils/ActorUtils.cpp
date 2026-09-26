#include "ActorUtils.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <string_view>

namespace Util
{
	bool GetShapeBound(RE::bhkNiCollisionObject* collisionObj, RE::NiPoint3& centerPos, float& radius)
	{
		if (!collisionObj)
			return false;

		RE::bhkRigidBody* bhkRigid = collisionObj->body.get() ? collisionObj->body.get()->AsBhkRigidBody() : nullptr;
		RE::hkpRigidBody* hkpRigid = bhkRigid ? skyrim_cast<RE::hkpRigidBody*>(bhkRigid->referencedObject.get()) : nullptr;
		if (bhkRigid && hkpRigid && !skyrim_cast<RE::hkpListShape*>(hkpRigid)) {  // Ignore hkpListShape, unsupported
			RE::hkVector4 massCenter;
			bhkRigid->GetCenterOfMassWorld(massCenter);
			float massTrans[4];
			// Use unaligned store to avoid UB from potential stack misalignment
			_mm_storeu_ps(massTrans, massCenter.quad);
			centerPos = RE::NiPoint3(massTrans[0], massTrans[1], massTrans[2]) * RE::bhkWorld::GetWorldScaleInverse();
			return Util::ExtractShapeBound(hkpRigid->collidable.GetShape(), radius);
		}
		return false;
	}

	bool ExtractShapeBound(const RE::hkpShape* shape, float& radius)
	{
		using ShapeType = RE::hkpShapeType;
		if (!shape)
			return false;

		// Helpers to avoid repeating projection math and ensure offset-invariant half-extents
		auto project = [shape](float x, float y, float z) {
			return shape->GetMaximumProjection(RE::hkVector4{ x, y, z, 0.0f }) * RE::bhkWorld::GetWorldScaleInverse();
		};
		auto symmetricHalfExtents = [&project](float& hx, float& hy, float& hz) {
			float x_pos = project(1.0f, 0.0f, 0.0f);
			float x_neg = project(-1.0f, 0.0f, 0.0f);
			float y_pos = project(0.0f, 1.0f, 0.0f);
			float y_neg = project(0.0f, -1.0f, 0.0f);
			float z_pos = project(0.0f, 0.0f, 1.0f);
			float z_neg = project(0.0f, 0.0f, -1.0f);
			hx = 0.5f * (x_pos - x_neg);
			hy = 0.5f * (y_pos - y_neg);
			hz = 0.5f * (z_pos - z_neg);
		};
		auto halfDiagonal = [](float hx, float hy, float hz) {
			return sqrtf(hx * hx + hy * hy + hz * hz);
		};
		if (shape->type == ShapeType::kCapsule) {
			float hx, hy, hz;
			symmetricHalfExtents(hx, hy, hz);
			// For capsules, use the maximum half-extent (typically hz for vertical orientation)
			// as the farthest point lies along the capsule's main axis, not at the diagonal
			radius = std::max(hx, std::max(hy, hz));
			return true;
		} else if (shape->type == ShapeType::kSphere) {
			// For spheres, any axis should yield the same half-extent; use symmetric X
			float hx, hy, hz;
			symmetricHalfExtents(hx, hy, hz);
			radius = hx;
			return true;
		} else if (shape->type == ShapeType::kBox) {
			float hx, hy, hz;
			symmetricHalfExtents(hx, hy, hz);
			radius = halfDiagonal(hx, hy, hz);
			return true;
		} else if (shape->type == ShapeType::kCylinder) {
			// Use symmetric half-extents; cylinder radius is max of X/Y half-extents
			float hx, hy, hz;
			symmetricHalfExtents(hx, hy, hz);
			float hr = std::max(hx, hy);
			radius = sqrtf(hr * hr + hz * hz);
			return true;
		} else if (shape->type == ShapeType::kConvexVertices || shape->type == ShapeType::kTriangle) {
			// Offset-invariant estimate: take symmetric half-extents per axis and use the max
			float hx, hy, hz;
			symmetricHalfExtents(hx, hy, hz);
			radius = std::max(hx, std::max(hy, hz));
			return true;
		} else {
			// Fallback: mirror the convex/triangle approach for consistency
			float hx, hy, hz;
			symmetricHalfExtents(hx, hy, hz);
			radius = std::max(hx, std::max(hy, hz));
			return true;
		}
	}

	bool IsDragon(const RE::Actor& a_actor, const RE::BGSKeyword* a_dragonKeyword)
	{
		const auto* race = a_actor.GetRace();
		if (!race)
			return false;
		if (a_dragonKeyword)
			return race->HasKeyword(a_dragonKeyword);
		if (race->HasKeywordString("ActorTypeDragon"))
			return true;

		constexpr std::string_view dragonGraph = "dragonbehavior.hkx";
		for (const auto& behaviorGraph : race->behaviorGraphs) {
			const char* model = behaviorGraph.GetModel();
			if (!model)
				continue;
			const std::string_view path(model);
			const auto match = std::search(path.begin(), path.end(), dragonGraph.begin(), dragonGraph.end(),
				[](unsigned char a_left, unsigned char a_right) {
					return std::tolower(a_left) == std::tolower(a_right);
				});
			if (match != path.end())
				return true;
		}
		return false;
	}

	float3 GetVisualOrigin(RE::Actor& a_actor) noexcept
	{
		if (auto* root = a_actor.Get3D(false)) {
			const auto& origin = root->world.translate;
			return { origin.x, origin.y, origin.z };
		}

		auto origin = a_actor.GetPosition();
		origin.z += (a_actor.GetBoundMax().z - a_actor.GetBoundMin().z) * 0.5f;
		return { origin.x, origin.y, origin.z };
	}

	float3 GetMagicOrigin(RE::Actor& a_actor) noexcept
	{
		if (auto* caster = a_actor.GetMagicCaster(RE::MagicSystem::CastingSource::kOther)) {
			if (auto* magicNode = caster->GetMagicNode()) {
				const auto& origin = magicNode->world.translate;
				return { origin.x, origin.y, origin.z };
			}
		}
		auto origin = a_actor.GetPosition();
		origin.z += (a_actor.GetBoundMax().z - a_actor.GetBoundMin().z) * 0.7f;
		return { origin.x, origin.y, origin.z };
	}

	float3 GetAimDirection(RE::Actor& a_actor) noexcept
	{
		float aimAngle = a_actor.GetAimAngle();
		float aimHeading = a_actor.GetAimHeading();
		if (!std::isfinite(aimAngle))
			aimAngle = a_actor.GetAngleX();
		if (!std::isfinite(aimHeading))
			aimHeading = a_actor.GetAngleZ();
		const float horizontalScale = std::cos(aimAngle);
		return {
			horizontalScale * std::sin(aimHeading),
			horizontalScale * std::cos(aimHeading),
			-std::sin(aimAngle)
		};
	}
}
