#include "Wind.h"

#include "Features/Wind/TransientWindImpulse.h"
#include "Features/Wind/WindEffects/DragonWind.h"
#include "Features/Wind/WindEffects/ExplosionWindRouter.h"
#include "Features/Wind/WindEffects/FusRoDahWind.h"
#include "Features/Wind/WindEffects/HeavyImpactWindRouter.h"
#include "Features/Wind/WindEffects/ProjectileMagicWindRouter.h"
#include "Features/Wind/WindEffects/SpellShoutWindRouter.h"
#include "Features/Wind/WindEffects/StormCallWindRouter.h"
#include "Features/Wind/WindEffects/WeaponThrowVRWind.h"
#include "Features/Wind/WindMath.h"
#include "Globals.h"
#include "ShaderCache.h"
#include "State.h"
#include "Trees/TreeWindPatcher.h"
#include "Utils/Format.h"

#include "RE/B/BSGeometry.h"
#include "RE/B/BSLeafAnimNode.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <string_view>

using namespace WindSettingsLimits;
using WindMath::ClampFiniteOrDefault;

namespace
{
	constexpr uint32_t kNearTransientFieldMask = 1u << 0u;
	constexpr uint32_t kMidTransientFieldMask = 1u << 1u;
	constexpr uint32_t kFarTransientFieldMask = 1u << 2u;

	constexpr auto kTreeBendDescriptor = static_cast<uint32_t>(State::ExtraShaderDescriptors::TreeBend);

	bool IsExplicitTrunkGeometry(const RE::BSGeometry* a_geometry)
	{
		if (!a_geometry)
			return false;

		const char* geometryName = a_geometry->name.c_str();
		return geometryName && (std::strcmp(geometryName, "trunk") == 0 || std::strncmp(geometryName, "OS_TRUNK", 8) == 0);
	}

	bool IsExplicitTreeLeavesGeometry(const RE::BSGeometry* a_geometry)
	{
		if (!a_geometry || !netimmerse_cast<RE::BSLeafAnimNode*>(a_geometry->parent))
			return false;

		const char* geometryName = a_geometry->name.c_str();
		return geometryName && std::strcmp(geometryName, "leaves") == 0;
	}

	bool IsTreeGeometry(const RE::BSGeometry* a_geometry)
	{
		return IsExplicitTrunkGeometry(a_geometry) || IsExplicitTreeLeavesGeometry(a_geometry);
	}

	bool SupportsTreeBend(const RE::BSShader& a_shader, uint32_t a_vertexDescriptor)
	{
		if (a_shader.shaderType == RE::BSShader::Type::Lighting) {
			const auto technique = static_cast<SIE::ShaderCache::LightingShaderTechniques>((a_vertexDescriptor >> 24) & 0x3F);
			return technique != SIE::ShaderCache::LightingShaderTechniques::LODObjects &&
			       technique != SIE::ShaderCache::LightingShaderTechniques::LODObjectHD;
		}

		return a_shader.shaderType == RE::BSShader::Type::Utility &&
		       (a_vertexDescriptor & static_cast<uint32_t>(SIE::ShaderCache::UtilityShaderFlags::LodObject)) == 0;
	}

	uint32_t GetRenderPassVertexDescriptor(const RE::BSRenderPass& a_pass)
	{
		constexpr uint32_t kLightingTechniqueStart = 0x4800002D;
		if (a_pass.shader && a_pass.shader->shaderType == RE::BSShader::Type::Lighting && a_pass.passEnum >= kLightingTechniqueStart)
			return a_pass.passEnum - kLightingTechniqueStart;
		return a_pass.passEnum;
	}

	bool IsTreeRenderPass(const RE::BSRenderPass* a_pass)
	{
		if (!a_pass || !a_pass->shader || !a_pass->geometry)
			return false;

		const auto shaderType = a_pass->shader->shaderType.get();
		return (shaderType == RE::BSShader::Type::Lighting || shaderType == RE::BSShader::Type::Utility) &&
		       IsTreeGeometry(a_pass->geometry);
	}
}

