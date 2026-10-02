#include "StrandStyle.h"

#include <algorithm>
#include <fstream>

#include "Utils/FileSystem.h"

namespace Strands
{
	namespace
	{
		constexpr std::array<std::string_view, static_cast<size_t>(HairPreset::Count)> kPresetNames{ "auto", "straight", "wavy", "curly", "coily", "locs" };
		constexpr std::array<std::string_view, static_cast<size_t>(SeedMode::Count)> kSeedNames{ "auto", "roots", "area" };
		constexpr std::array<std::string_view, static_cast<size_t>(FlowAxis::Count)> kFlowNames{ "auto", "v", "-v", "u", "-u" };

		constexpr std::string_view kUserFileName = "UserStyles.json";

		std::string ToLower(std::string_view a_text)
		{
			std::string result{ a_text };
			std::ranges::transform(result, result.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			return result;
		}

		// Model paths compare lowercase, backslash-separated and relative to Meshes.
		std::string NormalizeModel(std::string_view a_model)
		{
			std::string result = ToLower(a_model);
			std::ranges::replace(result, '/', '\\');
			for (std::string_view prefix : { "data\\meshes\\"sv, "meshes\\"sv }) {
				if (result.starts_with(prefix)) {
					result.erase(0, prefix.size());
					break;
				}
			}
			return result;
		}

		template <class E, size_t N>
		E EnumFromName(const json& a_json, const char* a_key, const std::array<std::string_view, N>& a_names, E a_default)
		{
			auto it = a_json.find(a_key);
			if (it == a_json.end() || !it->is_string())
				return a_default;
			const auto name = ToLower(it->get<std::string>());
			for (size_t i = 0; i < N; ++i) {
				if (a_names[i] == name)
					return static_cast<E>(i);
			}
			logger::warn("[HairStrands] Unknown {} '{}' in a style entry; using the default", a_key, name);
			return a_default;
		}

		void ReadFloat(const json& a_json, const char* a_key, float& io_value)
		{
			auto it = a_json.find(a_key);
			if (it != a_json.end() && it->is_number())
				io_value = it->get<float>();
		}

		// A count; Sanitize clamps it to its range afterwards.
		void ReadCount(const json& a_json, const char* a_key, uint32_t& io_value)
		{
			auto it = a_json.find(a_key);
			if (it != a_json.end() && it->is_number())
				io_value = static_cast<uint32_t>(std::clamp(it->get<double>(), 0.0, 1024.0));
		}

		void HashBytes(uint64_t& io_hash, const void* a_data, size_t a_size)
		{
			const auto* bytes = static_cast<const uint8_t*>(a_data);
			for (size_t i = 0; i < a_size; ++i) {
				io_hash ^= bytes[i];
				io_hash *= 0x100000001B3ull;
			}
		}

		template <class T>
		void Hash(uint64_t& io_hash, const T& a_value)
		{
			HashBytes(io_hash, &a_value, sizeof(T));
		}
	}

	bool StrandStyle::GenerationDiffers(const StrandStyle& a_other) const
	{
		return GenerationHash() != a_other.GenerationHash();
	}

	uint64_t StrandStyle::GenerationHash() const
	{
		uint64_t hash = 0xCBF29CE484222325ull;
		Hash(hash, seeding);
		Hash(hash, flowAxis);
		Hash(hash, density);
		Hash(hash, segmentLength);
		Hash(hash, lengthScale);
		Hash(hash, volume);
		Hash(hash, layerJitter);
		Hash(hash, clumpStrength);
		Hash(hash, clumpSize);
		Hash(hash, clumpTwist);
		Hash(hash, shortLength);
		Hash(hash, coverageThreshold);
		Hash(hash, seed);
		for (const auto& rect : excludeUV)
			Hash(hash, rect);
		Hash(hash, keepWoven);
		for (const auto& rect : chainUV)
			Hash(hash, rect);
		if (!asset.empty()) {
			// A file exported again is a new asset.
			HashBytes(hash, asset.data(), asset.size());
			std::error_code ec;
			const auto written = std::filesystem::last_write_time(Util::PathHelpers::GetDataPath() / asset, ec);
			if (!ec)
				Hash(hash, written.time_since_epoch().count());
		}
		return hash;
	}

