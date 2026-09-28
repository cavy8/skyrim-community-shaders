#pragma once

#include "Features/LightLimitFix.h"

namespace LocalShadowDiagnostics
{
	enum class Loss : uint8_t
	{
		None,
		Evicted,
		Preempted,
		NewNiLight,
		Teleported,
		CacheReleased,
	};

	bool IsActive();
	void NoteSchedule();
	void NoteLoss(RE::BSShadowLight* a_light, Loss a_reason, RE::BSShadowLight* a_by = nullptr, float a_distance = 0.0f);
	void NoteCacheReleased(const LightLimitFix& a_llf);
	void NoteNoSlice(RE::BSShadowLight* a_light);
	void NoteLodDimmerRestored(RE::BSShadowLight* a_light);
	void NoteRenderedCaster(const LightLimitFix& a_llf, RE::BSShadowLight* a_light);
	void NoteSliceCollision(RE::BSShadowLight* a_light, uint32_t a_engineSlice, uint32_t a_claims);
	void NoteUploadedLight(RE::BSLight* a_light, const LightLimitFix::LightData& a_data, bool a_lit, bool a_withheld);
	void EndFrame(LightLimitFix& a_llf);
	void RequestReport();
	bool HandleHotkey(uint32_t a_vkKey);
	bool IsReportNoticeVisible();
}
