#pragma once

struct CullPoolExhaustionFix : EngineFix
{
	std::string GetName() override { return "Cull Pool Exhaustion Fix"; }
	const char* GetEngineFixesName() const override { return "CullingProcessAppendVirtualPoolGuard"; }

	void Install() override;

	template <class Vtable>
	struct AppendVirtualGuard
	{
		static void thunk(RE::BSCullingProcess* a_this, RE::BSGeometry& a_visible, std::int32_t a_alphaGroupIndex);
		static inline REL::Relocation<decltype(thunk)> func;
	};
};