	void Sanitize(StrandStyle& a_style)
	{
		a_style.preset = static_cast<HairPreset>(std::min(static_cast<uint32_t>(a_style.preset), static_cast<uint32_t>(HairPreset::Count) - 1));
		a_style.seeding = static_cast<SeedMode>(std::min(static_cast<uint32_t>(a_style.seeding), static_cast<uint32_t>(SeedMode::Count) - 1));
		a_style.flowAxis = static_cast<FlowAxis>(std::min(static_cast<uint32_t>(a_style.flowAxis), static_cast<uint32_t>(FlowAxis::Count) - 1));

		a_style.density = std::clamp(a_style.density, StyleLimits::kMinDensity, StyleLimits::kMaxDensity);
		a_style.segmentLength = std::clamp(a_style.segmentLength, StyleLimits::kMinSegmentLength, StyleLimits::kMaxSegmentLength);
		a_style.lengthScale = std::clamp(a_style.lengthScale, 0.05f, 1.0f);
		a_style.volume = std::clamp(a_style.volume, 0.0f, StyleLimits::kMaxVolume);
		a_style.layerJitter = std::clamp(a_style.layerJitter, 0.0f, StyleLimits::kMaxVolume);
		a_style.clumpStrength = std::clamp(a_style.clumpStrength, 0.0f, 1.0f);
		a_style.clumpSize = std::clamp(a_style.clumpSize, 0.1f, StyleLimits::kMaxClumpSize);
		a_style.clumpTwist = std::clamp(a_style.clumpTwist, -StyleLimits::kMaxTwist, StyleLimits::kMaxTwist);
		a_style.shortLength = std::clamp(a_style.shortLength, 0.1f, StyleLimits::kMaxShortLength);
		a_style.coverageThreshold = std::clamp(a_style.coverageThreshold, 0.0f, 1.0f);

		a_style.rootWidth = std::clamp(a_style.rootWidth, StyleLimits::kMinWidth, StyleLimits::kMaxWidth);
		a_style.tipWidth = std::clamp(a_style.tipWidth, StyleLimits::kMinWidth, StyleLimits::kMaxWidth);
		a_style.waveAmplitude = std::clamp(a_style.waveAmplitude, 0.0f, StyleLimits::kMaxWaveAmplitude);
		a_style.waveLength = std::clamp(a_style.waveLength, StyleLimits::kMinPeriod, StyleLimits::kMaxPeriod);
		a_style.curlRadius = std::clamp(a_style.curlRadius, 0.0f, StyleLimits::kMaxCurlRadius);
		a_style.curlLength = std::clamp(a_style.curlLength, StyleLimits::kMinPeriod, StyleLimits::kMaxPeriod);
		a_style.curlStart = std::clamp(a_style.curlStart, 0.0f, 1.0f);
		a_style.frizz = std::clamp(a_style.frizz, 0.0f, 1.0f);
		a_style.flyaways = std::clamp(a_style.flyaways, 0.0f, 0.5f);

		a_style.vspCoeff = std::clamp(a_style.vspCoeff, 0.0f, 1.0f);
		a_style.vspAccelThreshold = std::clamp(a_style.vspAccelThreshold, 0.0f, StyleLimits::kMaxVspAccelThreshold);
		a_style.localConstraintStiffness = std::clamp(a_style.localConstraintStiffness, 0.0f, 1.0f);
		a_style.localConstraintsIterations = std::clamp(a_style.localConstraintsIterations, 0u, StyleLimits::kMaxLocalIterations);
		a_style.globalConstraintStiffness = std::clamp(a_style.globalConstraintStiffness, 0.0f, 1.0f);
		a_style.globalConstraintsRange = std::clamp(a_style.globalConstraintsRange, 0.0f, 1.0f);
		a_style.lengthConstraintsIterations = std::clamp(a_style.lengthConstraintsIterations, 1u, StyleLimits::kMaxLengthIterations);
		a_style.damping = std::clamp(a_style.damping, 0.0f, 1.0f);
		a_style.gravityMagnitude = std::clamp(a_style.gravityMagnitude, 0.0f, StyleLimits::kMaxGravityMagnitude);
		a_style.tipSeparation = std::clamp(a_style.tipSeparation, 0.0f, StyleLimits::kMaxTipSeparation);
		a_style.clampPositionDelta = std::clamp(a_style.clampPositionDelta, StyleLimits::kMinClampPositionDelta, StyleLimits::kMaxClampPositionDelta);
		a_style.windResponse = std::clamp(a_style.windResponse, 0.0f, StyleLimits::kMaxWindResponse);

		a_style.chainStiffness = std::clamp(a_style.chainStiffness, 0.0f, 1.0f);
		a_style.chainDamping = std::clamp(a_style.chainDamping, 0.0f, 1.0f);
		a_style.chainGravity = std::clamp(a_style.chainGravity, 0.0f, StyleLimits::kMaxChainGravity);

		// Malformed or absurd rectangles would only waste time; keep bounded, ordered lists.
		for (auto* rects : { &a_style.excludeUV, &a_style.chainUV }) {
			if (rects->size() > StyleLimits::kMaxExcludeRects)
				rects->resize(StyleLimits::kMaxExcludeRects);
			for (auto& rect : *rects) {
				if (rect.minU > rect.maxU)
					std::swap(rect.minU, rect.maxU);
				if (rect.minV > rect.maxV)
					std::swap(rect.minV, rect.maxV);
			}
		}
	}

