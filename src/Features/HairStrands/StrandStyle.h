#pragma once

#include <filesystem>
#include <optional>
#include <shared_mutex>
#include <string>
#include <vector>

/**
 * Hair strand styles: how a hair mesh is turned into strands and how those strands look.
 *
 * A style is resolved in three layers: the preset's defaults, then every field an authored
 * style entry sets explicitly, then (in-game editor only) unsaved edits. Fields are split
 * into generation fields, which rebuild the strand asset when they change, and render
 * fields, which the vertex shader applies every frame and so update instantly.
 */
namespace Strands
{
	/** @brief Hair texture presets. Auto picks one from the hair's names, else Straight. */
	enum class HairPreset : uint32_t
	{
		Auto,
		Straight,
		Wavy,
		Curly,
		Coily,  // tight type-4 coils: afro-textured hair
		Locs,   // rope-like clumps: locs, braids, twists
		Count
	};

	/** @brief Where strands start. Auto uses Roots, and Area for very short hair. */
	enum class SeedMode : uint32_t
	{
		Auto,
		Roots,  // along the card edges the hair flows out of
		Area,   // scattered over the surface, each a short strand (buzz cuts, fuzz)
		Count
	};

	/** @brief Which texture direction runs from root to tip. Auto follows the flow map, else the way each atlas strip's strands are painted, oriented away from the head. */
	enum class FlowAxis : uint32_t
	{
		Auto,
		V,
		NegV,
		U,
		NegU,
		Count
	};

	/** @brief A UV rectangle of the hair texture: its triangles are left as cards, or hang on a chain. */
	struct UVRect
	{
		float minU = 0.0f;
		float minV = 0.0f;
		float maxU = 0.0f;
		float maxV = 0.0f;

		bool Contains(float u, float v) const { return u >= minU && u <= maxU && v >= minV && v <= maxV; }
		bool operator==(const UVRect&) const = default;
	};

	/** @brief Every authored parameter of a hairstyle. Units are Skyrim units (about 1.4 cm). */
	struct StrandStyle
	{
		bool enabled = true;
		HairPreset preset = HairPreset::Auto;

		// Generation: changing any of these rebuilds the strand asset.
		SeedMode seeding = SeedMode::Auto;
		FlowAxis flowAxis = FlowAxis::Auto;
		float density = 20.0f;           // strands per unit of root edge (Area: per square unit x 4)
		float segmentLength = 1.0f;      // control-point spacing
		float lengthScale = 1.0f;        // fraction of the traced length kept
		float volume = 0.15f;            // lift off the card surface towards the tip
		float layerJitter = 0.12f;       // random root offset along the normal, for depth
		float clumpStrength = 0.25f;     // pull towards the clump centre towards the tip
		float clumpSize = 1.5f;          // clump radius at the root
		float clumpTwist = 0.0f;         // turns per unit around the clump centre (locs, twists)
		float shortLength = 1.2f;        // strand length when seeding by area
		float coverageThreshold = 0.3f;  // card texture alpha below this has no hair (0: ignore the texture)
		uint32_t seed = 1;
		std::vector<UVRect> excludeUV;  // triangles kept as cards (scalp caps, hairlines, ribbons, beads)
		// Hair that is not loose stays cards: braids, twists, ties, buns and the hair gathered into
		// them. Braids hanging free swing on chains of their own.
		bool keepWoven = true;
		std::vector<UVRect> chainUV;  // triangles kept as cards that hang on a chain
		// A .skhair file (the Skyrim Hair Designer's export), relative to Data: the hair is loaded
		// from it rather than converted, and the generation fields above are ignored. Hair the file
		// does not cover keeps its cards; a file that no longer fits the mesh is ignored (logged).
		// A hair with no style entry uses the .skhair beside its nif (hair.nif -> hair.skhair).
		std::string asset;