Wind::Wind()
{
	windEffects.emplace_back(std::make_unique<FusRoDahWind>());
	windEffects.emplace_back(std::make_unique<DragonWind>());
	windEffects.emplace_back(std::make_unique<SpellShoutWindRouter>());
	windEffects.emplace_back(std::make_unique<ProjectileMagicWindRouter>());
	windEffects.emplace_back(std::make_unique<ExplosionWindRouter>());
	windEffects.emplace_back(std::make_unique<StormCallWindRouter>());
	windEffects.emplace_back(std::make_unique<WeaponThrowVRWind>());
	windEffects.emplace_back(std::make_unique<HeavyImpactWindRouter>());
}

void Wind::SanitizeSettings(Settings& a_settings)
{
	const Settings defaults{};
	a_settings.trunkWindBendSensitivity = ClampFiniteOrDefault(a_settings.trunkWindBendSensitivity,
		kTrunkWindSensitivityMin, kTrunkWindSensitivityMax, defaults.trunkWindBendSensitivity);
	a_settings.treeLeafBaseWindFlutterGain = ClampFiniteOrDefault(a_settings.treeLeafBaseWindFlutterGain,
		kTreeLeafBaseWindFlutterGainMin, kTreeLeafBaseWindFlutterGainMax, defaults.treeLeafBaseWindFlutterGain);
	a_settings.treeWindGustScale = ClampFiniteOrDefault(a_settings.treeWindGustScale,
		kTreeWindGustScaleMin, kTreeWindGustScaleMax, defaults.treeWindGustScale);
	a_settings.treeWindGustSoftLimit = ClampFiniteOrDefault(a_settings.treeWindGustSoftLimit,
		kTreeWindGustSoftLimitMin, kTreeWindGustSoftLimitMax, defaults.treeWindGustSoftLimit);
	a_settings.treeWindSpringFrequency = ClampFiniteOrDefault(a_settings.treeWindSpringFrequency,
		kTreeWindSpringFrequencyMin, kTreeWindSpringFrequencyMax, defaults.treeWindSpringFrequency);
	a_settings.treeWindSpringDamping = ClampFiniteOrDefault(a_settings.treeWindSpringDamping,
		kTreeWindSpringDampingMin, kTreeWindSpringDampingMax, defaults.treeWindSpringDamping);
	a_settings.treeTransientSpringFrequency = ClampFiniteOrDefault(a_settings.treeTransientSpringFrequency,
		kTreeTransientSpringFrequencyMin, kTreeTransientSpringFrequencyMax, defaults.treeTransientSpringFrequency);
	a_settings.treeTransientSpringDamping = ClampFiniteOrDefault(a_settings.treeTransientSpringDamping,
		kTreeTransientSpringDampingMin, kTreeTransientSpringDampingMax, defaults.treeTransientSpringDamping);
	a_settings.windFieldGustScale = ClampFiniteOrDefault(a_settings.windFieldGustScale,
		kWindFieldGustScaleMin, kWindFieldGustScaleMax, defaults.windFieldGustScale);
	a_settings.windFieldGustCrosswindScale = ClampFiniteOrDefault(a_settings.windFieldGustCrosswindScale,
		kWindFieldGustCrosswindScaleMin, kWindFieldGustCrosswindScaleMax, defaults.windFieldGustCrosswindScale);
	a_settings.windFieldGustAmplitude = ClampFiniteOrDefault(a_settings.windFieldGustAmplitude,
		kWindFieldGustAmplitudeMin, kWindFieldGustAmplitudeMax, defaults.windFieldGustAmplitude);
	a_settings.windFieldGustAdvectionMultiplier = ClampFiniteOrDefault(a_settings.windFieldGustAdvectionMultiplier,
		kWindFieldGustAdvectionMultiplierMin, kWindFieldGustAdvectionMultiplierMax, defaults.windFieldGustAdvectionMultiplier);
	a_settings.windFieldDirectionTransitionDuration = ClampFiniteOrDefault(a_settings.windFieldDirectionTransitionDuration,
		kWindFieldDirectionTransitionDurationMin, kWindFieldDirectionTransitionDurationMax,
		defaults.windFieldDirectionTransitionDuration);
	SanitizeGrassWindSettings(a_settings);
}

