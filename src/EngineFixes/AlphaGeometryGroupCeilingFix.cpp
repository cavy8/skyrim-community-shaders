#include "AlphaGeometryGroupCeilingFix.h"

#include "Features/Skylighting.h"

namespace
{
	constexpr std::uint32_t CAPACITY_FLAT = 512;
	constexpr std::uint32_t CAPACITY_VR = 1024;
	constexpr std::uint32_t RESERVE = 64;

	std::uint32_t* groupCount = nullptr;
	std::uint32_t groupLimit = 0;
	std::atomic<std::uint64_t> refused{ 0 };

	std::uint32_t* DecodeCounter()
	{
		constexpr std::uint8_t MOV_DWORD_IMM = 0xC7;
		constexpr std::uint8_t RIP_RELATIVE_MODRM = 0x05;
		constexpr std::uint8_t RET = 0xC3;
		constexpr std::size_t MOV_DWORD_IMM_SIZE = 10;

		const auto clearFunction = REL::RelocationID(100856, 107646).address();
		if (!clearFunction)
			return nullptr;

		const auto* code = reinterpret_cast<const std::uint8_t*>(clearFunction);
		std::int32_t displacement = 0;
		std::uint32_t immediate = 1;
		std::memcpy(&displacement, code + 2, sizeof(displacement));
		std::memcpy(&immediate, code + 6, sizeof(immediate));
		if (code[0] != MOV_DWORD_IMM || code[1] != RIP_RELATIVE_MODRM || immediate != 0u || code[MOV_DWORD_IMM_SIZE] != RET)
			return nullptr;

		const auto counter = clearFunction + MOV_DWORD_IMM_SIZE + displacement;
		const auto data = REL::Module::get().segment(REL::Segment::data);
		if (counter < data.address() || counter >= data.address() + data.size())
			return nullptr;
		return reinterpret_cast<std::uint32_t*>(counter);
	}
}

void AlphaGeometryGroupCeilingFix::Install()
{
	groupCount = DecodeCounter();
	if (!groupCount) {
		logger::error("[Engine Fixes] Alpha GeometryGroup ceiling not installed: ClearAlphaGeometryGroups did not decode to a counter in .data");
		return;
	}
	groupLimit = (REL::Module::IsVR() ? CAPACITY_VR : CAPACITY_FLAT) - RESERVE;
	stl::detour_thunk<BSBatchRenderer_StartGroupingAlphas>(REL::RelocationID(100874, 107670));
}

void* AlphaGeometryGroupCeilingFix::BSBatchRenderer_StartGroupingAlphas::thunk(RE::BSBatchRenderer* a_this, void* a_bound, RE::NiCamera* a_camera, bool a_sortByClosestPoint)
{
	const std::uint32_t limit = std::max(groupLimit, Skylighting::BSShaderAccumulator_StartGroupingAlphas::poolCapacity - RESERVE);
	if (groupCount && a_camera && *groupCount >= limit) {
		const std::uint64_t count = refused.fetch_add(1, std::memory_order_relaxed) + 1;
		if (count == 1u || (count % 10000u) == 0u)
			logger::warn("[Engine Fixes] Alpha GeometryGroup ceiling reached ({} live, {} refused)", *groupCount, count);
		return nullptr;
	}
	return func(a_this, a_bound, a_camera, a_sortByClosestPoint);
}
