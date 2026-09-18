#pragma once

#include "Feature.h"
#include "Features/Wind/Grass/GrassWindState.h"
#include "Features/Wind/Runtime/WindRuntimeState.h"
#include "Features/Wind/Settings/WindSettings.h"
#include "Features/Wind/TransientWindImpulse.h"
#include "Features/Wind/Trees/TreeWindState.h"
#include "Features/Wind/UI/WindUIState.h"
#include "Features/Wind/WindEffects/WindEffect.h"
#include "Features/Wind/WindField.h"
#include "Globals.h"
#include "I18n/I18n.h"

#include <array>
#include <memory>
#include <mutex>
#include <span>
#include <vector>

/** Per-frame contribution Wind makes to the FeatureData cbuffer; see Wind::GetSharedWindData and FeatureBuffer.cpp. */
struct WindPermutationContribution
{
	float windIntensityOverride;
	uint32_t overrideWindIntensity;
	float treeTransientWindInfluenceDefault;
	float treeLeafTransientWindInfluenceDefault;
	float treeLeafTransientFlutterMaximumDefault;
	float treeTransientMaximumBendMultiplierDefault;
	float trunkWindBendSensitivity;
	float treeLeafBaseWindFlutterGain;
	uint32_t enableAmbientGrassWind;
	float grassWindSensitivity;
	float grassWindBendProfile;
	float grassWindCompressionToBend;
	float grassWindFlutterStrength;
	float grassWindFlutterFrequency;
};

/** Per-frame contribution Wind makes to the FeatureData cbuffer; see Wind::GetSharedWindData and FeatureBuffer.cpp. */
struct alignas(16) WindSharedData
{
	WindField::WindTuning tuning;
	float4 ambient;
	float4 previousAmbient;
	WindField::Field current;
	WindField::Field previous;
	WindField::Field transition;
	WindField::Field previousTransition;
	float4 transitionData;
	float4 springDebug;
	std::array<uint32_t, 4> activeCounts;
	std::array<WindField::TransientWindSource, WindField::kTransientImpulseCapacity> transientImpulses;
	std::array<WindField::TransientWindSource, WindField::kTransientImpulseCapacity> previousTransientImpulses;
};
static_assert(sizeof(WindSharedData) % 16 == 0, "WindSharedData must be 16-byte-safe to byte-copy into the FeatureData cbuffer");

struct Wind : Feature
{
	Wind();

	virtual std::string GetName() override { return "Wind"; }
	virtual std::string GetDisplayName() override { return T("feature.wind.name", "Wind"); }
	virtual std::string GetShortName() override { return "Wind"; }
	virtual std::string_view GetCategory() const override { return FeatureCategories::kGrass; }
	virtual bool IsCore() const override { return true; }

	virtual std::pair<std::string, std::vector<std::string>> GetFeatureSummary() override
	{
		return { T("feature.wind.description", "Shared ambient, tree, grass, and transient wind controls."),
			{ T("feature.wind.key_feature_1", "Procedural ambient gust field"),
				T("feature.wind.key_feature_2", "Tree and grass response tuning"),
				T("feature.wind.key_feature_3", "Transient wind impulses") } };
	}

	using Settings = WindSettings;
	using SettingsPage = WindSettingsPage;
	using PerFrameData = WindPerFrameData;
	using GrassWindSpringFieldData = ::GrassWindSpringFieldData;
	using GrassWindSpringData = ::GrassWindSpringData;
	using WindFieldDebugView = ::WindFieldDebugView;
	using RuntimeWindTest = ::RuntimeWindTest;

	enum class TransientWindSourceOwner : uint8_t
	{
		Generic,
		FusRoDah,
		Dragon,
		SpellShout,
		StormCall,
		ProjectileMagic,
		WeaponThrowVR,
		Explosion,
		HeavyImpact
	};

	/** Controls which sources survive when the shared pool reaches capacity. */
	enum class TransientWindSourcePriority : uint8_t
	{
		Wingbeat = 10,
		Flight = 20,
		Impact = 40,
		Breath = 50,
		FusRoDah = 60
	};