uint32_t Wind::GetTransientFieldMask() const
{
	return kNearTransientFieldMask |
	       (settings.processMidRangeTransients ? kMidTransientFieldMask : 0u) |
	       (settings.processFarRangeTransients ? kFarTransientFieldMask : 0u);
}

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	Wind::Settings::GrassWindSpringQualityRange,
	textureSize,
	maxDistance)

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	Wind::Settings,
	enableTrunkBend,
	overrideTrunkWindIntensity,
	trunkWindIntensityOverride,
	trunkWindBendSensitivity,
	treeLeafBaseWindFlutterGain,
	treeWindGustScale,
	treeWindGustSoftLimit,
	treeWindSpringFrequency,
	treeWindSpringDamping,
	treeTransientSpringFrequency,
	treeTransientSpringDamping,
	windFieldGustScale,
	windFieldGustCrosswindScale,
	windFieldGustAmplitude,
	windFieldGustAdvectionMultiplier,
	windFieldDirectionTransitionDuration,
	processMidRangeTransients,
	processFarRangeTransients,
	enableAmbientGrassWind,
	grassWindResponse,
	grassWindSensitivity,
	grassWindMaximumTilt,
	grassWindBendProfile,
	grassWindCompressionToBend,
	grassWindSpringFrequency,
	grassWindSpringDamping,
	grassWindSpringQuality,
	grassWindFlutterStrength,
	grassWindFlutterFrequency)

void Wind::SetTreeWindTestEnabled(bool a_enabled)
{
	if (runtimeState.treeWindTest.enabled == a_enabled)
		return;

	if (a_enabled) {
		runtimeState.treeWindTest.speed = ClampFiniteOrDefault(windFieldSelectedSpeed, 0.0f, 2.0f, 1.0f);
		runtimeState.treeWindTest.gustScale = ClampFiniteOrDefault(windFieldTuning.gustScale,
			kWindFieldGustScaleMin, kWindFieldGustScaleMax, settings.windFieldGustScale);
		runtimeState.treeWindTest.gustAmplitude = ClampFiniteOrDefault(windFieldTuning.gustAmplitude,
			kWindFieldGustAmplitudeMin, kWindFieldGustAmplitudeMax, settings.windFieldGustAmplitude);
		runtimeState.treeWindTest.gustAdvectionMultiplier = ClampFiniteOrDefault(
			windFieldTuning.gustAdvectionMultiplier,
			kWindFieldGustAdvectionMultiplierMin, kWindFieldGustAdvectionMultiplierMax, settings.windFieldGustAdvectionMultiplier);
	}
	runtimeState.treeWindTest.enabled = a_enabled;
}

