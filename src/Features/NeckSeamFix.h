#pragma once

#include "Buffer.h"

struct NeckSeamFix : Feature
{
private:
	static constexpr std::string_view MOD_ID = "0";  // placeholder — no Nexus page yet
	bool EnsureResources();
	void ReleaseRenderResources();
	void BSLightingShader_SetupGeometry(RE::BSRenderPass* a_pass);

public:
	static constexpr RE::RENDER_TARGETS::RENDER_TARGET LABELS_RENDER_TARGET = RE::RENDER_TARGETS::kLENSFLAREVIS;

	struct Settings
	{
		/// Maximum pixel radius to search for nearby skin pixels around the seam.
		float SearchRadius = 2.0f;

		/// Maximum linearised depth difference between the head/body edge samples
		/// that are allowed to participate in the same seam reconstruction.
		float DepthThreshold = 0.005f;

		/// Blend weight used for edge feathering and gap reconstruction.
		float BlendStrength = 1.0f;

		/// Maximum pixel radius for post-composite color offset correction.
		float LateSearchRadius = 2.0f;

		/// Blend weight for post-composite color offset correction.
		float LateBlendStrength = 0.8f;
	};

	Settings settings;

	/// \brief Aligned constant buffer uploaded to the GPU each frame.
	struct alignas(16) NeckSeamCB
	{
		float SearchRadius;
		float DepthThreshold;
		float BlendStrength;
		float LateSearchRadius;
		float LateBlendStrength;
		float pad[3];
	};
	STATIC_ASSERT_ALIGNAS_16(NeckSeamCB);

	struct alignas(16) NeckSeamPerGeometryCB
	{
		float ObjectId;
		float Flags;
		float pad[2];
	};
	STATIC_ASSERT_ALIGNAS_16(NeckSeamPerGeometryCB);

	ConstantBuffer* neckSeamCB = nullptr;
	ConstantBuffer* neckSeamPerGeometryCB = nullptr;
	ID3D11ComputeShader* neckSeamCS = nullptr;
	ID3D11ComputeShader* neckSeamLateCS = nullptr;
	Texture2D* seamMainTexture = nullptr;
	Texture2D* seamAlbedoTexture = nullptr;
	Texture2D* seamSpecularTexture = nullptr;
	Texture2D* seamReflectanceTexture = nullptr;
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

	virtual bool HasShaderDefine(RE::BSShader::Type a_type) override { return a_type == RE::BSShader::Type::Lighting; }
	virtual bool SupportsVR() override { return true; }

	// -------------------------------------------------------------------------
	// Lifecycle
	// -------------------------------------------------------------------------

	virtual void SetupResources() override;
	virtual void Reset() override { seamOutputsValid = false; }
	virtual void RestoreDefaultSettings() override;
	virtual void PostPostLoad() override;

	// -------------------------------------------------------------------------
	// Settings UI
	// -------------------------------------------------------------------------

	virtual void DrawSettings() override;

	// -------------------------------------------------------------------------
	// Rendering
	// -------------------------------------------------------------------------

	/// \brief Dispatches the seam-fix compute shader.  Called from Deferred::DeferredPasses().
	void DrawSeamFix();
	/// \brief Applies post-composite color offset correction across detected skin seams.
	void DrawSeamFixLate();
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
	ID3D11ShaderResourceView* GetSpecularSRV() const
	{
		if (!seamOutputsValid)
			return nullptr;
		return seamSpecularTexture ? seamSpecularTexture->srv.get() : nullptr;
	}
	ID3D11ShaderResourceView* GetReflectanceSRV() const
	{
		if (!seamOutputsValid)
			return nullptr;
		return seamReflectanceTexture ? seamReflectanceTexture->srv.get() : nullptr;
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
	ID3D11ComputeShader* GetLateComputeShader();

	struct Hooks
	{
		struct BSLightingShader_SetupGeometry
		{
			static void thunk(RE::BSShader* a_shader, RE::BSRenderPass* a_pass, uint32_t a_renderFlags);
			static inline REL::Relocation<decltype(thunk)> func;
		};

		static void Install()
		{
			stl::write_vfunc<0x6, BSLightingShader_SetupGeometry>(RE::VTABLE_BSLightingShader[0]);
			logger::info("[Neck Seam Fix] Installed hooks");
		}
	};
};
