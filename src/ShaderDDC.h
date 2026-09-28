#pragma once

#include <array>
#include <cstdint>
#include <d3dcompiler.h>
#include <filesystem>
#include <string>
#include <string_view>

namespace ShaderDDC
{
	using Digest = std::array<uint8_t, 32>;

	class Entry
	{
	public:
		Entry(std::filesystem::path a_source, std::filesystem::path a_includeRoot, std::wstring_view a_group,
			const D3D_SHADER_MACRO* a_macros, std::string_view a_entryPoint, std::string_view a_profile,
			uint32_t a_flags, uint32_t a_stripFlags, std::wstring_view a_extension);

		[[nodiscard]] const std::wstring& GetPath() const { return path; }
		[[nodiscard]] ID3DBlob* Load() const;
		bool Store(ID3DBlob* a_blob) const;

	private:
		std::filesystem::path source;
		std::filesystem::path includeRoot;
		Digest sourceDigest{};
		std::wstring path;
	};

	void InvalidateSources();
	void RemoveLegacyCache();
	void Trim();
}