	StrandStyle MakePresetStyle(HairPreset a_preset)
	{
		StrandStyle style{};
		style.preset = a_preset;
		switch (a_preset) {
		case HairPreset::Wavy:
			style.waveAmplitude = 0.35f;
			style.waveLength = 4.0f;
			style.clumpStrength = 0.35f;
			style.volume = 0.25f;
			style.frizz = 0.04f;
			style.flyaways = 0.03f;
			// Waves keep their shape a little more firmly than straight hair.
			style.localConstraintStiffness = 0.93f;
			style.damping = 0.075f;
			break;
		case HairPreset::Curly:
			style.density = 14.0f;
			style.segmentLength = 0.8f;
			style.curlRadius = 0.35f;
			style.curlLength = 1.6f;
			style.curlStart = 0.15f;
			style.clumpStrength = 0.55f;
			style.clumpSize = 0.8f;
			style.volume = 0.4f;
			style.frizz = 0.08f;
			style.flyaways = 0.04f;
			// Curls are springy: they keep their shape and bounce rather than swing.
			style.vspCoeff = 0.5f;
			style.localConstraintStiffness = 0.95f;
			style.localConstraintsIterations = 4;
			style.globalConstraintStiffness = 0.45f;
			style.globalConstraintsRange = 0.5f;
			style.damping = 0.08f;
			style.gravityMagnitude = 75.0f;
			break;
		case HairPreset::Coily:
			// Afro-textured hair: tight coils that start at the root, almost no clumping
			// (coils interlock into a cloud rather than ringlets), lots of volume and frizz,
			// and denser, slightly thicker strands so the scalp does not show through.
			style.density = 20.0f;
			style.segmentLength = 0.75f;
			style.curlRadius = 0.18f;
			style.curlLength = 0.45f;
			style.curlStart = 0.02f;
			style.clumpStrength = 0.1f;
			style.clumpSize = 0.6f;
			style.volume = 0.8f;
			style.layerJitter = 0.3f;
			style.frizz = 0.3f;
			style.flyaways = 0.08f;
			style.rootWidth = 0.07f;
			style.tipWidth = 0.03f;
			// A coily cloud holds its shape and barely sways.
			style.vspCoeff = 0.7f;
			style.localConstraintStiffness = 0.95f;
			style.localConstraintsIterations = 4;
			style.globalConstraintStiffness = 0.6f;
			style.globalConstraintsRange = 0.8f;
			style.damping = 0.15f;
			style.gravityMagnitude = 50.0f;
			style.windResponse = 0.4f;
			break;
		case HairPreset::Locs:
			// Strands collapse onto their clump centre and twist around it: ropes.
			style.density = 16.0f;
			style.clumpStrength = 0.95f;
			style.clumpSize = 0.9f;
			style.clumpTwist = 0.35f;
			style.volume = 0.1f;
			style.frizz = 0.12f;
			style.flyaways = 0.05f;
			style.rootWidth = 0.07f;
			style.tipWidth = 0.05f;
			// Heavy ropes: they hang lower and swing wide.
			style.vspCoeff = 0.3f;
			style.localConstraintStiffness = 0.85f;
			style.globalConstraintsRange = 0.3f;
			style.lengthConstraintsIterations = 12;
			style.gravityMagnitude = 150.0f;
			style.windResponse = 0.6f;
			break;
		case HairPreset::Auto:
		case HairPreset::Straight:
		default:
			break;
		}
		return style;
	}

