#include "HairStrands.h"

#include <algorithm>

#include "I18n/I18n.h"

#define I18N_KEY_PREFIX "feature.hair_strands."

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	HairStrands::Settings,
	Enable,
	AutoConvert,
	PlayerOnly,
	MaxActors,
	DensityScale,
	LodStart,
	LodEnd,
	MinStrandFraction,
	MinPixelWidth,
	MaxWidthScale,
	MaxSubdivisions,
	MaxStrandsPerFrame,
	Physics,
	PhysicsDistance,
	SmpGuidance,
	WindStrength,
	BodyCollision);

namespace
{
	constexpr float kMaxLodDistance = 4000.0f;
	constexpr uint kMaxActorsLimit = 32;
	constexpr uint kMaxSubdivisionsLimit = 8;
	constexpr uint kMinStrandBudget = 10000;
	constexpr uint kMaxStrandBudget = 1000000;
	constexpr float kMinDensityScale = 0.05f;
	constexpr float kMinPixelWidthLimit = 0.25f;
	constexpr float kMaxPixelWidthLimit = 4.0f;
	constexpr float kMaxWidthScaleLimit = 10.0f;
	constexpr float kMaxPhysicsDistance = 2000.0f;
	constexpr float kMaxWindStrength = 3.0f;

	struct QualityPreset
	{
		float density;
		uint actors;
		uint strands;
		uint subdivisions;
		float lodEnd;
		float minFraction;
	};
	// Low, Medium, High (the defaults), Ultra.
	constexpr QualityPreset kQualityPresets[] = {
		{ 0.35f, 2, 60000, 2, 350.0f, 0.10f },
		{ 0.6f, 4, 120000, 3, 500.0f, 0.15f },
		{ 1.0f, 6, 200000, 4, 600.0f, 0.15f },
		{ 1.0f, 10, 400000, 6, 900.0f, 0.25f },
	};

	template <class E>
	bool EnumCombo(const char* a_label, E& a_value, std::initializer_list<const char*> a_names)
	{
		std::vector<const char*> names(a_names);
		int index = std::clamp(static_cast<int>(a_value), 0, static_cast<int>(names.size()) - 1);
		if (ImGui::Combo(a_label, &index, names.data(), static_cast<int>(names.size()))) {
			a_value = static_cast<E>(index);
			return true;
		}
		return false;
	}

	std::string HairName(const Strands::HairKey& a_key)
	{
		return a_key.headPart.empty() ? a_key.shape : a_key.headPart;
	}
}

void HairStrands::PostPostLoad()
{
	library.Reload();
	renderer = std::make_unique<Strands::StrandRenderer>(library);
	// Chained after every other Lighting SetupGeometry hook, so the pass's state is final.
	stl::write_vfunc<0x6, Hooks::BSLightingShader_SetupGeometry>(RE::VTABLE_BSLightingShader[0]);
	stl::write_vfunc<0x7, Hooks::BSLightingShader_RestoreGeometry>(RE::VTABLE_BSLightingShader[0]);
	// The depth prepass (Utility shader) must not keep the outline of cards the strands replace.
	stl::write_vfunc<0x6, Hooks::BSUtilityShader_SetupGeometry>(RE::VTABLE_BSUtilityShader[0]);
	stl::write_vfunc<0x7, Hooks::BSUtilityShader_RestoreGeometry>(RE::VTABLE_BSUtilityShader[0]);
	// Effect shaders drawn over the hair (magic effect membranes, dirt and blood) follow the cards.
	stl::write_vfunc<0x6, Hooks::BSEffectShader_SetupGeometry>(RE::VTABLE_BSEffectShader[0]);
	stl::write_vfunc<0x7, Hooks::BSEffectShader_RestoreGeometry>(RE::VTABLE_BSEffectShader[0]);
	hooksInstalled = true;
	logger::info("[HairStrands] Installed hooks; {} authored style entries", library.GetEntryCount());
}

void HairStrands::Hooks::BSLightingShader_SetupGeometry::thunk(RE::BSShader* This, RE::BSRenderPass* Pass, uint32_t RenderFlags)
{
	func(This, Pass, RenderFlags);
	auto& feature = globals::features::hairStrands;
	if (feature.settings.Enable && feature.renderer)
		feature.renderer->OnSetupGeometry(Pass);
}

void HairStrands::Hooks::BSLightingShader_RestoreGeometry::thunk(RE::BSShader* This, RE::BSRenderPass* Pass, uint32_t RenderFlags)
{
	// Before the game restores anything: the hair pass's state is still bound for the strands.
	auto& feature = globals::features::hairStrands;
	if (feature.settings.Enable && feature.renderer)
		feature.renderer->OnRestoreGeometry(Pass);
	func(This, Pass, RenderFlags);
}

void HairStrands::Hooks::BSUtilityShader_SetupGeometry::thunk(RE::BSShader* This, RE::BSRenderPass* Pass, uint32_t RenderFlags)
{
	func(This, Pass, RenderFlags);
	auto& feature = globals::features::hairStrands;
	if (feature.settings.Enable && feature.renderer)
		feature.renderer->OnUtilitySetupGeometry(Pass);
}