		// Render: the vertex shader applies these every frame.
		float rootWidth = 0.06f;
		float tipWidth = 0.015f;
		float waveAmplitude = 0.0f;
		float waveLength = 4.0f;
		float curlRadius = 0.0f;
		float curlLength = 1.5f;  // length of one curl turn
		float curlStart = 0.1f;   // fraction of the strand before curls reach full radius
		float frizz = 0.02f;
		float flyaways = 0.02f;  // fraction of strands that stray from the style

		// Motion: TressFX 4.1's simulation settings (TressFXSimulationSettings), which the
		// simulation reads every frame. As in TressFX they apply per step (1/60 s here), with
		// lengths in units: TressFX's sample values for the threshold and clamp carry over.
		bool simulate = true;
		float vspCoeff = 0.4f;                    // share of the root segment's motion each step passes rigidly to the strand
		float vspAccelThreshold = 1.208f;         // root pseudo-acceleration (units per step^2) past which that share is 1
		float localConstraintStiffness = 0.908f;  // how firmly each segment keeps its rest angle to the one before it
		uint32_t localConstraintsIterations = 3;
		float globalConstraintStiffness = 0.408f;  // pull towards the styled shape per step, within the global range
		float globalConstraintsRange = 0.4f;       // fraction of the strand, from the root, the global constraint holds
		uint32_t lengthConstraintsIterations = 10;
		float damping = 0.068f;            // velocity lost per step (air drag)
		float gravityMagnitude = 100.0f;   // units/s^2 (Earth's is about 687)
		float tipSeparation = 0.0f;        // how far strands spread from their guide towards the tip
		float clampPositionDelta = 20.0f;  // largest move of a point in a step, in units
		float windResponse = 1.0f;         // how much the weather's wind moves the hair

		// Motion of braids hanging on chains (CardsToStrands::ChainSettings), per 1/60 s step.
		float chainStiffness = 0.4f;  // pull back towards the styled shape: 0 limp, 1 rigid
		float chainDamping = 0.12f;   // velocity lost
		float chainGravity = 687.0f;  // units/s^2 (Skyrim's)

		bool operator==(const StrandStyle&) const = default;

		/** @brief True if a change from this style to another requires regenerating the asset. */
		bool GenerationDiffers(const StrandStyle& a_other) const;
		/** @brief Hash of the generation fields and the asset file's name and write time (the part of the asset cache key a style owns). */
		uint64_t GenerationHash() const;
	};

	/** @brief Ranges every style is clamped to on load, shared with the editor's sliders. */
	namespace StyleLimits
	{
		inline constexpr float kMinDensity = 1.0f;
		inline constexpr float kMaxDensity = 60.0f;
		inline constexpr float kMinSegmentLength = 0.25f;
		inline constexpr float kMaxSegmentLength = 4.0f;
		inline constexpr float kMaxVolume = 3.0f;
		inline constexpr float kMaxClumpSize = 6.0f;
		inline constexpr float kMaxTwist = 2.0f;
		inline constexpr float kMaxShortLength = 6.0f;
		inline constexpr float kMinWidth = 0.005f;
		inline constexpr float kMaxWidth = 0.5f;
		inline constexpr float kMaxWaveAmplitude = 2.0f;
		inline constexpr float kMinPeriod = 0.2f;
		inline constexpr float kMaxPeriod = 20.0f;
		inline constexpr float kMaxCurlRadius = 1.5f;
		inline constexpr float kMaxVspAccelThreshold = 100.0f;
		inline constexpr uint32_t kMaxLocalIterations = 8;
		inline constexpr uint32_t kMaxLengthIterations = 16;
		inline constexpr float kMaxGravityMagnitude = 1400.0f;
		inline constexpr float kMaxTipSeparation = 2.0f;
		inline constexpr float kMinClampPositionDelta = 0.5f;
		inline constexpr float kMaxClampPositionDelta = 200.0f;
		inline constexpr float kMaxWindResponse = 3.0f;
		inline constexpr size_t kMaxExcludeRects = 32;
		inline constexpr float kMaxChainGravity = 1400.0f;
	}