	HairPreset GuessPreset(std::string_view a_headPart, std::string_view a_model, std::string_view a_shape)
	{
		const std::string names = ToLower(a_headPart) + '|' + ToLower(a_model) + '|' + ToLower(a_shape);
		const auto has = [&](std::initializer_list<std::string_view> a_words) {
			return std::ranges::any_of(a_words, [&](std::string_view w) { return names.find(w) != std::string::npos; });
		};
		if (has({ "afro", "coil", "kink", "4c" }))
			return HairPreset::Coily;
		if (has({ "locs", "dread", "braid", "twist", "cornrow" }))
			return HairPreset::Locs;
		if (has({ "curl", "ringlet" }))
			return HairPreset::Curly;
		if (has({ "wavy", "wave" }))
			return HairPreset::Wavy;
		return HairPreset::Straight;
	}

	std::string_view PresetName(HairPreset a_preset)
	{
		return kPresetNames[std::min(static_cast<size_t>(a_preset), kPresetNames.size() - 1)];
	}

	void StyleToJson(const StrandStyle& a_style, json& o_json)
	{
		o_json["enabled"] = a_style.enabled;
		o_json["preset"] = PresetName(a_style.preset);
		o_json["seeding"] = kSeedNames[static_cast<size_t>(a_style.seeding)];
		o_json["flowAxis"] = kFlowNames[static_cast<size_t>(a_style.flowAxis)];
		o_json["density"] = a_style.density;
		o_json["segmentLength"] = a_style.segmentLength;
		o_json["lengthScale"] = a_style.lengthScale;
		o_json["volume"] = a_style.volume;
		o_json["layerJitter"] = a_style.layerJitter;
		o_json["clumpStrength"] = a_style.clumpStrength;
		o_json["clumpSize"] = a_style.clumpSize;
		o_json["clumpTwist"] = a_style.clumpTwist;
		o_json["shortLength"] = a_style.shortLength;
		o_json["coverageThreshold"] = a_style.coverageThreshold;
		o_json["seed"] = a_style.seed;
		o_json["rootWidth"] = a_style.rootWidth;
		o_json["tipWidth"] = a_style.tipWidth;
		o_json["waveAmplitude"] = a_style.waveAmplitude;
		o_json["waveLength"] = a_style.waveLength;
		o_json["curlRadius"] = a_style.curlRadius;
		o_json["curlLength"] = a_style.curlLength;
		o_json["curlStart"] = a_style.curlStart;
		o_json["frizz"] = a_style.frizz;
		o_json["flyaways"] = a_style.flyaways;
		o_json["simulate"] = a_style.simulate;
		o_json["vspCoeff"] = a_style.vspCoeff;
		o_json["vspAccelThreshold"] = a_style.vspAccelThreshold;
		o_json["localConstraintStiffness"] = a_style.localConstraintStiffness;
		o_json["localConstraintsIterations"] = a_style.localConstraintsIterations;
		o_json["globalConstraintStiffness"] = a_style.globalConstraintStiffness;
		o_json["globalConstraintsRange"] = a_style.globalConstraintsRange;
		o_json["lengthConstraintsIterations"] = a_style.lengthConstraintsIterations;
		// Not "damping": the solver before TressFX's saved its own damping (of velocity relative to the
		// head, 0.4 by default) under that name, which would load here as heavy air drag.
		o_json["dampingCoeff"] = a_style.damping;
		o_json["gravityMagnitude"] = a_style.gravityMagnitude;
		o_json["tipSeparation"] = a_style.tipSeparation;
		o_json["clampPositionDelta"] = a_style.clampPositionDelta;
		o_json["windResponse"] = a_style.windResponse;
		o_json["chainStiffness"] = a_style.chainStiffness;
		o_json["chainDamping"] = a_style.chainDamping;
		o_json["chainGravity"] = a_style.chainGravity;
		o_json["keepWoven"] = a_style.keepWoven;
		const auto rects = [](const std::vector<UVRect>& a_rects) {
			json array = json::array();
			for (const auto& rect : a_rects)
				array.push_back({ rect.minU, rect.minV, rect.maxU, rect.maxV });
			return array;
		};
		o_json["excludeUV"] = rects(a_style.excludeUV);
		o_json["chainUV"] = rects(a_style.chainUV);
		if (!a_style.asset.empty())
			o_json["asset"] = a_style.asset;
	}