void HairStrands::Hooks::BSUtilityShader_RestoreGeometry::thunk(RE::BSShader* This, RE::BSRenderPass* Pass, uint32_t RenderFlags)
{
	auto& feature = globals::features::hairStrands;
	if (feature.settings.Enable && feature.renderer)
		feature.renderer->OnRestoreGeometry(Pass);
	func(This, Pass, RenderFlags);
}

void HairStrands::Hooks::BSEffectShader_SetupGeometry::thunk(RE::BSShader* This, RE::BSRenderPass* Pass, uint32_t RenderFlags)
{
	func(This, Pass, RenderFlags);
	auto& feature = globals::features::hairStrands;
	if (feature.settings.Enable && feature.renderer)
		feature.renderer->OnEffectSetupGeometry(Pass);
}

void HairStrands::Hooks::BSEffectShader_RestoreGeometry::thunk(RE::BSShader* This, RE::BSRenderPass* Pass, uint32_t RenderFlags)
{
	auto& feature = globals::features::hairStrands;
	if (feature.settings.Enable && feature.renderer)
		feature.renderer->OnRestoreGeometry(Pass);
	func(This, Pass, RenderFlags);
}

Strands::RenderSettings HairStrands::MakeRenderSettings() const
{
	Strands::RenderSettings result;
	result.autoConvert = settings.AutoConvert;
	result.playerOnly = settings.PlayerOnly;
	result.maxActors = settings.MaxActors;
	result.densityScale = settings.DensityScale;
	result.lodStart = settings.LodStart;
	result.lodEnd = settings.LodEnd;
	result.minStrandFraction = settings.MinStrandFraction;
	result.minPixelWidth = settings.MinPixelWidth;
	result.maxWidthScale = settings.MaxWidthScale;
	result.maxSubdivisions = settings.MaxSubdivisions;
	result.maxStrandsPerFrame = settings.MaxStrandsPerFrame;
	result.physics = settings.Physics;
	result.physicsDistance = settings.PhysicsDistance;
	result.smpGuidance = settings.SmpGuidance;
	result.windStrength = settings.WindStrength;
	result.collision = settings.BodyCollision;
	return result;
}

void HairStrands::Prepass()
{
	if (renderer && settings.Enable)
		renderer->BeginFrame(MakeRenderSettings());
}

void HairStrands::ClearShaderCache()
{
	if (renderer)
		renderer->ClearShaders();
}

void HairStrands::DrawSettings()
{
	if (ImGui::Checkbox(T(TKEY("enable"), "Enable"), &settings.Enable) && !settings.Enable && renderer)
		renderer->Reset();
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("enable_tooltip"), "Draws hair as strands. Turning it off frees all strand memory."));
	}

	int conversion = settings.AutoConvert ? 1 : 0;
	const char* conversionNames[] = { T(TKEY("conversion_authored"), "Authored styles only"), T(TKEY("conversion_all"), "All hair (automatic)") };
	if (ImGui::Combo(T(TKEY("conversion"), "Convert"), &conversion, conversionNames, 2)) {
		settings.AutoConvert = conversion == 1;
		if (renderer)
			renderer->InvalidateStyles();
	}
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("conversion_tooltip"), "All hair: every hairstyle is converted, using its authored style if it has one.\nAuthored styles only: only hairstyles with a style file entry (or edited in the\nhairstyle editor below) are converted; every other hair keeps its cards."));
	}

	ImGui::SeparatorText(T(TKEY("performance"), "Performance"));
	DrawPerformanceSettings();

	ImGui::SeparatorText(T(TKEY("physics"), "Physics"));
	DrawPhysicsSettings();

	ImGui::SeparatorText(T(TKEY("statistics"), "Statistics"));
	DrawStatistics();

	ImGui::SeparatorText(T(TKEY("editor"), "Hairstyle Editor"));
	DrawEditor();
}