	struct TransientWindSourceSubmission
	{
		WindField::TransientWindSource source;
		TransientWindSourcePriority priority;
	};

	Settings settings;
	WindUIState uiState;
	WindRuntimeState runtimeState;
	GrassWindState grassState;
	TreeWindState treeState;

	// Wind effects live until process exit because their Bethesda event sinks remain registered for that lifetime.
	// ProjectileHookDispatcher observers are removed explicitly by their effect routers.
	std::vector<std::unique_ptr<WindEffect>> windEffects;

	float windFieldFrameTime = 0.0f;
	float windFieldAmbientSpeed = 0.0f;
	float windFieldAdvectionSpeed = 0.0f;
	float windFieldGustTravelDistance = 0.0f;
	float previousWindFieldGustTravelDistance = 0.0f;
	float windFieldTravelDelta = 0.0f;
	float3 ambientWindVelocity = {};
	float3 windFieldSelectedVelocity = {};
	float3 previousWindFieldSelectedVelocity = {};
	WindField::Field windFieldCurrent{};
	WindField::Field previousWindFieldCurrent{};
	WindField::Field windFieldTransition{};
	WindField::Field previousWindFieldTransition{};
	float windFieldTransitionElapsed = 0.0f;
	float windFieldTransitionBlend = 1.0f;
	float previousWindFieldTransitionBlend = 1.0f;
	bool windFieldTransitionActive = false;
	float windFieldSelectedSpeed = 0.0f;
	bool windFieldHasPreviousSample = false;
	WindField::WindTuning windFieldTuning{};

	/** @copydoc Feature::DrawSettings */
	virtual void DrawSettings() override;
	virtual void LoadSettings(json& o_json) override;
	virtual void SaveSettings(json& o_json) override;
	virtual void RestoreDefaultSettings() override;
	virtual void SetupResources() override;
	virtual void ClearShaderCache() override;
	virtual void PostPostLoad() override;
	virtual void DataLoaded() override;
	/** @brief Resets shared wind field/transient state on cell or scene transitions. */
	void OnSceneTransitionReset(bool a_opening);
	/** @brief Injects per-mesh tree-bend sensitivities before a tree draw call; see BSLightingShader/BSUtilityShader SetupGeometry hooks in the .cpp. */
	void OnTreeBendRenderPassBegin(RE::BSRenderPass* a_pass);
	/** @brief Advances the shared wind field and transient impulses for the current frame. */
	virtual void Reset() override;

	[[nodiscard]] PerFrameData GetCommonBufferData() const;
	[[nodiscard]] WindPermutationContribution GetPermutationContribution() const;
	[[nodiscard]] WindSharedData GetSharedWindData() const;
	[[nodiscard]] WindField::WindSample SampleWind(const float3& a_worldPosition) const noexcept;
	[[nodiscard]] WindField::WindSample SampleWind(const float3& a_worldPosition,
		const float3& a_windDirection, float a_windSpeed) const noexcept;
	void QueueTransientWindImpulse(const WindField::TransientWindSource& a_impulse);
	void QueueTransientWindSource(const WindField::TransientWindSource& a_source,
		TransientWindSourceOwner a_owner, TransientWindSourcePriority a_priority);
	void SetAttachedTransientWindSources(TransientWindSourceOwner a_owner,
		std::span<const TransientWindSourceSubmission> a_sources);
	/** @brief Removes active, pending, and attached sources owned by one producer. */
	void ClearTransientWindSources(TransientWindSourceOwner a_owner);
	/** @brief Unlike ClearTransientWindSources, clears every owner's sources, not just one. */
	void ClearTransientWindImpulses();
	void UpdateWindEffects(float a_frameTime);
	void SetTreeWindTestEnabled(bool a_enabled);
	[[nodiscard]] bool ShouldUseRealWindSpeed() const { return !runtimeState.treeWindTest.enabled && runtimeState.windFieldUseRealSpeed; }
	[[nodiscard]] float GetEffectiveWindOverrideSpeed() const { return runtimeState.treeWindTest.enabled ? runtimeState.treeWindTest.speed : runtimeState.windFieldOverrideSpeed; }
	[[nodiscard]] float GetEffectiveWindGustScale() const { return runtimeState.treeWindTest.enabled ? runtimeState.treeWindTest.gustScale : settings.windFieldGustScale; }
	[[nodiscard]] float GetEffectiveWindGustAmplitude() const { return runtimeState.treeWindTest.enabled ? runtimeState.treeWindTest.gustAmplitude : settings.windFieldGustAmplitude; }
	[[nodiscard]] float GetEffectiveWindGustAdvectionMultiplier() const { return runtimeState.treeWindTest.enabled ? runtimeState.treeWindTest.gustAdvectionMultiplier : settings.windFieldGustAdvectionMultiplier; }
	void UpdateGrassWindSpring(bool a_compute = false);
	void UpdateTreeWindSpring();
	void RecreateGrassWindSpringTextures(uint32_t a_qualityIndex, uint32_t a_textureSize);
	[[nodiscard]] ID3D11ShaderResourceView* GetGrassWindSpringDebugSRV() const;

private:
	struct Hooks;