	StrandStyle StyleFromJson(const json& a_json, HairPreset a_autoPreset)
	{
		if (!a_json.is_object())
			return MakePresetStyle(a_autoPreset);

		const auto preset = EnumFromName(a_json, "preset", kPresetNames, HairPreset::Auto);
		StrandStyle style = MakePresetStyle(preset == HairPreset::Auto ? a_autoPreset : preset);
		// Keep Auto visible to the editor: the preset it resolved to is only a default.
		style.preset = preset;

		if (auto it = a_json.find("enabled"); it != a_json.end() && it->is_boolean())
			style.enabled = it->get<bool>();
		style.seeding = EnumFromName(a_json, "seeding", kSeedNames, style.seeding);
		style.flowAxis = EnumFromName(a_json, "flowAxis", kFlowNames, style.flowAxis);

		ReadFloat(a_json, "density", style.density);
		ReadFloat(a_json, "segmentLength", style.segmentLength);
		ReadFloat(a_json, "lengthScale", style.lengthScale);
		ReadFloat(a_json, "volume", style.volume);
		ReadFloat(a_json, "layerJitter", style.layerJitter);
		ReadFloat(a_json, "clumpStrength", style.clumpStrength);
		ReadFloat(a_json, "clumpSize", style.clumpSize);
		ReadFloat(a_json, "clumpTwist", style.clumpTwist);
		ReadFloat(a_json, "shortLength", style.shortLength);
		ReadFloat(a_json, "coverageThreshold", style.coverageThreshold);
		if (auto it = a_json.find("seed"); it != a_json.end() && it->is_number_integer())
			style.seed = static_cast<uint32_t>(it->get<int64_t>());

		ReadFloat(a_json, "rootWidth", style.rootWidth);
		ReadFloat(a_json, "tipWidth", style.tipWidth);
		ReadFloat(a_json, "waveAmplitude", style.waveAmplitude);
		ReadFloat(a_json, "waveLength", style.waveLength);
		ReadFloat(a_json, "curlRadius", style.curlRadius);
		ReadFloat(a_json, "curlLength", style.curlLength);
		ReadFloat(a_json, "curlStart", style.curlStart);
		ReadFloat(a_json, "frizz", style.frizz);
		ReadFloat(a_json, "flyaways", style.flyaways);

		if (auto it = a_json.find("simulate"); it != a_json.end() && it->is_boolean())
			style.simulate = it->get<bool>();
		ReadFloat(a_json, "vspCoeff", style.vspCoeff);
		ReadFloat(a_json, "vspAccelThreshold", style.vspAccelThreshold);
		ReadFloat(a_json, "localConstraintStiffness", style.localConstraintStiffness);
		ReadCount(a_json, "localConstraintsIterations", style.localConstraintsIterations);
		ReadFloat(a_json, "globalConstraintStiffness", style.globalConstraintStiffness);
		ReadFloat(a_json, "globalConstraintsRange", style.globalConstraintsRange);
		ReadCount(a_json, "lengthConstraintsIterations", style.lengthConstraintsIterations);
		ReadFloat(a_json, "dampingCoeff", style.damping);
		ReadFloat(a_json, "gravityMagnitude", style.gravityMagnitude);
		ReadFloat(a_json, "tipSeparation", style.tipSeparation);
		ReadFloat(a_json, "clampPositionDelta", style.clampPositionDelta);
		ReadFloat(a_json, "windResponse", style.windResponse);
		ReadFloat(a_json, "chainStiffness", style.chainStiffness);
		ReadFloat(a_json, "chainDamping", style.chainDamping);
		ReadFloat(a_json, "chainGravity", style.chainGravity);

		if (auto it = a_json.find("keepWoven"); it != a_json.end() && it->is_boolean())
			style.keepWoven = it->get<bool>();
		const auto readRects = [&](const char* a_key, std::vector<UVRect>& o_rects) {
			auto it = a_json.find(a_key);
			if (it == a_json.end() || !it->is_array())
				return;
			for (const auto& rect : *it) {
				if (!rect.is_array() || rect.size() != 4 || !std::ranges::all_of(rect, [](const json& v) { return v.is_number(); })) {
					logger::warn("[HairStrands] Skipping a {} rectangle that is not [minU, minV, maxU, maxV]", a_key);
					continue;
				}
				o_rects.push_back({ rect[0].get<float>(), rect[1].get<float>(), rect[2].get<float>(), rect[3].get<float>() });
			}
		};
		readRects("excludeUV", style.excludeUV);
		readRects("chainUV", style.chainUV);
		if (auto it = a_json.find("asset"); it != a_json.end() && it->is_string()) {
			const std::filesystem::path path(it->get<std::string>());
			// Inside Data only: no drive, no root, no way up.
			if (path.empty() || path.has_root_name() || path.has_root_directory() || std::ranges::any_of(path, [](const std::filesystem::path& a_part) { return a_part == ".."; }))
				logger::warn("[HairStrands] Ignoring asset \"{}\": it must be a path relative to Data", it->get<std::string>());
			else
				style.asset = it->get<std::string>();
		}

		Sanitize(style);
		return style;
	}