void Wind::LoadSettings(json& o_json)
{
	const Settings defaults{};
	settings = o_json;
	if (!o_json.contains("windFieldGustCrosswindScale")) {
		const float legacyGustScale = ClampFiniteOrDefault(settings.windFieldGustScale,
			kWindFieldGustScaleMin, kWindFieldGustScaleMax, defaults.windFieldGustScale);
		settings.windFieldGustCrosswindScale = legacyGustScale * WindField::WindTuning{}.frontAspectRatio;
	}
	if (!o_json.contains("grassWindSpringQuality")) {
		if (o_json.contains("grassWindSpringTextureSize") && o_json["grassWindSpringTextureSize"].is_number_unsigned()) {
			const auto legacyTextureSize = o_json["grassWindSpringTextureSize"].get<uint32_t>();
			for (auto& range : settings.grassWindSpringQuality)
				range.textureSize = legacyTextureSize;
		}
		if (o_json.contains("grassWindSpringWorldSize") && o_json["grassWindSpringWorldSize"].is_number()) {
			const float legacyWorldSize = o_json["grassWindSpringWorldSize"].get<float>();
			if (std::isfinite(legacyWorldSize))
				settings.grassWindSpringQuality.back().maxDistance = legacyWorldSize * 0.5f;
		}
	}
	if (!o_json.contains("grassWindSpringFrequency") && o_json.contains("grassWindSpringLag") &&
		o_json["grassWindSpringLag"].is_number()) {
		const float legacyLag = std::max(o_json["grassWindSpringLag"].get<float>(), 0.01f);
		settings.grassWindSpringFrequency = 0.25f / legacyLag;
	}
	if (!o_json.contains("grassWindSpringDamping"))
		settings.grassWindSpringDamping = defaults.grassWindSpringDamping;
	if (!o_json.contains("treeLeafBaseWindFlutterGain")) {
		if (o_json.contains("treeLeafWindSensitivity") && o_json["treeLeafWindSensitivity"].is_number())
			settings.treeLeafBaseWindFlutterGain =
				o_json["treeLeafWindSensitivity"].get<float>() * defaults.treeLeafBaseWindFlutterGain;
		else if (o_json.contains("treeLeafAmbientSensitivity") && o_json["treeLeafAmbientSensitivity"].is_number())
			settings.treeLeafBaseWindFlutterGain =
				o_json["treeLeafAmbientSensitivity"].get<float>() * defaults.treeLeafBaseWindFlutterGain;
	}
	const auto windEffectSettings = o_json.find("windEffects");
	const bool hasWindEffectSettings = windEffectSettings != o_json.end() && windEffectSettings->is_object();
	const json emptyEffectSettings = json::object();
	for (auto& effect : windEffects) {
		const std::string effectId(effect->GetId());
		if (hasWindEffectSettings) {
			const auto effectSettings = windEffectSettings->find(effectId);
			effect->LoadSettings(effectSettings != windEffectSettings->end() && effectSettings->is_object() ?
									 *effectSettings :
									 emptyEffectSettings);
		} else {
			effect->LoadSettings(o_json);
		}
	}
	SanitizeSettings(settings);
}

void Wind::SaveSettings(json& o_json)
{
	SanitizeSettings(settings);
	o_json = settings;
	json windEffectSettings = json::object();
	for (const auto& effect : windEffects) {
		json effectSettings;
		effect->SaveSettings(effectSettings);
		windEffectSettings[std::string(effect->GetId())] = std::move(effectSettings);
	}
	o_json["windEffects"] = std::move(windEffectSettings);
}

void Wind::RestoreDefaultSettings()
{
	settings = {};
	for (auto& effect : windEffects)
		effect->RestoreDefaultSettings();
	TreeWindPatcher::SetUniversalOverride(false, {});
}

void Wind::SetupResources()
{
	SetupGrassWindResources();
	SetupTreeWindResources();
}

bool Wind::IsTreeBendRenderPass(const RE::BSRenderPass* a_pass) const
{
	if (!loaded || !settings.enableTrunkBend || !IsTreeRenderPass(a_pass) ||
		!SupportsTreeBend(*a_pass->shader, GetRenderPassVertexDescriptor(*a_pass)))
		return false;

	return true;
}

