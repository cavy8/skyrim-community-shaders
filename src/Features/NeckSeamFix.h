#pragma once

#include "Buffer.h"

struct NeckSeamFix : Feature
{
private:
	static constexpr std::string_view MOD_ID = "0";  // placeholder — no Nexus page yet

public:
	struct Settings
	{
		/// Maximum pixel radius to search for skin neighbours on each side of a seam.
		float SearchRadius = 2.0f;

		/// Maximum linearised depth difference (metres) between a candidate gap
		/// pixel and its skin neighbours for them to be considered the same surface.
		float DepthThreshold = 0.005f;

		/// Blend weight: 0 = no fill, 1 = full replacement with averaged neighbour colour.
		float BlendStrength = 1.0f;
	};

	Settings settings;

	/// \brief Aligned constant buffer uploaded to the GPU each frame.
	struct alignas(16) NeckSeamCB
	{
		float SearchRadius;
		float DepthThreshold;
		float BlendStrength;
		float pad;
	};
	STATIC_ASSERT_ALIGNAS_16(NeckSeamCB);

	ConstantBuffer* neckSeamCB = nullptr;
	ID3D11ComputeShader* neckSeamCS = nullptr;

	// -------------------------------------------------------------------------
	// Feature interface
	// -------------------------------------------------------------------------

	virtual inline std::string GetName() override { return "Neck Seam Fix"; }
	virtual inline std::string GetShortName() override { return "NeckSeamFix"; }
	virtual inline std::string_view GetShaderDefineName() override { return "NECK_SEAM_FIX"; }
	virtual inline std::string_view GetCategory() const override { return FeatureCategories::kCharacters; }

	virtual std::pair<std::string, std::vector<std::string>> GetFeatureSummary() override
	{
		return {
			"Neck Seam Fix closes the 1–2 pixel gap that sometimes appears between Skyrim's\n"
			"separately-rendered head and body meshes. It works as a post-process compute\n"
			"pass that detects thin gaps bounded by skin pixels and fills them using colour\n"
			"data sampled from neighbouring geometry.",
			{ "Eliminates visible neck seam gaps",
				"Operates in screen space — works with any body or head mod",
				"Configurable search radius and blend strength",
				"Low GPU overhead (single compute dispatch)" }
		};
	}

	virtual bool HasShaderDefine(RE::BSShader::Type) override { return false; }
	virtual bool SupportsVR() override { return true; }

	// -------------------------------------------------------------------------
	// Lifecycle
	// -------------------------------------------------------------------------

	virtual void SetupResources() override;
	virtual void Reset() override {}
	virtual void RestoreDefaultSettings() override;

	// -------------------------------------------------------------------------
	// Settings UI
	// -------------------------------------------------------------------------

	virtual void DrawSettings() override;

	// -------------------------------------------------------------------------
	// Rendering
	// -------------------------------------------------------------------------

	/// \brief Dispatches the seam-fix compute shader.  Called from Deferred::DeferredPasses().
	void DrawSeamFix();

	// -------------------------------------------------------------------------
	// Serialisation
	// -------------------------------------------------------------------------

	virtual void LoadSettings(json& o_json) override;
	virtual void SaveSettings(json& o_json) override;

	// -------------------------------------------------------------------------
	// Shader cache
	// -------------------------------------------------------------------------

	virtual void ClearShaderCache() override;
	ID3D11ComputeShader* GetComputeShader();
};
