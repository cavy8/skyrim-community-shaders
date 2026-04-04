#pragma once

#include "Buffer.h"

struct NeckSeamFix : Feature
{
private:
	static constexpr std::string_view MOD_ID = "0";  // placeholder — no Nexus page yet
	bool EnsureResources();
	void ReleaseRenderResources();

public:
	struct Settings
	{
		/// Maximum pixel radius to search for nearby skin pixels around the seam.
		float SearchRadius = 2.0f;

		/// Maximum linearised depth difference between the head/body edge samples
		/// that are allowed to participate in the same seam reconstruction.
		float DepthThreshold = 0.005f;

		/// Blend weight used for edge feathering and gap reconstruction.
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
	Texture2D* seamMainTexture = nullptr;
	Texture2D* seamAlbedoTexture = nullptr;
	Texture2D* seamNormalRoughnessTexture = nullptr;
	Texture2D* seamMasksTexture = nullptr;
	Texture2D* seamDepthTexture = nullptr;
	Texture2D* seamDepthTexture16 = nullptr;
	bool seamOutputsValid = false;

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
			"Neck Seam Fix reconstructs and blends the thin seam that can appear between\n"
			"Skyrim's separately-rendered head and body skin meshes. It patches the seam\n"
			"in screen space before later lighting passes so gaps, exposed interior pixels,\n"
			"and hard body/head edge transitions are smoothed together.",
			{ "Fills narrow head/body skin gaps",
				"Blends touching seam edges, not just empty holes",
				"Updates seam data before later screen-space lighting",
				"Configurable search radius and blend strength" }
		};
	}

	virtual bool HasShaderDefine(RE::BSShader::Type) override { return false; }
	virtual bool SupportsVR() override { return true; }

	// -------------------------------------------------------------------------
	// Lifecycle
	// -------------------------------------------------------------------------

	virtual void SetupResources() override;
	virtual void Reset() override { seamOutputsValid = false; }
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
	ID3D11ShaderResourceView* GetDepthSRV(bool prefer16bit) const
	{
		if (!seamOutputsValid)
			return nullptr;
		if (prefer16bit)
			return seamDepthTexture16 ? seamDepthTexture16->srv.get() : nullptr;
		return seamDepthTexture ? seamDepthTexture->srv.get() : nullptr;
	}
	ID3D11ShaderResourceView* GetAlbedoSRV() const
	{
		if (!seamOutputsValid)
			return nullptr;
		return seamAlbedoTexture ? seamAlbedoTexture->srv.get() : nullptr;
	}
	ID3D11ShaderResourceView* GetNormalRoughnessSRV() const
	{
		if (!seamOutputsValid)
			return nullptr;
		return seamNormalRoughnessTexture ? seamNormalRoughnessTexture->srv.get() : nullptr;
	}
	ID3D11ShaderResourceView* GetMasksSRV() const
	{
		if (!seamOutputsValid)
			return nullptr;
		return seamMasksTexture ? seamMasksTexture->srv.get() : nullptr;
	}

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