// Mirrors the BSLightingShader/BSUtilityShader SetupGeometry vfunc-hook pattern already used by
// TerrainVariation/SnowCover for per-draw mesh-conditional permutation data: this repo has no
// generic Feature::OnRenderPassBegin dispatch, so Wind installs its own hooks (see Hooks below)
// instead of relying on one. Called unconditionally before every qualifying draw, so it always
// clears the descriptor bit and sensitivities first; State::Update() diffs and re-uploads
// PermutationCB automatically whenever it differs from the previous draw's, same as every other
// field in that struct, so no manual upload/restore step is needed here.
void Wind::OnTreeBendRenderPassBegin(RE::BSRenderPass* a_pass)
{
	auto* state = globals::state;
	if (!state)
		return;

	auto& permutationData = state->permutationData;
	permutationData.ExtraShaderDescriptor &= ~kTreeBendDescriptor;

	if (!IsTreeBendRenderPass(a_pass))
		return;

	const auto sensitivities = TreeWindPatcher::GetSensitivities(a_pass->geometry);
	permutationData.ExtraShaderDescriptor |= kTreeBendDescriptor;
	permutationData.TreeBendModelSensitivity = sensitivities.bend;
	permutationData.TreeLeafModelSensitivity = sensitivities.leafAmbient;
	permutationData.TreeWindUpperBendRange = sensitivities.upperBendRange;
	permutationData.TreeWindMaximumDisplacementPercent = sensitivities.maximumDisplacementPercent;
	permutationData.TreeWindTrunkGustInfluence = sensitivities.trunkGustInfluence;
	permutationData.TreeLeafGustInfluence = sensitivities.leafGustInfluence;
	permutationData.TreeTransientWindInfluence = sensitivities.transientWindInfluence;
	permutationData.TreeLeafTransientWindInfluence = sensitivities.leafTransientWindInfluence;
	permutationData.TreeLeafTransientFlutterMaximum = sensitivities.leafTransientFlutterMaximum;
	permutationData.TreeTransientMaximumBendMultiplier = sensitivities.transientMaximumBendMultiplier;
	if (sensitivities.hasBounds) {
		permutationData.TreeWindBoundsBase = sensitivities.boundMinimumZ;
		permutationData.TreeWindBoundsHeight = sensitivities.boundHeight;
		permutationData.TreeWindProbeBase = float4(sensitivities.probeBase.x, sensitivities.probeBase.y, sensitivities.probeBase.z, 0.0f);
		permutationData.TreeWindProbeTop = float4(sensitivities.probeTop.x, sensitivities.probeTop.y, sensitivities.probeTop.z, 0.0f);
	} else {
		permutationData.TreeWindBoundsBase = 0.0f;
		permutationData.TreeWindBoundsHeight = 0.0f;
		permutationData.TreeWindProbeBase = float4();
		permutationData.TreeWindProbeTop = float4();
	}
	UpdateTreeWindSpring();
}

struct Wind::Hooks
{
	struct BSLightingShader_SetupGeometry
	{
		static void thunk(RE::BSShader* This, RE::BSRenderPass* Pass, uint32_t RenderFlags)
		{
			globals::features::wind.OnTreeBendRenderPassBegin(Pass);
			func(This, Pass, RenderFlags);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct BSUtilityShader_SetupGeometry
	{
		static void thunk(RE::BSShader* This, RE::BSRenderPass* Pass, uint32_t RenderFlags)
		{
			globals::features::wind.OnTreeBendRenderPassBegin(Pass);
			func(This, Pass, RenderFlags);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	static void Install()
	{
		stl::write_vfunc<0x6, BSLightingShader_SetupGeometry>(RE::VTABLE_BSLightingShader[0]);
		stl::write_vfunc<0x6, BSUtilityShader_SetupGeometry>(RE::VTABLE_BSUtilityShader[0]);
		logger::info("Wind: Installed hooks - BSLightingShader_SetupGeometry, BSUtilityShader_SetupGeometry");
	}
};

Wind::PerFrameData Wind::GetCommonBufferData() const
{
	return {
		runtimeState.visualizeWindField ? 1u : 0u,
		static_cast<uint32_t>(runtimeState.windFieldDebugView),
		{}
	};
}

void Wind::PostPostLoad()
{
	TreeWindPatcher::LoadAndInstall();
	Hooks::Install();
	SceneTransitionEventHandler::Register();
}

void Wind::DataLoaded()
{
	for (auto& effect : windEffects)
		effect->DataLoaded();
}

void Wind::UpdateWindEffects(float a_frameTime)
{
	for (auto& effect : windEffects)
		effect->Update(a_frameTime);
}

void Wind::OnSceneTransitionReset(bool)
{
	for (auto& effect : windEffects)
		effect->Reset();
}

RE::BSEventNotifyControl Wind::SceneTransitionEventHandler::ProcessEvent(const RE::MenuOpenCloseEvent* a_event, RE::BSTEventSource<RE::MenuOpenCloseEvent>*)
{
	if (a_event && a_event->menuName == RE::LoadingMenu::MENU_NAME && !a_event->opening)
		globals::features::wind.OnSceneTransitionReset(true);
	return RE::BSEventNotifyControl::kContinue;
}