	/** @brief Resets transient wind effects when the loading screen closes (cell/scene transition). */
	class SceneTransitionEventHandler : public RE::BSTEventSink<RE::MenuOpenCloseEvent>
	{
	public:
		virtual RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent* a_event, RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override;

		static bool Register()
		{
			static SceneTransitionEventHandler singleton;
			auto ui = globals::game::ui;
			if (!ui)
				return false;
			auto eventSource = ui->GetEventSource<RE::MenuOpenCloseEvent>();
			if (!eventSource)
				return false;
			eventSource->AddEventSink(&singleton);
			return true;
		}
	};

	bool IsTreeBendRenderPass(const RE::BSRenderPass* a_pass) const;
	void UpdateWind();
	void AdvanceWindHistory(float a_frameTime);
	void UpdateWeatherWind();
	void UpdateWindField(const float3& a_direction, float a_speed, float a_frameTime);
	void UpdateTransientWindImpulses(float a_frameTime);
	struct ManagedTransientWindSource
	{
		WindField::TransientWindSource source;
		TransientWindSourceOwner owner;
		TransientWindSourcePriority priority;
		uint64_t sequence;
	};
	std::array<WindField::TransientWindSource, WindField::kTransientImpulseCapacity> transientWindImpulses{};
	std::array<WindField::TransientWindSource, WindField::kTransientImpulseCapacity> previousTransientWindImpulses{};
	std::array<TransientWindSourceOwner, WindField::kTransientImpulseCapacity> transientWindImpulseOwners{};
	std::array<TransientWindSourceOwner, WindField::kTransientImpulseCapacity> previousTransientWindImpulseOwners{};
	uint32_t activeTransientWindImpulseCount = 0;
	uint32_t previousActiveTransientWindImpulseCount = 0;
	std::vector<ManagedTransientWindSource> activeTransientWindSources;
	std::vector<ManagedTransientWindSource> pendingTransientWindSources;
	std::vector<ManagedTransientWindSource> attachedTransientWindSources;
	uint64_t transientWindSourceSequence = 0;
	std::mutex transientWindImpulseMutex;
	static void SanitizeSettings(Settings& a_settings);
	static void SanitizeGrassWindSettings(Settings& a_settings);
	static uint32_t SanitizeGrassWindSpringTextureSize(uint32_t a_textureSize);
	[[nodiscard]] uint32_t GetTransientFieldMask() const;
	void DrawWindFieldSettings();
	void DrawWindEffectsSettings();
	void SpawnDebugWindEffects();
	void DrawTreeSettings();
	void DrawTreeWindTestSettings();
	void DrawTreeMeshSettings();
	void DrawTreeMeshConflictWarning();
	void DrawTreeGlobalOverrideSettings();
	void DrawTreeMeshRuleControls();
	void DrawTreeMeshRulesTable();
	void ResetGrassWindSettings();
	void DrawGrassWindSettings();
	void SetupGrassWindResources();
	void SetupTreeWindResources();
	void RecreateTreeWindSpringTextures(uint32_t a_qualityIndex);
};