void HairStrands::DrawPerformanceSettings()
{
	const char* qualityNames[] = { T(TKEY("quality_low"), "Low"), T(TKEY("quality_medium"), "Medium"), T(TKEY("quality_high"), "High"), T(TKEY("quality_ultra"), "Ultra") };
	for (int i = 0; i < 4; ++i) {
		if (i > 0)
			ImGui::SameLine();
		if (ImGui::Button(qualityNames[i])) {
			const auto& preset = kQualityPresets[i];
			settings.DensityScale = preset.density;
			settings.MaxActors = preset.actors;
			settings.MaxStrandsPerFrame = preset.strands;
			settings.MaxSubdivisions = preset.subdivisions;
			settings.LodEnd = preset.lodEnd;
			settings.MinStrandFraction = preset.minFraction;
			settings.LodStart = std::min(settings.LodStart, settings.LodEnd);
		}
	}
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("quality_tooltip"), "Sets the options below to a quality level. High is the default."));
	}

	ImGui::SliderFloat(T(TKEY("density_scale"), "Strand Density"), &settings.DensityScale, kMinDensityScale, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("density_scale_tooltip"), "Fraction of each hairstyle's strands drawn up close. Fewer strands are drawn wider,\nso the hair keeps its fullness. The largest cost of this feature."));
	}

	int maxActors = static_cast<int>(settings.MaxActors);
	if (ImGui::SliderInt(T(TKEY("max_actors"), "Characters"), &maxActors, 1, kMaxActorsLimit, "%d", ImGuiSliderFlags_AlwaysClamp))
		settings.MaxActors = static_cast<uint>(maxActors);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("max_actors_tooltip"), "Most characters drawn with strands at once, nearest first (the player always first).\nThe others keep their hair cards."));
	}
	ImGui::Checkbox(T(TKEY("player_only"), "Player Only"), &settings.PlayerOnly);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("player_only_tooltip"), "Draws strands only on the player character."));
	}

	int budget = static_cast<int>(settings.MaxStrandsPerFrame);
	if (ImGui::SliderInt(T(TKEY("max_strands"), "Strand Budget"), &budget, kMinStrandBudget, kMaxStrandBudget, "%d", ImGuiSliderFlags_AlwaysClamp | ImGuiSliderFlags_Logarithmic))
		settings.MaxStrandsPerFrame = static_cast<uint>(budget);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("max_strands_tooltip"), "Most strands drawn in a frame, over all characters. Past it, farther characters\nget fewer (wider) strands."));
	}

	int subdivisions = static_cast<int>(settings.MaxSubdivisions);
	if (ImGui::SliderInt(T(TKEY("max_subdivisions"), "Curve Detail"), &subdivisions, 1, kMaxSubdivisionsLimit, "%d", ImGuiSliderFlags_AlwaysClamp))
		settings.MaxSubdivisions = static_cast<uint>(subdivisions);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("max_subdivisions_tooltip"), "Most render points between two strand control points. Curly and coily hair\nneeds more to keep its curls round up close."));
	}

	if (ImGui::TreeNode(T(TKEY("lod"), "Level of Detail"))) {
		ImGui::SliderFloat(T(TKEY("lod_start"), "Full Detail Distance"), &settings.LodStart, 0.0f, kMaxLodDistance, "%.0f", ImGuiSliderFlags_AlwaysClamp);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("%s", T(TKEY("lod_start_tooltip"), "Distance (in units, about 1.4 cm each) up to which hair is drawn with every strand."));
		}
		ImGui::SliderFloat(T(TKEY("lod_end"), "Strand Distance"), &settings.LodEnd, 0.0f, kMaxLodDistance, "%.0f", ImGuiSliderFlags_AlwaysClamp);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("%s", T(TKEY("lod_end_tooltip"), "Distance past which hair goes back to its cards."));
		}
		settings.LodStart = std::min(settings.LodStart, settings.LodEnd);
		ImGui::SliderFloat(T(TKEY("min_fraction"), "Far Strand Fraction"), &settings.MinStrandFraction, 0.01f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("%s", T(TKEY("min_fraction_tooltip"), "Fraction of the strands still drawn at the strand distance."));
		}
		ImGui::SliderFloat(T(TKEY("min_pixel_width"), "Minimum Width (Pixels)"), &settings.MinPixelWidth, kMinPixelWidthLimit, kMaxPixelWidthLimit, "%.2f", ImGuiSliderFlags_AlwaysClamp);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("%s", T(TKEY("min_pixel_width_tooltip"), "Strands are never drawn thinner than this; farther hair draws correspondingly\nfewer strands. Lower is sharper but flickers more without upscaling or TAA."));
		}
		ImGui::SliderFloat(T(TKEY("max_width_scale"), "Maximum Widening"), &settings.MaxWidthScale, 1.0f, kMaxWidthScaleLimit, "%.1fx", ImGuiSliderFlags_AlwaysClamp);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("%s", T(TKEY("max_width_scale_tooltip"), "How much wider strands may be drawn to make up for the strands the level of\ndetail leaves out."));
		}
		ImGui::TreePop();
	}
}

