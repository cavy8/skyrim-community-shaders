#pragma once

struct AlphaGeometryGroupCeilingFix : EngineFix
{
	std::string GetName() override { return "Alpha GeometryGroup Ceiling Fix"; }
	const char* GetEngineFixesName() const override { return "BatchRendererAlphaGeometryGroupOverflow"; }

	void Install() override;

	struct BSBatchRenderer_StartGroupingAlphas
	{
		static void* thunk(RE::BSBatchRenderer* a_this, void* a_bound, RE::NiCamera* a_camera, bool a_sortByClosestPoint);
		static inline REL::Relocation<decltype(thunk)> func;
	};
};