	std::string HairKey::ToString() const
	{
		return std::format("{} | {} | {} ({} verts, {} tris)", headPart.empty() ? "-" : headPart, model.empty() ? "-" : model, shape, vertexCount, triangleCount);
	}

	int StyleEntry::Specificity(const HairKey& a_key) const
	{
		int count = 0;
		if (!matchHeadPart.empty()) {
			if (ToLower(a_key.headPart) != matchHeadPart)
				return -1;
			++count;
		}
		if (!matchModel.empty()) {
			if (NormalizeModel(a_key.model) != matchModel)
				return -1;
			++count;
		}
		if (!matchShape.empty()) {
			if (ToLower(a_key.shape) != matchShape)
				return -1;
			++count;
		}
		return count;
	}

	std::filesystem::path StyleLibrary::GetDirectory()
	{
		return Util::PathHelpers::GetCommunityShaderPath() / "HairStrands";
	}

	std::filesystem::path StyleLibrary::GetUserFile()
	{
		return GetDirectory() / kUserFileName;
	}

	void StyleLibrary::LoadFile(const std::filesystem::path& a_path, std::vector<StyleEntry>& o_entries, size_t& io_order) const
	{
		std::ifstream file(a_path);
		if (!file.is_open()) {
			logger::warn("[HairStrands] Could not open style file {}", a_path.string());
			return;
		}
		const json root = json::parse(file, nullptr, false, true);
		if (root.is_discarded() || !root.is_object()) {
			logger::warn("[HairStrands] Skipping style file {}: not a JSON object", a_path.string());
			return;
		}
		auto styles = root.find("styles");
		if (styles == root.end() || !styles->is_array()) {
			logger::warn("[HairStrands] Skipping style file {}: no \"styles\" array", a_path.string());
			return;
		}

		size_t loaded = 0;
		for (const auto& item : *styles) {
			if (!item.is_object()) {
				logger::warn("[HairStrands] Skipping a style entry in {} that is not an object", a_path.string());
				continue;
			}
			StyleEntry entry;
			if (auto match = item.find("match"); match != item.end() && match->is_object()) {
				const auto read = [&](const char* a_key) -> std::string {
					auto it = match->find(a_key);
					return it != match->end() && it->is_string() ? it->get<std::string>() : std::string{};
				};
				entry.matchHeadPart = ToLower(read("headPart"));
				entry.matchModel = NormalizeModel(read("model"));
				entry.matchShape = ToLower(read("shape"));
			}
			entry.style = item;
			entry.style.erase("match");
			entry.sourceFile = a_path.filename().string();
			entry.order = io_order++;
			o_entries.push_back(std::move(entry));
			++loaded;
		}
		logger::info("[HairStrands] Loaded {} style entr{} from {}", loaded, loaded == 1 ? "y" : "ies", a_path.filename().string());
	}