void HairStrands::DrawPhysicsSettings()
{
	ImGui::Checkbox(T(TKEY("physics_enable"), "Simulate Strands"), &settings.Physics);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("physics_enable_tooltip"), "Strands swing, sway and settle on their own: gravity, inertia, wind and collision\nwith the head and body. Off, they follow the hair's bones (and SMP physics) only.\nEach hairstyle's motion is tuned in the hairstyle editor below."));
	}
	auto _ = Util::DisableGuard(!settings.Physics);
	ImGui::SliderFloat(T(TKEY("physics_distance"), "Physics Distance"), &settings.PhysicsDistance, 0.0f, kMaxPhysicsDistance, "%.0f", ImGuiSliderFlags_AlwaysClamp);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("physics_distance_tooltip"), "Distance (in units, about 1.4 cm each) past which strands are no longer simulated.\nThe motion fades out over the last quarter."));
	}
	ImGui::SliderFloat(T(TKEY("smp_guidance"), "SMP Guidance"), &settings.SmpGuidance, 0.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("smp_guidance_tooltip"), "For hair with its own physics bones (SMP): how much the bones' motion steers the\nstrands. 0: the strands move on their own; 1: they follow the SMP motion and add\ntheir own on top. Hair without such bones is not affected."));
	}
	ImGui::SliderFloat(T(TKEY("wind_strength"), "Wind Strength"), &settings.WindStrength, 0.0f, kMaxWindStrength, "%.2f", ImGuiSliderFlags_AlwaysClamp);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("wind_strength_tooltip"), "How much the weather's wind moves hair outdoors."));
	}
	ImGui::Checkbox(T(TKEY("body_collision"), "Collision"), &settings.BodyCollision);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("body_collision_tooltip"), "Keeps strands out of the head (the character's own head mesh), neck, torso, shoulders\nand upper arms."));
	}
}

void HairStrands::DrawStatistics()
{
	if (!renderer)
		return;
	const auto stats = renderer->GetStats();
	ImGui::Text(T(TKEY("stats_hair"), "Hair in view: %u, converted: %u, drawn as strands: %u"), stats.trackedHair, stats.convertedHair, stats.drawnHair);
	ImGui::Text(T(TKEY("stats_strands"), "Strands drawn: %llu"), static_cast<unsigned long long>(stats.strandsDrawn));
	ImGui::Text(T(TKEY("stats_assets"), "Generated hairstyles: %u (%u generating), GPU memory: %.1f MB"), stats.assets, stats.pendingJobs, stats.gpuBytes / (1024.0 * 1024.0));
	ImGui::Text(T(TKEY("stats_physics"), "Simulated hair: %u, guide strands: %llu"), stats.simulatedHair, static_cast<unsigned long long>(stats.guidesSimulated));
}

