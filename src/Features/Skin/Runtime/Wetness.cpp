#include "Features/Skin.h"
#include "Globals.h"
#include "State.h"
#include <cmath>

// By PO3
// https://github.com/powerof3/Splashes-of-Skyrim/blob/master/src/Manager.cpp
float Skin::GetWaterHeight(const RE::TESObjectREFR* a_ref, const RE::NiPoint3& a_pos)
{
	float waterHeight = -RE::NI_INFINITY;

	if (const auto waterManager = RE::TESWaterSystem::GetSingleton()) {
		waterHeight = a_ref->GetWaterHeight();

		if (waterHeight != -RE::NI_INFINITY) {
			return waterHeight;
		}

		const auto get_nearest_water_object_height = [&]() {
			for (const auto& waterObject : waterManager->waterObjects) {
				if (waterObject) {
					for (const auto& bound : waterObject->multiBounds) {
						if (bound) {
							if (auto size{ bound->size }; size.z <= 10.0f) {  //avoid sloped water
								auto center{ bound->center };
								const auto boundMin = center - size;
								const auto boundMax = center + size;
								if (!(a_pos.x < boundMin.x || a_pos.x > boundMax.x || a_pos.y < boundMin.y || a_pos.y > boundMax.y)) {
									return center.z;
								}
							}
						}
					}
				}
			}

			return -RE::NI_INFINITY;
		};

		waterHeight = get_nearest_water_object_height();
	}

	return waterHeight;
}