	void StyleLibrary::Reload()
	{
		std::vector<StyleEntry> loaded;
		size_t order = 0;

		const auto directory = GetDirectory();
		std::error_code ec;
		if (std::filesystem::is_directory(directory, ec)) {
			std::vector<std::filesystem::path> files;
			for (const auto& item : std::filesystem::directory_iterator(directory, ec)) {
				if (!item.is_regular_file(ec))
					continue;
				const auto& path = item.path();
				if (ToLower(path.extension().string()) != ".json" || ToLower(path.filename().string()) == ToLower(kUserFileName))
					continue;
				files.push_back(path);
			}
			std::ranges::sort(files, [](const auto& a, const auto& b) { return ToLower(a.filename().string()) < ToLower(b.filename().string()); });
			for (const auto& path : files)
				LoadFile(path, loaded, order);
			if (std::filesystem::exists(GetUserFile(), ec))
				LoadFile(GetUserFile(), loaded, order);
		}

		{
			std::unique_lock lock(mutex);
			entries = std::move(loaded);
		}
		++generation;
	}

	std::optional<StyleEntry> StyleLibrary::Find(const HairKey& a_key) const
	{
		std::shared_lock lock(mutex);
		const StyleEntry* best = nullptr;
		int bestSpecificity = -1;
		for (const auto& entry : entries) {
			const int specificity = entry.Specificity(a_key);
			if (specificity < 0)
				continue;
			if (specificity > bestSpecificity || (specificity == bestSpecificity && best && entry.order > best->order)) {
				best = &entry;
				bestSpecificity = specificity;
			}
		}
		return best ? std::optional<StyleEntry>{ *best } : std::nullopt;
	}

	size_t StyleLibrary::GetEntryCount() const
	{
		std::shared_lock lock(mutex);
		return entries.size();
	}

	namespace
	{
		json ReadUserRoot()
		{
			json root;
			std::ifstream file(StyleLibrary::GetUserFile());
			if (file.is_open())
				root = json::parse(file, nullptr, false, true);
			if (root.is_discarded() || !root.is_object())
				root = json::object();
			if (!root.contains("styles") || !root["styles"].is_array())
				root["styles"] = json::array();
			root["version"] = 1;
			return root;
		}

		json MakeMatch(const HairKey& a_key)
		{
			json match = json::object();
			if (!a_key.headPart.empty())
				match["headPart"] = a_key.headPart;
			if (!a_key.model.empty())
				match["model"] = NormalizeModel(a_key.model);
			if (!a_key.shape.empty())
				match["shape"] = a_key.shape;
			return match;
		}

		// Match objects compare case-insensitively, like StyleEntry::Specificity.
		bool SameMatch(const json& a_item, const json& a_match)
		{
			auto it = a_item.find("match");
			const json empty = json::object();
			const json& match = it != a_item.end() && it->is_object() ? *it : empty;
			for (const char* key : { "headPart", "model", "shape" }) {
				const auto read = [&](const json& a_obj) {
					auto field = a_obj.find(key);
					return field != a_obj.end() && field->is_string() ? ToLower(field->get<std::string>()) : std::string{};
				};
				if (read(match) != read(a_match))
					return false;
			}
			return true;
		}

		bool WriteUserRoot(const json& a_root)
		{
			std::error_code ec;
			std::filesystem::create_directories(StyleLibrary::GetDirectory(), ec);
			std::ofstream file(StyleLibrary::GetUserFile(), std::ios::trunc);
			if (!file.is_open()) {
				logger::error("[HairStrands] Could not write {}", StyleLibrary::GetUserFile().string());
				return false;
			}
			file << a_root.dump(4);
			return file.good();
		}
	}

	bool StyleLibrary::SaveUserStyle(const HairKey& a_key, const StrandStyle& a_style)
	{
		json root = ReadUserRoot();
		const json match = MakeMatch(a_key);
		auto& styles = root["styles"];
		for (auto it = styles.begin(); it != styles.end();) {
			it = it->is_object() && SameMatch(*it, match) ? styles.erase(it) : it + 1;
		}
		json item = json::object();
		item["match"] = match;
		StyleToJson(a_style, item);
		styles.push_back(std::move(item));

		const bool written = WriteUserRoot(root);
		Reload();
		return written;
	}

	bool StyleLibrary::RemoveUserStyle(const HairKey& a_key)
	{
		json root = ReadUserRoot();
		const json match = MakeMatch(a_key);
		auto& styles = root["styles"];
		const auto before = styles.size();
		for (auto it = styles.begin(); it != styles.end();) {
			it = it->is_object() && SameMatch(*it, match) ? styles.erase(it) : it + 1;
		}
		bool written = true;
		if (styles.size() != before)
			written = WriteUserRoot(root);
		Reload();
		return written;
	}
}