bool HairStrands::DrawStyleFields(Strands::StrandStyle& a_style, bool& o_regenerate)
{
	using namespace Strands;
	namespace L = StyleLimits;
	bool changed = false;
	// Generation fields rebuild the strands, so they apply when a slider is released.
	const auto generationEdited = [&]() {
		if (ImGui::IsItemDeactivatedAfterEdit()) {
			changed = true;
			o_regenerate = true;
		}
	};
	const auto tooltip = [](const char* a_text) {
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("%s", a_text);
	};

	changed |= ImGui::Checkbox(T(TKEY("style_enabled"), "Convert This Hair"), &a_style.enabled);
	tooltip(T(TKEY("style_enabled_tooltip"), "Off keeps this hairstyle's cards even when automatic conversion is on."));

	EnumCombo(T(TKEY("style_preset"), "Hair Type"), a_style.preset,
		{ T(TKEY("preset_auto"), "Auto (from the hair's name)"), T(TKEY("preset_straight"), "Straight"), T(TKEY("preset_wavy"), "Wavy"), T(TKEY("preset_curly"), "Curly"),
			T(TKEY("preset_coily"), "Coily (afro-textured)"), T(TKEY("preset_locs"), "Locs, braids and twists") });
	ImGui::SameLine();
	if (ImGui::Button(T(TKEY("style_apply_preset"), "Apply"))) {
		StrandStyle preset = MakePresetStyle(a_style.preset == HairPreset::Auto ? HairPreset::Straight : a_style.preset);
		preset.preset = a_style.preset;
		preset.enabled = a_style.enabled;
		preset.flowAxis = a_style.flowAxis;
		preset.coverageThreshold = a_style.coverageThreshold;
		preset.excludeUV = a_style.excludeUV;
		preset.simulate = a_style.simulate;
		a_style = preset;
		changed = true;
		o_regenerate = true;
	}
	tooltip(T(TKEY("style_preset_tooltip"), "Resets every strand shape and motion field below to the hair type's defaults."));

	if (ImGui::TreeNodeEx(T(TKEY("style_shape"), "Strand Shape"), ImGuiTreeNodeFlags_DefaultOpen)) {
		changed |= ImGui::SliderFloat(T(TKEY("style_root_width"), "Root Width"), &a_style.rootWidth, L::kMinWidth, L::kMaxWidth, "%.3f", ImGuiSliderFlags_AlwaysClamp | ImGuiSliderFlags_Logarithmic);
		tooltip(T(TKEY("style_root_width_tooltip"), "Width of a strand at its root, in units (about 1.4 cm). A drawn strand stands for a\nsmall lock of real hairs, so it is much wider than a single hair."));
		changed |= ImGui::SliderFloat(T(TKEY("style_tip_width"), "Tip Width"), &a_style.tipWidth, L::kMinWidth, L::kMaxWidth, "%.3f", ImGuiSliderFlags_AlwaysClamp | ImGuiSliderFlags_Logarithmic);
		tooltip(T(TKEY("style_tip_width_tooltip"), "Width of a strand at its tip."));
		changed |= ImGui::SliderFloat(T(TKEY("style_wave_amplitude"), "Wave Size"), &a_style.waveAmplitude, 0.0f, L::kMaxWaveAmplitude, "%.2f", ImGuiSliderFlags_AlwaysClamp);
		tooltip(T(TKEY("style_wave_amplitude_tooltip"), "How far waves swing to either side, in units. Strands of one lock wave together."));
		changed |= ImGui::SliderFloat(T(TKEY("style_wave_length"), "Wave Length"), &a_style.waveLength, L::kMinPeriod, L::kMaxPeriod, "%.2f", ImGuiSliderFlags_AlwaysClamp);
		tooltip(T(TKEY("style_wave_length_tooltip"), "Length of one wave along the strand, in units."));
		changed |= ImGui::SliderFloat(T(TKEY("style_curl_radius"), "Curl Radius"), &a_style.curlRadius, 0.0f, L::kMaxCurlRadius, "%.2f", ImGuiSliderFlags_AlwaysClamp);
		tooltip(T(TKEY("style_curl_radius_tooltip"), "Radius of the curl or coil each strand winds around its path, in units. 0 is straight."));
		changed |= ImGui::SliderFloat(T(TKEY("style_curl_length"), "Curl Length"), &a_style.curlLength, L::kMinPeriod, L::kMaxPeriod, "%.2f", ImGuiSliderFlags_AlwaysClamp);
		tooltip(T(TKEY("style_curl_length_tooltip"), "Length of one turn of the curl along the strand, in units. Short turns make tight coils."));
		changed |= ImGui::SliderFloat(T(TKEY("style_curl_start"), "Curl Start"), &a_style.curlStart, 0.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
		tooltip(T(TKEY("style_curl_start_tooltip"), "Fraction of the strand, from the root, over which curls grow to their full radius."));
		changed |= ImGui::SliderFloat(T(TKEY("style_frizz"), "Frizz"), &a_style.frizz, 0.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
		tooltip(T(TKEY("style_frizz_tooltip"), "Random wandering of strands, strongest at the tips, in units."));
		changed |= ImGui::SliderFloat(T(TKEY("style_flyaways"), "Flyaways"), &a_style.flyaways, 0.0f, 0.5f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
		tooltip(T(TKEY("style_flyaways_tooltip"), "Fraction of strands that stray from the style."));
		ImGui::TreePop();
	}

	if (ImGui::TreeNodeEx(T(TKEY("style_motion"), "Motion"), ImGuiTreeNodeFlags_DefaultOpen)) {
		changed |= ImGui::Checkbox(T(TKEY("style_simulate"), "Simulate"), &a_style.simulate);
		tooltip(T(TKEY("style_simulate_tooltip"), "Off, this hairstyle's strands follow its bones only. Very short hair is never simulated."));
		changed |= ImGui::SliderFloat(T(TKEY("style_root_stiffness"), "Root Stiffness"), &a_style.rootStiffness, 0.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
		tooltip(T(TKEY("style_root_stiffness_tooltip"), "How firmly strands hold their styled shape near the roots. 1 is rigid."));
		changed |= ImGui::SliderFloat(T(TKEY("style_tip_stiffness"), "Tip Stiffness"), &a_style.tipStiffness, 0.0f, 1.0f, "%.3f", ImGuiSliderFlags_AlwaysClamp | ImGuiSliderFlags_Logarithmic);
		tooltip(T(TKEY("style_tip_stiffness_tooltip"), "How firmly strands hold their styled shape at the tips. Low values let the ends\nswing freely. Reached 20 units (about 28 cm) from the root: shorter strands stay stiffer."));
		changed |= ImGui::SliderFloat(T(TKEY("style_bend_stiffness"), "Bend Stiffness"), &a_style.bendStiffness, 0.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
		tooltip(T(TKEY("style_bend_stiffness_tooltip"), "How firmly each strand keeps its own curve as it moves. High values keep curls\nand waves springy; low values let strands fold."));
		changed |= ImGui::SliderFloat(T(TKEY("style_damping"), "Damping"), &a_style.damping, 0.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
		tooltip(T(TKEY("style_damping_tooltip"), "How quickly swings die down: the share of the hair's motion relative to the head\nlost per 1/60 s. Low values swing and bounce longer. It does not slow the hair as\nthe head moves, so it never makes hair trail further when running."));
		changed |= ImGui::SliderFloat(T(TKEY("style_gravity"), "Gravity"), &a_style.gravity, 0.0f, L::kMaxGravity, "%.2f", ImGuiSliderFlags_AlwaysClamp);
		tooltip(T(TKEY("style_gravity_tooltip"), "How strongly hair falls when the head tilts or bows. The styled shape is how the\nhair hangs with the head upright, so it does not sag further."));
		changed |= ImGui::SliderFloat(T(TKEY("style_inertia"), "Inertia"), &a_style.inertia, 0.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
		tooltip(T(TKEY("style_inertia_tooltip"), "How much the hair lags behind when the head (or its SMP bones) moves. 0 moves\nrigidly with them; 1 is full inertia: it swings out on turns and streams back at speed."));
		changed |= ImGui::SliderFloat(T(TKEY("style_wind"), "Wind Response"), &a_style.windResponse, 0.0f, L::kMaxWindResponse, "%.2f", ImGuiSliderFlags_AlwaysClamp);
		tooltip(T(TKEY("style_wind_tooltip"), "How much the weather's wind moves this hairstyle."));
		ImGui::TreePop();
	}

	if (ImGui::TreeNodeEx(T(TKEY("style_generation"), "Conversion"), ImGuiTreeNodeFlags_DefaultOpen)) {
		ImGui::TextDisabled("%s", T(TKEY("style_generation_note"), "These rebuild the strands when a slider is released."));
		ImGui::SliderFloat(T(TKEY("style_density"), "Density"), &a_style.density, L::kMinDensity, L::kMaxDensity, "%.1f", ImGuiSliderFlags_AlwaysClamp);
		generationEdited();
		tooltip(T(TKEY("style_density_tooltip"), "Strands per unit of card width at the roots."));
		ImGui::SliderFloat(T(TKEY("style_segment_length"), "Segment Length"), &a_style.segmentLength, L::kMinSegmentLength, L::kMaxSegmentLength, "%.2f", ImGuiSliderFlags_AlwaysClamp);
		generationEdited();
		tooltip(T(TKEY("style_segment_length_tooltip"), "Spacing of the points that follow the hair's bones and shape. Shorter follows\ntight bends better and costs more."));
		ImGui::SliderFloat(T(TKEY("style_length_scale"), "Length"), &a_style.lengthScale, 0.05f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
		generationEdited();
		tooltip(T(TKEY("style_length_scale_tooltip"), "Fraction of the card length the strands keep."));
		ImGui::SliderFloat(T(TKEY("style_coverage"), "Texture Coverage"), &a_style.coverageThreshold, 0.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
		generationEdited();
		tooltip(T(TKEY("style_coverage_tooltip"), "Strands only grow where the hair texture's alpha is at least this, so they end where\nthe painted hair ends and skip the transparent parts of the cards. Lower keeps\nfainter wisps; 0 ignores the texture and fills the whole cards."));
		ImGui::SliderFloat(T(TKEY("style_volume"), "Volume"), &a_style.volume, 0.0f, L::kMaxVolume, "%.2f", ImGuiSliderFlags_AlwaysClamp);
		generationEdited();
		tooltip(T(TKEY("style_volume_tooltip"), "How far strands lift off the cards towards their tips, in units."));
		ImGui::SliderFloat(T(TKEY("style_layer_jitter"), "Layering"), &a_style.layerJitter, 0.0f, L::kMaxVolume, "%.2f", ImGuiSliderFlags_AlwaysClamp);
		generationEdited();
		tooltip(T(TKEY("style_layer_jitter_tooltip"), "Random lift of whole strands off the cards, in units, for depth."));
		ImGui::SliderFloat(T(TKEY("style_clump_strength"), "Clumping"), &a_style.clumpStrength, 0.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
		generationEdited();
		tooltip(T(TKEY("style_clump_strength_tooltip"), "How tightly neighbouring strands gather into locks towards their tips.\nHigh values also make curls spiral together as ringlets."));
		ImGui::SliderFloat(T(TKEY("style_clump_size"), "Clump Size"), &a_style.clumpSize, 0.1f, L::kMaxClumpSize, "%.2f", ImGuiSliderFlags_AlwaysClamp);
		generationEdited();
		tooltip(T(TKEY("style_clump_size_tooltip"), "Radius of a lock at the roots, in units."));
		ImGui::SliderFloat(T(TKEY("style_clump_twist"), "Twist"), &a_style.clumpTwist, -L::kMaxTwist, L::kMaxTwist, "%.2f", ImGuiSliderFlags_AlwaysClamp);
		generationEdited();
		tooltip(T(TKEY("style_clump_twist_tooltip"), "Turns per unit that strands twist around their lock (twists, locs, braids)."));

		if (EnumCombo(T(TKEY("style_seeding"), "Strand Roots"), a_style.seeding, { T(TKEY("seeding_auto"), "Auto"), T(TKEY("seeding_roots"), "Card edges"), T(TKEY("seeding_area"), "Whole surface (short hair)") }))
			changed = o_regenerate = true;
		tooltip(T(TKEY("style_seeding_tooltip"), "Card edges: strands grow from the edge each card's hair flows out of.\nWhole surface: short strands scattered over the mesh, for buzz cuts and fuzz.\nAuto picks the surface when the traced strands come out very short."));
		ImGui::SliderFloat(T(TKEY("style_short_length"), "Short Hair Length"), &a_style.shortLength, 0.1f, L::kMaxShortLength, "%.2f", ImGuiSliderFlags_AlwaysClamp);
		generationEdited();
		tooltip(T(TKEY("style_short_length_tooltip"), "Strand length, in units, when roots cover the whole surface."));
		if (EnumCombo(T(TKEY("style_flow"), "Flow Direction"), a_style.flowAxis, { T(TKEY("flow_auto"), "Auto"), "V", "-V", "U", "-U" }))
			changed = o_regenerate = true;
		tooltip(T(TKEY("style_flow_tooltip"), "Which texture direction runs from root to tip. Auto follows the hair's flow map\n(the one Hair Specular uses) where it has one, elsewhere the way the strands are\npainted in each part of the texture, turned to run away from the head.\nChange it if strands grow across the cards or from the tips."));

		int seed = static_cast<int>(a_style.seed);
		if (ImGui::InputInt(T(TKEY("style_seed"), "Random Seed"), &seed))
			a_style.seed = static_cast<uint32_t>(std::max(seed, 0));
		generationEdited();
		tooltip(T(TKEY("style_seed_tooltip"), "Another seed gives another, equally likely, placement of the strands."));

		ImGui::TextUnformatted(T(TKEY("style_exclude"), "Keep as cards (UV rectangles):"));
		tooltip(T(TKEY("style_exclude_tooltip"), "Triangles whose texture coordinates fall inside a rectangle keep their cards and\nget no strands: scalp caps, hairlines, ribbons, beads. Values: min U, min V, max U, max V."));
		for (size_t i = 0; i < a_style.excludeUV.size(); ++i) {
			ImGui::PushID(static_cast<int>(i));
			auto& rect = a_style.excludeUV[i];
			float values[4] = { rect.minU, rect.minV, rect.maxU, rect.maxV };
			if (ImGui::DragFloat4("##rect", values, 0.005f, -2.0f, 3.0f, "%.3f"))
				rect = { values[0], values[1], values[2], values[3] };
			generationEdited();
			ImGui::SameLine();
			if (ImGui::Button(T(TKEY("style_exclude_remove"), "Remove"))) {
				a_style.excludeUV.erase(a_style.excludeUV.begin() + i);
				changed = o_regenerate = true;
				ImGui::PopID();
				break;
			}
			ImGui::PopID();
		}
		if (a_style.excludeUV.size() < L::kMaxExcludeRects && ImGui::Button(T(TKEY("style_exclude_add"), "Add Rectangle"))) {
			a_style.excludeUV.push_back({ 0.0f, 0.0f, 0.1f, 0.1f });
			changed = o_regenerate = true;
		}
		ImGui::TreePop();
	}

	if (changed)
		Sanitize(a_style);
	return changed;
}

void HairStrands::DrawEditor()
{
	if (!renderer)
		return;

	ImGui::TextWrapped("%s", T(TKEY("editor_help"), "Pick a hairstyle in view to tune how it converts. Changes show at once on every character wearing it; Save writes them to the style files, where they replace automatic conversion for that hairstyle."));
	if (ImGui::Button(T(TKEY("reload_styles"), "Reload Style Files"))) {
		library.Reload();
		editorKey.reset();
	}
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text(T(TKEY("reload_styles_tooltip"), "Style files are read from:\n%s"), Strands::StyleLibrary::GetDirectory().string().c_str());
	}
	ImGui::SameLine();
	ImGui::TextDisabled(T(TKEY("style_entries"), "%u authored styles"), static_cast<uint>(library.GetEntryCount()));

	const auto hair = renderer->GetInstances();
	if (hair.empty()) {
		ImGui::TextDisabled("%s", T(TKEY("editor_no_hair"), "No hair in view."));
		return;
	}

	const Strands::InstanceView* selected = nullptr;
	if (editorKey) {
		for (const auto& view : hair) {
			if (view.key == *editorKey)
				selected = &view;
		}
		// After Revert or Delete, pick up the style the hair resolves to now.
		if (editorReload && selected && !selected->edited) {
			editorStyle = selected->style;
			editorReload = false;
		}
	}
	if (ImGui::BeginTable("##HairStrandsHair", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
		ImGui::TableSetupColumn(T(TKEY("column_hair"), "Hair"));
		ImGui::TableSetupColumn(T(TKEY("column_style"), "Style"));
		ImGui::TableSetupColumn(T(TKEY("column_status"), "Status"));
		ImGui::TableSetupColumn(T(TKEY("column_strands"), "Strands"));
		ImGui::TableHeadersRow();
		for (const auto& view : hair) {
			ImGui::TableNextRow();
			ImGui::TableNextColumn();
			const bool isSelected = editorKey && *editorKey == view.key;
			std::string label = HairName(view.key);
			if (view.isPlayer)
				label += std::format(" ({})", T(TKEY("player"), "player"));
			label += "##" + view.key.ToString();
			if (ImGui::Selectable(label.c_str(), isSelected, ImGuiSelectableFlags_SpanAllColumns)) {
				editorKey = view.key;
				editorStyle = renderer->GetStyleOverride(view.key).value_or(view.style);
				editorReload = false;
				selected = &view;
			}
			ImGui::TableNextColumn();
			if (view.edited)
				ImGui::TextUnformatted(T(TKEY("style_source_edited"), "Edited (unsaved)"));
			else if (view.authored)
				ImGui::TextUnformatted(view.source.c_str());
			else
				ImGui::TextUnformatted(T(TKEY("style_source_auto"), "Automatic"));
			ImGui::TableNextColumn();
			using Status = Strands::InstanceView::Status;
			switch (view.status) {
			case Status::Cards:
				ImGui::TextUnformatted(view.disabledByStyle ? T(TKEY("status_disabled"), "Cards (style off)") : T(TKEY("status_cards"), "Cards"));
				break;
			case Status::Waiting:
			case Status::Generating:
				ImGui::TextUnformatted(T(TKEY("status_generating"), "Generating"));
				break;
			case Status::Ready:
				ImGui::TextUnformatted(view.activeStrands ? T(TKEY("status_drawn"), "Strands") : T(TKEY("status_ready"), "Ready (not drawn)"));
				break;
			case Status::Failed:
				ImGui::TextUnformatted(T(TKEY("status_failed"), "Failed"));
				if (auto _tt = Util::HoverTooltipWrapper())
					ImGui::Text("%s", view.error.c_str());
				break;
			}
			ImGui::TableNextColumn();
			if (view.strands)
				ImGui::Text("%u / %u x %u", view.activeStrands, view.strands, view.pointsPerStrand);
		}
		ImGui::EndTable();
	}

	if (!editorKey)
		return;

	ImGui::SeparatorText(HairName(*editorKey).c_str());
	ImGui::TextDisabled("%s", editorKey->ToString().c_str());
	if (!selected)
		ImGui::TextDisabled("%s", T(TKEY("editor_not_in_view"), "This hairstyle is not in view; edits apply when it is."));

	bool regenerate = false;
	if (DrawStyleFields(editorStyle, regenerate))
		renderer->SetStyleOverride(*editorKey, editorStyle);

	ImGui::Spacing();
	if (ImGui::Button(T(TKEY("style_save"), "Save"))) {
		if (library.SaveUserStyle(*editorKey, editorStyle))
			renderer->SetStyleOverride(*editorKey, std::nullopt);
	}
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text(T(TKEY("style_save_tooltip"), "Writes this style for this exact hairstyle to\n%s"), Strands::StyleLibrary::GetUserFile().string().c_str());
	}
	ImGui::SameLine();
	if (ImGui::Button(T(TKEY("style_revert"), "Revert"))) {
		renderer->SetStyleOverride(*editorKey, std::nullopt);
		editorReload = true;
	}
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("style_revert_tooltip"), "Discards unsaved changes."));
	}
	ImGui::SameLine();
	if (ImGui::Button(T(TKEY("style_delete"), "Delete Saved Style"))) {
		library.RemoveUserStyle(*editorKey);
		renderer->SetStyleOverride(*editorKey, std::nullopt);
		editorReload = true;
	}
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("style_delete_tooltip"), "Removes this hairstyle's entry from the in-game editor's style file. Styles\nshipped in other style files are not touched."));
	}
}

void HairStrands::SaveSettings(json& o_json)
{
	o_json = settings;
}

void HairStrands::LoadSettings(json& o_json)
{
	settings = o_json;
	settings.MaxActors = std::clamp(settings.MaxActors, 1u, kMaxActorsLimit);
	settings.DensityScale = std::clamp(settings.DensityScale, kMinDensityScale, 1.0f);
	settings.LodEnd = std::clamp(settings.LodEnd, 0.0f, kMaxLodDistance);
	settings.LodStart = std::clamp(settings.LodStart, 0.0f, settings.LodEnd);
	settings.MinStrandFraction = std::clamp(settings.MinStrandFraction, 0.01f, 1.0f);
	settings.MinPixelWidth = std::clamp(settings.MinPixelWidth, kMinPixelWidthLimit, kMaxPixelWidthLimit);
	settings.MaxWidthScale = std::clamp(settings.MaxWidthScale, 1.0f, kMaxWidthScaleLimit);
	settings.MaxSubdivisions = std::clamp(settings.MaxSubdivisions, 1u, kMaxSubdivisionsLimit);
	settings.MaxStrandsPerFrame = std::clamp(settings.MaxStrandsPerFrame, kMinStrandBudget, kMaxStrandBudget);
	settings.PhysicsDistance = std::clamp(settings.PhysicsDistance, 0.0f, kMaxPhysicsDistance);
	settings.SmpGuidance = std::clamp(settings.SmpGuidance, 0.0f, 1.0f);
	settings.WindStrength = std::clamp(settings.WindStrength, 0.0f, kMaxWindStrength);
	if (renderer) {
		renderer->ForgetInstances();
		if (!settings.Enable)
			renderer->Reset();
	}
}

void HairStrands::RestoreDefaultSettings()
{
	settings = {};
	if (renderer)
		renderer->ForgetInstances();
}

#undef I18N_KEY_PREFIX