float4 Skin::GetWetness(RE::BSGeometry* a_geometry)
{
	float4 wetness = float4(0.0f, 0.0f, 0.0f, 0.0f);
	auto userData = a_geometry->GetUserData();
	if (userData && userData->IsActor()) {
		auto actor = static_cast<RE::Character*>(userData);
		const uint32_t actorFormID = userData->formID;
		const uint currentFrame = globals::state->frameCount;

		// Drop actors not seen since the last frame; clearing everything would reset the wetness fade of actors still in view.
		if (actorWetnessMap.size() > 1024) {
			std::erase_if(actorWetnessMap, [currentFrame](const auto& entry) {
				return currentFrame - entry.second.frameCount > 1;
			});
		}

		auto [it, inserted] = actorWetnessMap.try_emplace(actorFormID);
		auto& cached = it->second;
		if (!inserted && cached.frameCount == currentFrame) {
			return cached.output;
		}
		cached.frameCount = currentFrame;

		const float positionZ = actor->GetPositionZ();
		wetness.z = positionZ;
		const float stamina = actor->AsActorValueOwner()->GetActorValue(RE::ActorValue::kStamina);
		const float permanentStamina = actor->AsActorValueOwner()->GetPermanentActorValue(RE::ActorValue::kStamina);
		const float temporaryStamina = actor->GetActorValueModifier(RE::ACTOR_VALUE_MODIFIER::kTemporary, RE::ActorValue::kStamina);
		const float maxStamina = std::max(permanentStamina + temporaryStamina, 1.0f);
		const float staminaPercentage = actor->IsDead() ? 1.0f : (stamina / maxStamina);
		// The two sliders are independent, so order them before building the ramp.
		const float sweatStart = std::max(settings.StartSweat, settings.FullSweat);
		const float sweatFull = std::min(settings.StartSweat, settings.FullSweat);
		const float sweatRange = sweatStart - sweatFull;
		wetness.x = (sweatRange < 1e-5f)              ? 0.0f :
		            (staminaPercentage >= sweatStart) ? 0.0f :
		            (staminaPercentage <= sweatFull)  ? 1.0f :
		                                                (sweatStart - staminaPercentage) / sweatRange;
		if (actor->IsInWater()) {
			wetness.y = 2.0f;
			const float waterHeight = GetWaterHeight(userData, actor->GetPosition());
			wetness.w = std::max(0.0f, waterHeight - positionZ);
		} else {
			wetness.y = 0.0f;
			wetness.w = 0.0f;
		}

		ExternalWetness external;
		{
			std::scoped_lock lock(externalWetnessLock);
			if (auto ext = externalWetnessMap.find(actorFormID); ext != externalWetnessMap.end())
				external = ext->second;
		}
		const float externalWet = external.wet * 2.0f;

		if (inserted) {
			cached.wetness = wetness;
			cached.externalSweat = external.sweat;
			cached.externalWet = externalWet;
		} else {
			const float fadeTime = std::max(settings.WetFadeTime, 0.001f);
			if (cached.wetness.x < wetness.x) {
				cached.wetness.x = wetness.x;
			} else if (cached.wetness.x > wetness.x) {
				cached.wetness.x -= *globals::game::deltaTime / fadeTime;
				cached.wetness.x = std::max(cached.wetness.x, wetness.x);
			}
			wetness.x = cached.wetness.x;

			if (cached.wetness.y < wetness.y) {
				cached.wetness.y = wetness.y;
				if (cached.wetness.w < wetness.w) {
					cached.wetness.w = wetness.w;
				} else {
					wetness.w = cached.wetness.w;
				}
			} else if (cached.wetness.y > wetness.y) {
				cached.wetness.y -= *globals::game::deltaTime / fadeTime;
				cached.wetness.y = std::max(cached.wetness.y, wetness.y);
				wetness.y = cached.wetness.y;
				if (wetness.y == 0.0f) {
					wetness.w = 0.0f;
					cached.wetness.w = 0.0f;
				} else if (cached.wetness.w < wetness.w) {
					cached.wetness.w = wetness.w;
				} else {
					wetness.w = cached.wetness.w;
				}
			} else if (cached.wetness.w < wetness.w) {
				cached.wetness.w = wetness.w;
			} else {
				wetness.w = cached.wetness.w;
			}
			cached.wetness = wetness;

			const float fadeStep = std::max(*globals::game::deltaTime, 0.0f) / fadeTime;
			cached.externalSweat += std::clamp(external.sweat - cached.externalSweat, -fadeStep, fadeStep);
			cached.externalWet += std::clamp(externalWet - cached.externalWet, -fadeStep, fadeStep);
		}

		const float waterLine = wetness.y > 0.0f ? wetness.z + wetness.w : 0.0f;
		cached.output = float4(std::max(wetness.x, cached.externalSweat), wetness.y, waterLine, cached.externalWet);
		return cached.output;
	}
	return wetness;
}

void Skin::SetExternalWetness(uint32_t a_formID, float a_sweat, float a_wet)
{
	const float sweat = std::isnan(a_sweat) ? 0.0f : std::clamp(a_sweat, 0.0f, 1.0f);
	const float wet = std::isnan(a_wet) ? 0.0f : std::clamp(a_wet, 0.0f, 1.0f);

	std::scoped_lock lock(externalWetnessLock);
	if (sweat == 0.0f && wet == 0.0f) {
		externalWetnessMap.erase(a_formID);
		return;
	}
	if (externalWetnessMap.size() > 1024 && !externalWetnessMap.contains(a_formID))
		externalWetnessMap.clear();
	externalWetnessMap[a_formID] = { sweat, wet };
}

void Skin::ClearExternalWetness(uint32_t a_formID)
{
	std::scoped_lock lock(externalWetnessLock);
	externalWetnessMap.erase(a_formID);
}

extern "C" __declspec(dllexport) uint32_t BottledShaders_WetnessAPIVersion()
{
	return 1;
}

extern "C" __declspec(dllexport) void BottledShaders_SetActorWetness(uint32_t a_formID, float a_sweat, float a_wet)
{
	globals::features::skin.SetExternalWetness(a_formID, a_sweat, a_wet);
}

extern "C" __declspec(dllexport) void BottledShaders_ClearActorWetness(uint32_t a_formID)
{
	globals::features::skin.ClearExternalWetness(a_formID);
}
