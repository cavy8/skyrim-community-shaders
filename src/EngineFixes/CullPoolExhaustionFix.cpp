#include "CullPoolExhaustionFix.h"

namespace
{
	constexpr std::uint32_t FREE_ENTRY_MARGIN = 64;

	struct BaseVtable;
	struct ParabolicVtable;

	std::atomic<bool> loggedDrop{ false };

	bool NearExhaustion(const RE::BSCullingProcess* a_this)
	{
		const auto& pool = a_this->cullQueue.free;
		return pool.end - pool.start < FREE_ENTRY_MARGIN;
	}
}

void CullPoolExhaustionFix::Install()
{
	stl::write_vfunc<0x18, AppendVirtualGuard<BaseVtable>>(RE::VTABLE_BSCullingProcess[0]);
	stl::write_vfunc<0x18, AppendVirtualGuard<ParabolicVtable>>(RE::VTABLE_BSParabolicCullingProcess[0]);
}

template <class Vtable>
void CullPoolExhaustionFix::AppendVirtualGuard<Vtable>::thunk(RE::BSCullingProcess* a_this, RE::BSGeometry& a_visible, std::int32_t a_alphaGroupIndex)
{
	if (NearExhaustion(a_this)) {
		if (!loggedDrop.exchange(true, std::memory_order_relaxed))
			logger::warn("[Engine Fixes] Culling pool near exhaustion; dropping appends");
		return;
	}
	func(a_this, a_visible, a_alphaGroupIndex);
}
