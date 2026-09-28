#include "ShaderDDC.h"

#include "Utils/Format.h"

#include <bcrypt.h>
#include <winrt/base.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <cwctype>
#include <fstream>
#include <mutex>
#include <optional>
#include <span>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#pragma comment(lib, "bcrypt.lib")

namespace ShaderDDC
{
	namespace
	{
		constexpr uint32_t kFormatVersion = 1;
		constexpr std::wstring_view kCacheRoot = L"Data/ShaderCache";
		constexpr std::wstring_view kRoot = L"Data/ShaderCache/DDC";
		constexpr std::wstring_view kRootName = L"DDC";
		constexpr uintmax_t kMaxBytes = 1ull << 30;
		constexpr LONGLONG kMaxEntryBytes = 64ll << 20;
		constexpr std::chrono::hours kMaxUnusedAge{ 24 * 30 };
		constexpr std::chrono::hours kStaleTempAge{ 1 };

		BCRYPT_ALG_HANDLE GetSha256Algorithm()
		{
			static const BCRYPT_ALG_HANDLE algorithm = [] {
				BCRYPT_ALG_HANDLE handle = nullptr;
				if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&handle, BCRYPT_SHA256_ALGORITHM, nullptr, 0))) {
					logger::error("[DDC] SHA-256 provider unavailable; shader disk cache disabled");
					handle = nullptr;
				}
				return handle;
			}();
			return algorithm;
		}

		class Sha256
		{
		public:
			Sha256()
			{
				if (const auto algorithm = GetSha256Algorithm())
					ok = BCRYPT_SUCCESS(BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0));
			}

			~Sha256()
			{
				if (hash)
					BCryptDestroyHash(hash);
			}

			Sha256(const Sha256&) = delete;
			Sha256& operator=(const Sha256&) = delete;

			void Update(const void* a_data, size_t a_size)
			{
				if (ok && a_size)
					ok = BCRYPT_SUCCESS(BCryptHashData(hash, static_cast<PUCHAR>(const_cast<void*>(a_data)), static_cast<ULONG>(a_size), 0));
			}

			void UpdateText(std::string_view a_text)
			{
				Update(a_text.data(), a_text.size());
				UpdateValue('\0');
			}

			template <class T>
				requires std::is_trivially_copyable_v<T>
			void UpdateValue(const T& a_value)
			{
				Update(&a_value, sizeof(T));
			}

			std::optional<Digest> Finish()
			{
				Digest digest{};
				if (!ok || !BCRYPT_SUCCESS(BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0)))
					return std::nullopt;
				return digest;
			}

		private:
			BCRYPT_HASH_HANDLE hash = nullptr;
			bool ok = false;
		};

		struct SourceFile
		{
			bool exists = false;
			std::optional<Digest> hash;
			std::vector<std::string> includes;
		};

		std::mutex sourceMutex;
		std::unordered_map<std::wstring, SourceFile> sourceFiles;
		std::unordered_map<std::wstring, Digest> sourceClosures;

		std::wstring NormalizePath(const std::filesystem::path& a_path)
		{
			auto result = a_path.lexically_normal().generic_wstring();
			for (auto& c : result)
				c = static_cast<wchar_t>(std::towlower(c));
			return result;
		}

		std::vector<std::string> ScanIncludes(std::string_view a_text)
		{
			std::vector<std::string> includes;
			size_t lineStart = 0;
			while (lineStart < a_text.size()) {
				const size_t lineEnd = std::min(a_text.find('\n', lineStart), a_text.size());
				const auto line = a_text.substr(lineStart, lineEnd - lineStart);
				lineStart = lineEnd + 1;

				auto pos = line.find_first_not_of(" \t");
				if (pos == std::string_view::npos || line[pos] != '#')
					continue;
				pos = line.find_first_not_of(" \t", pos + 1);
				if (pos == std::string_view::npos || line.substr(pos, 7) != "include")
					continue;
				pos = line.find_first_not_of(" \t", pos + 7);
				if (pos == std::string_view::npos || (line[pos] != '"' && line[pos] != '<'))
					continue;
				const auto close = line.find(line[pos] == '"' ? '"' : '>', pos + 1);
				if (close != std::string_view::npos)
					includes.emplace_back(line.substr(pos + 1, close - pos - 1));
			}
			return includes;
		}

		SourceFile LoadSourceFile(const std::filesystem::path& a_path)
		{
			SourceFile file;
			std::ifstream stream(a_path, std::ios::binary);
			if (!stream) {
				file.hash = Digest{};
				return file;
			}
			std::ostringstream buffer;
			buffer << stream.rdbuf();
			const auto text = buffer.str();

			Sha256 sha;
			sha.Update(text.data(), text.size());
			file.exists = true;
			file.hash = sha.Finish();
			file.includes = ScanIncludes(text);
			return file;
		}

		std::optional<Digest> ComputeSourceDigest(const std::filesystem::path& a_source, const std::filesystem::path& a_root, bool a_useMemo)
		{
			Sha256 closure;
			std::unordered_set<std::wstring> visited;
			bool ok = true;

			auto visit = [&](auto&& a_self, const std::filesystem::path& a_path) -> void {
				auto key = NormalizePath(a_path);
				if (!visited.insert(key).second)
					return;

				SourceFile fresh;
				const SourceFile* file = &fresh;
				if (a_useMemo) {
					auto it = sourceFiles.find(key);
					if (it == sourceFiles.end())
						it = sourceFiles.emplace(std::move(key), LoadSourceFile(a_path)).first;
					file = &it->second;
				} else {
					fresh = LoadSourceFile(a_path);
				}

				if (!file->hash) {
					ok = false;
					return;
				}
				closure.UpdateValue(file->exists);
				closure.Update(file->hash->data(), file->hash->size());
				for (const auto& include : file->includes)
					a_self(a_self, a_root / include);
			};

			try {
				visit(visit, a_source);
			} catch (const std::exception& e) {
				logger::warn("[DDC] Failed to hash sources of {}: {}", Util::WStringToString(a_source.wstring()), e.what());
				return std::nullopt;
			}

			auto digest = closure.Finish();
			if (!ok)
				return std::nullopt;
			return digest;
		}

		std::optional<Digest> GetSourceDigest(const std::filesystem::path& a_source, const std::filesystem::path& a_root)
		{
			auto key = NormalizePath(a_source);
			key += L'|';
			key += NormalizePath(a_root);

			std::scoped_lock lock(sourceMutex);
			if (const auto it = sourceClosures.find(key); it != sourceClosures.end())
				return it->second;
			const auto digest = ComputeSourceDigest(a_source, a_root, true);
			if (digest)
				sourceClosures.emplace(std::move(key), *digest);
			return digest;
		}

		std::array<uint32_t, 3> GetCompilerIdentity()
		{
			static const auto identity = [] {
				std::array<uint32_t, 3> result{};
				if (const auto module = GetModuleHandleW(L"d3dcompiler_47.dll")) {
					const auto base = reinterpret_cast<const uint8_t*>(module);
					const auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
					const auto nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
					result = { nt->FileHeader.TimeDateStamp, nt->OptionalHeader.SizeOfImage, nt->OptionalHeader.CheckSum };
				}
				return result;
			}();
			return identity;
		}

		std::wstring ToHex(std::span<const uint8_t> a_bytes)
		{
			static constexpr wchar_t kDigits[] = L"0123456789abcdef";
			std::wstring result;
			result.reserve(a_bytes.size() * 2);
			for (const auto byte : a_bytes) {
				result += kDigits[byte >> 4];
				result += kDigits[byte & 0xF];
			}
			return result;
		}

		bool IsValidDxbc(const void* a_data, size_t a_size)
		{
			if (a_size < 32 || std::memcmp(a_data, "DXBC", 4) != 0)
				return false;
			uint32_t declaredSize = 0;
			std::memcpy(&declaredSize, static_cast<const uint8_t*>(a_data) + 24, sizeof(declaredSize));
			return declaredSize == a_size;
		}
	}

	Entry::Entry(std::filesystem::path a_source, std::filesystem::path a_includeRoot, std::wstring_view a_group,
		const D3D_SHADER_MACRO* a_macros, std::string_view a_entryPoint, std::string_view a_profile,
		uint32_t a_flags, uint32_t a_stripFlags, std::wstring_view a_extension) :
		source(std::move(a_source)),
		includeRoot(std::move(a_includeRoot))
	{
		const auto digest = GetSourceDigest(source, includeRoot);
		if (!digest)
			return;
		sourceDigest = *digest;

		Sha256 key;
		key.UpdateValue(kFormatVersion);
		key.UpdateValue(GetCompilerIdentity());
		key.UpdateText(a_profile);
		key.UpdateText(a_entryPoint);
		key.UpdateValue(a_flags);
		key.UpdateValue(a_stripFlags);
		for (auto macro = a_macros; macro && macro->Name; ++macro) {
			key.UpdateText(macro->Name);
			key.UpdateValue(macro->Definition != nullptr);
			key.UpdateText(macro->Definition ? macro->Definition : "");
		}
		key.Update(sourceDigest.data(), sourceDigest.size());

		const auto keyDigest = key.Finish();
		if (!keyDigest)
			return;
		path = std::format(L"{}/{}/{}{}", kRoot, a_group, ToHex(std::span<const uint8_t>(keyDigest->data(), 16)), a_extension);
	}

	ID3DBlob* Entry::Load() const
	{
		if (path.empty())
			return nullptr;

		constexpr DWORD kShare = FILE_SHARE_READ | FILE_SHARE_DELETE;
		winrt::file_handle file{ CreateFileW(path.c_str(), GENERIC_READ | FILE_WRITE_ATTRIBUTES, kShare, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr) };
		if (!file) {
			const DWORD error = GetLastError();
			if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND)
				return nullptr;
			file.attach(CreateFileW(path.c_str(), GENERIC_READ, kShare, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
			if (!file)
				return nullptr;
		}

		LARGE_INTEGER size{};
		if (!GetFileSizeEx(file.get(), &size) || size.QuadPart > kMaxEntryBytes)
			return nullptr;
		const auto bytes = static_cast<DWORD>(size.QuadPart);

		winrt::com_ptr<ID3DBlob> blob;
		DWORD read = 0;
		if (FAILED(D3DCreateBlob(bytes, blob.put())) || !ReadFile(file.get(), blob->GetBufferPointer(), bytes, &read, nullptr) || read != bytes)
			return nullptr;

		if (!IsValidDxbc(blob->GetBufferPointer(), blob->GetBufferSize())) {
			logger::warn("[DDC] Discarding corrupt entry {}", Util::WStringToString(path));
			file.close();
			DeleteFileW(path.c_str());
			return nullptr;
		}

		FILETIME now{};
		GetSystemTimeAsFileTime(&now);
		SetFileTime(file.get(), nullptr, nullptr, &now);
		return blob.detach();
	}

	bool Entry::Store(ID3DBlob* a_blob) const
	{
		if (path.empty() || !a_blob)
			return false;

		if (ComputeSourceDigest(source, includeRoot, false) != sourceDigest) {
			logger::debug("[DDC] {} changed while compiling; not caching", Util::WStringToString(source.wstring()));
			InvalidateSources();
			return false;
		}

		std::error_code ec;
		std::filesystem::create_directories(std::filesystem::path{ path }.parent_path(), ec);

		const auto temp = std::format(L"{}.{}.tmp", path, GetCurrentThreadId());
		const auto size = static_cast<DWORD>(a_blob->GetBufferSize());
		bool written = false;
		{
			winrt::file_handle file{ CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr) };
			DWORD count = 0;
			written = file && WriteFile(file.get(), a_blob->GetBufferPointer(), size, &count, nullptr) && count == size;
		}
		if (written && MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING))
			return true;

		const DWORD error = GetLastError();
		DeleteFileW(temp.c_str());
		if (written && GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES)
			return true;
		logger::warn("[DDC] Failed to save {} (error {})", Util::WStringToString(path), error);
		return false;
	}

	void InvalidateSources()
	{
		std::scoped_lock lock(sourceMutex);
		sourceFiles.clear();
		sourceClosures.clear();
	}

	void RemoveLegacyCache()
	{
		std::vector<std::filesystem::path> legacy;
		std::error_code ec;
		for (std::filesystem::directory_iterator it(std::filesystem::path{ kCacheRoot }, ec), end; !ec && it != end; it.increment(ec)) {
			if (_wcsicmp(it->path().filename().c_str(), kRootName.data()) != 0)
				legacy.push_back(it->path());
		}
		for (const auto& entry : legacy)
			std::filesystem::remove_all(entry, ec);
		if (!legacy.empty())
			logger::info("[DDC] Removed {} legacy shader cache entries", legacy.size());
	}

	void Trim()
	{
		struct CachedFile
		{
			std::filesystem::path path;
			std::filesystem::file_time_type time;
			uintmax_t size = 0;
		};

		std::vector<CachedFile> entries;
		std::vector<CachedFile> evict;
		uintmax_t totalBytes = 0;
		const auto now = std::filesystem::file_time_type::clock::now();

		std::error_code ec;
		for (std::filesystem::recursive_directory_iterator it(std::filesystem::path{ kRoot }, ec), end; !ec && it != end; it.increment(ec)) {
			std::error_code typeError, timeError, sizeError;
			if (!it->is_regular_file(typeError))
				continue;
			CachedFile file{ it->path(), it->last_write_time(timeError), it->file_size(sizeError) };
			if (timeError || sizeError)
				continue;

			const bool isTemp = file.path.extension() == L".tmp";
			if (now - file.time > (isTemp ? kStaleTempAge : kMaxUnusedAge)) {
				evict.push_back(std::move(file));
			} else if (!isTemp) {
				totalBytes += file.size;
				entries.push_back(std::move(file));
			}
		}

		if (totalBytes > kMaxBytes) {
			std::ranges::sort(entries, {}, &CachedFile::time);
			uintmax_t keptBytes = totalBytes;
			for (const auto& file : entries) {
				if (keptBytes <= kMaxBytes / 10 * 9)
					break;
				keptBytes -= file.size;
				evict.push_back(file);
			}
		}

		size_t evictedCount = 0;
		uintmax_t evictedBytes = 0;
		for (const auto& file : evict) {
			std::error_code removeError;
			if (std::filesystem::remove(file.path, removeError)) {
				++evictedCount;
				evictedBytes += file.size;
			}
		}

		constexpr double kMiB = 1024.0 * 1024.0;
		logger::info("[DDC] {} entries ({:.1f} MiB), evicted {} ({:.1f} MiB)",
			entries.size(), static_cast<double>(totalBytes) / kMiB, evictedCount, static_cast<double>(evictedBytes) / kMiB);
	}
}