	/** @brief Clamps every field to StyleLimits so a malformed style file cannot break generation. */
	void Sanitize(StrandStyle& a_style);

	/** @brief Returns the defaults of a preset (Auto returns Straight's). */
	StrandStyle MakePresetStyle(HairPreset a_preset);

	/** @brief Guesses a preset from the hair's editor ID, model path and shape name. */
	HairPreset GuessPreset(std::string_view a_headPart, std::string_view a_model, std::string_view a_shape);

	std::string_view PresetName(HairPreset a_preset);

	/** @brief Writes every field of a style (the in-game editor saves fully resolved styles). */
	void StyleToJson(const StrandStyle& a_style, json& o_json);
	/**
	 * @brief Resolves a style entry: the preset's defaults, then every field the entry sets.
	 * @param a_json      The style entry.
	 * @param a_autoPreset Preset used when the entry's preset is Auto (or absent).
	 */
	StrandStyle StyleFromJson(const json& a_json, HairPreset a_autoPreset);

	/** @brief Identifies one hair shape: which head part, model and shape it comes from. */
	struct HairKey
	{
		std::string headPart;  // editor ID of the hair or facial-hair head part, empty for wigs
		std::string model;     // lowercase model path relative to Meshes, empty when unknown
		std::string shape;     // the geometry's node name
		uint32_t vertexCount = 0;
		uint32_t triangleCount = 0;

		bool operator==(const HairKey&) const = default;
		std::string ToString() const;
	};

	/** @brief One authored entry: which hair it applies to and its raw style JSON. */
	struct StyleEntry
	{
		std::string matchHeadPart;
		std::string matchModel;
		std::string matchShape;
		json style;
		std::string sourceFile;
		size_t order = 0;  // load order; later entries win ties

		/** @brief Number of match fields set, or -1 if the entry does not match a_key. */
		int Specificity(const HairKey& a_key) const;
	};

	/**
	 * Loads authored styles from Data/SKSE/Plugins/CommunityShaders/HairStrands/*.json and
	 * answers which style applies to a hair shape. Files load in name order and UserStyles.json
	 * (the in-game editor's file) last, so the editor's saves win ties.
	 *
	 * File format:
	 * @code
	 * { "version": 1, "styles": [
	 *     { "match": { "headPart": "HairFemaleNord01", "model": "...\\hair.nif", "shape": "Hair" },
	 *       "preset": "curly", "mode": "replace", "curlRadius": 0.3 } ] }
	 * @endcode
	 * Every match field is optional (case-insensitive, all given fields must match); the most
	 * specific matching entry wins. `"enabled": false` excludes a hair from conversion.
	 */
	class StyleLibrary
	{
	public:
		/** @brief Directory the style files are read from. */
		static std::filesystem::path GetDirectory();
		/** @brief The in-game editor's file. */
		static std::filesystem::path GetUserFile();

		/** @brief (Re)reads every style file. Malformed files and entries are skipped and logged. */
		void Reload();

		/** @brief The authored style entry for a hair shape, or nothing if none matches. */
		std::optional<StyleEntry> Find(const HairKey& a_key) const;

		/**
		 * @brief Saves a resolved style for exactly this hair (head part, model and shape) to
		 * UserStyles.json, replacing an existing entry with the same match, then reloads.
		 * @return false (with a logged error) if the file could not be written.
		 */
		bool SaveUserStyle(const HairKey& a_key, const StrandStyle& a_style);

		/** @brief Removes this hair's UserStyles.json entry, if any, then reloads. */
		bool RemoveUserStyle(const HairKey& a_key);

		/** @brief Incremented on every reload so cached resolutions can be invalidated. */
		uint32_t GetGeneration() const { return generation.load(); }

		size_t GetEntryCount() const;

	private:
		void LoadFile(const std::filesystem::path& a_path, std::vector<StyleEntry>& o_entries, size_t& io_order) const;

		mutable std::shared_mutex mutex;
		std::vector<StyleEntry> entries;
		std::atomic<uint32_t> generation{ 0 };
	};
}
