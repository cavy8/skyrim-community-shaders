#include "SubsurfaceScattering.h"

#include "../I18n/I18n.h"
#include "Deferred.h"
#include "ShaderCache.h"
#include "State.h"
#include "Utils/MathUtils.h"

#define I18N_KEY_PREFIX "feature.sss."

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(SubsurfaceScattering::DiffusionProfile,
	BlurRadius, Thickness, Strength, Falloff)

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	SubsurfaceScattering::Settings,
	EnableCharacterLighting,
	CharacterLightingHumanoidOnly,
	SSMode,
	ScatterMode,
	BaseProfile,
	HumanProfile,
	BurleySamples,
	MeanFreePathBase,
	MeanFreePathHuman,
	EnableFoliageProfile,
	FoliageProfile,
	MeanFreePathFoliage,
	EnableTranslucentProfile,
	TranslucentProfile,
	MeanFreePathTranslucent)

void SubsurfaceScattering::DrawSettings()
{
	ImGui::Checkbox(T(TKEY("enable_character_lighting"), "Enable Character Lighting"), (bool*)&settings.EnableCharacterLighting);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("enable_character_lighting_tooltip"), "Vanilla feature."));
	}
	if (settings.EnableCharacterLighting) {
		ImGui::Checkbox(T(TKEY("character_lighting_humanoid_only"), "Humanoid Races Only"), (bool*)&settings.CharacterLightingHumanoidOnly);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("%s", T(TKEY("character_lighting_humanoid_only_tooltip"), "Applies character lighting only to human, elf and beast races. Creatures such as Falmer and draugr stop glowing."));
		}
	}

	ImGui::RadioButton(T(TKEY("separable_sss"), "Separable SSS"), &settings.SSMode, 0);
	ImGui::SameLine();
	ImGui::RadioButton(T(TKEY("burley"), "Burley"), &settings.SSMode, 1);

	if (settings.SSMode == 0) {
		ImGui::Spacing();
		ImGui::Text("%s", T(TKEY("albedo_handling"), "Albedo Handling"));
		ImGui::RadioButton(T(TKEY("pre_scatter"), "Pre-scatter"), &settings.ScatterMode, kPreScatter);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("%s", T(TKEY("pre_scatter_tooltip"), "Blur the lit color directly. Fastest, but blurs albedo texture detail along with lighting."));
		}
		ImGui::SameLine();
		ImGui::RadioButton(T(TKEY("post_scatter"), "Post-scatter"), &settings.ScatterMode, kPostScatter);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("%s", T(TKEY("post_scatter_tooltip"), "Divide out albedo, blur the irradiance, multiply albedo back. Preserves texture detail."));
		}
		ImGui::SameLine();
		ImGui::RadioButton(T(TKEY("pre_and_post_scatter"), "Pre and Post"), &settings.ScatterMode, kPreAndPostScatter);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("%s", T(TKEY("pre_and_post_scatter_tooltip"), "Split albedo across the blur using sqrt(albedo) on each side. A physically motivated middle ground."));
		}
	}

	if (settings.SSMode == 0) {
		ImGui::SeparatorText(T(TKEY("base_profile"), "Base Profile"));
		ImGui::PushID("BaseProfile");
		ImGui::SliderFloat(T(TKEY("blur_radius"), "Blur Radius"), &settings.BaseProfile.BlurRadius, 0, 3, "%.2f");
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("%s", T(TKEY("blur_radius_tooltip"), "Blur radius."));
		}

		ImGui::SliderFloat(T(TKEY("thickness"), "Thickness"), &settings.BaseProfile.Thickness, 0, 3, "%.2f");
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("%s", T(TKEY("thickness_tooltip"), "Blur radius relative to depth."));
		}

		updateKernels |= ImGui::ColorEdit3(T(TKEY("strength"), "Strength"), (float*)&settings.BaseProfile.Strength);
		updateKernels |= ImGui::ColorEdit3(T(TKEY("falloff"), "Falloff"), (float*)&settings.BaseProfile.Falloff);
		ImGui::PopID();

		ImGui::SeparatorText(T(TKEY("human_profile"), "Human Profile"));
		ImGui::PushID("HumanProfile");
		ImGui::SliderFloat(T(TKEY("blur_radius"), "Blur Radius"), &settings.HumanProfile.BlurRadius, 0, 3, "%.2f");
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("%s", T(TKEY("blur_radius_tooltip"), "Blur radius."));
		}

		ImGui::SliderFloat(T(TKEY("thickness"), "Thickness"), &settings.HumanProfile.Thickness, 0, 3, "%.2f");
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("%s", T(TKEY("thickness_tooltip"), "Blur radius relative to depth."));
		}

		updateKernels |= ImGui::ColorEdit3(T(TKEY("strength"), "Strength"), (float*)&settings.HumanProfile.Strength);
		updateKernels |= ImGui::ColorEdit3(T(TKEY("falloff"), "Falloff"), (float*)&settings.HumanProfile.Falloff);
		ImGui::PopID();
	} else if (settings.SSMode == 1) {
		ImGui::SliderInt(T(TKEY("burley_samples"), "Burley Samples"), (int*)&settings.BurleySamples, 1, 64, "%d", ImGuiSliderFlags_AlwaysClamp);
		ImGui::SeparatorText(T(TKEY("base_profile"), "Base Profile"));
		ImGui::PushID("BaseProfile");
		ImGui::ColorEdit3(T(TKEY("mean_free_path_color"), "Mean Free Path Color"), (float*)&settings.MeanFreePathBase);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("%s", T(TKEY("mean_free_path_color_tooltip"), "Controls how far light goes into the subsurface in the red, green, and blue channel. It is scaled by the Mean Free Path Distance."));
		}
		ImGui::SliderFloat(T(TKEY("mean_free_path_distance"), "Mean Free Path Distance"), &settings.MeanFreePathBase.w, 0.01f, 10.0f, "%.2f");
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("%s", T(TKEY("mean_free_path_distance_tooltip"), "Controls the distance that Mean Free Path Color goes into subsurface."));
		}
		ImGui::PopID();

		ImGui::SeparatorText(T(TKEY("human_profile"), "Human Profile"));
		ImGui::PushID("HumanProfile");
		ImGui::ColorEdit3(T(TKEY("mean_free_path_color"), "Mean Free Path Color"), (float*)&settings.MeanFreePathHuman);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("%s", T(TKEY("mean_free_path_color_tooltip"), "Controls how far light goes into the subsurface in the red, green, and blue channel. It is scaled by the Mean Free Path Distance."));
		}
		ImGui::SliderFloat(T(TKEY("mean_free_path_distance"), "Mean Free Path Distance"), &settings.MeanFreePathHuman.w, 0.01f, 10.0f, "%.2f");
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("%s", T(TKEY("mean_free_path_distance_tooltip"), "Controls the distance that Mean Free Path Color goes into subsurface."));
		}
		ImGui::PopID();
	}

	ImGui::Spacing();
	ImGui::Checkbox(T(TKEY("enable_foliage_profile"), "Foliage Profile"), (bool*)&settings.EnableFoliageProfile);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("enable_foliage_profile_tooltip"), "Softens PBR foliage that has subsurface data. Costly on dense leaves."));
	}
	ImGui::Checkbox(T(TKEY("enable_translucent_profile"), "Translucent Profile"), (bool*)&settings.EnableTranslucentProfile);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("enable_translucent_profile_tooltip"), "Diffuses light inside thick PBR subsurface materials such as wax, marble or ice."));
	}

	auto drawExtraProfile = [&](const char* a_label, bool a_enabled, DiffusionProfile& a_profile, float4& a_meanFreePath) {
		if (!a_enabled || !ImGui::TreeNodeEx(a_label))
			return;
		if (settings.SSMode == 0) {
			ImGui::SliderFloat(T(TKEY("blur_radius"), "Blur Radius"), &a_profile.BlurRadius, 0, 3, "%.2f");
			ImGui::SliderFloat(T(TKEY("thickness"), "Thickness"), &a_profile.Thickness, 0, 3, "%.2f");
			updateKernels |= ImGui::ColorEdit3(T(TKEY("strength"), "Strength"), (float*)&a_profile.Strength);
			updateKernels |= ImGui::ColorEdit3(T(TKEY("falloff"), "Falloff"), (float*)&a_profile.Falloff);
		} else {
			ImGui::ColorEdit3(T(TKEY("mean_free_path_color"), "Mean Free Path Color"), (float*)&a_meanFreePath);
			ImGui::SliderFloat(T(TKEY("mean_free_path_distance"), "Mean Free Path Distance"), &a_meanFreePath.w, 0.01f, 10.0f, "%.2f");
		}
		ImGui::TreePop();
	};
	drawExtraProfile(T(TKEY("foliage_profile"), "Foliage Profile Settings"), settings.EnableFoliageProfile, settings.FoliageProfile, settings.MeanFreePathFoliage);
	drawExtraProfile(T(TKEY("translucent_profile"), "Translucent Profile Settings"), settings.EnableTranslucentProfile, settings.TranslucentProfile, settings.MeanFreePathTranslucent);
}

float3 SubsurfaceScattering::Gaussian(DiffusionProfile& a_profile, float variance, float r)
{
	/**
     * We use a falloff to modulate the shape of the profile. Big falloffs
     * spreads the shape making it wider, while small falloffs make it
     * narrower.
     */
	float falloff[3] = { a_profile.Falloff.x, a_profile.Falloff.y, a_profile.Falloff.z };
	float g[3];
	for (int i = 0; i < 3; i++) {
		float rr = r / (0.001f + falloff[i]);
		g[i] = exp((-(rr * rr)) / (2.0f * variance)) / (2.0f * 3.14f * variance);
	}
	return float3(g[0], g[1], g[2]);
}

float3 SubsurfaceScattering::Profile(DiffusionProfile& a_profile, float r)
{
	/**
     * We used the red channel of the original skin profile defined in
     * [d'Eon07] for all three channels. We noticed it can be used for green
     * and blue channels (scaled using the falloff parameter) without
     * introducing noticeable differences and allowing for total control over
     * the profile. For example, it allows to create blue SSS gradients, which
     * could be useful in case of rendering blue creatures.
     */
	return  // 0.233f * gaussian(0.0064f, r) + /* We consider this one to be directly bounced light, accounted by the strength parameter (see @STRENGTH) */
		0.100f * Gaussian(a_profile, 0.0484f, r) +
		0.118f * Gaussian(a_profile, 0.187f, r) +
		0.113f * Gaussian(a_profile, 0.567f, r) +
		0.358f * Gaussian(a_profile, 1.99f, r) +
		0.078f * Gaussian(a_profile, 7.41f, r);
}

void SubsurfaceScattering::CalculateKernel(DiffusionProfile& a_profile, Kernel& kernel)
{
	uint nSamples = SSSS_N_SAMPLES;

	const float RANGE = nSamples > 20 ? 3.0f : 2.0f;
	const float EXPONENT = 2.0f;

	// Calculate the offsets:
	float step = 2.0f * RANGE / (nSamples - 1);
	for (uint i = 0; i < nSamples; i++) {
		float o = -RANGE + float(i) * step;
		float sign = o < 0.0f ? -1.0f : 1.0f;
		kernel.Sample[i].w = RANGE * sign * abs(pow(o, EXPONENT)) / pow(RANGE, EXPONENT);
	}

	// Calculate the weights:
	for (uint i = 0; i < nSamples; i++) {
		float w0 = i > 0 ? abs(kernel.Sample[i].w - kernel.Sample[i - 1].w) : 0.0f;
		float w1 = i < nSamples - 1 ? abs(kernel.Sample[i].w - kernel.Sample[i + 1].w) : 0.0f;
		float area = (w0 + w1) / 2.0f;
		float3 t = area * Profile(a_profile, kernel.Sample[i].w);
		kernel.Sample[i].x = t.x;
		kernel.Sample[i].y = t.y;
		kernel.Sample[i].z = t.z;
	}

	// We want the offset 0.0 to come first:
	float4 t = kernel.Sample[nSamples / 2];
	for (uint i = nSamples / 2; i > 0; i--)
		kernel.Sample[i] = kernel.Sample[i - 1];
	kernel.Sample[0] = t;

	// Calculate the sum of the weights, we will need to normalize them below:
	float3 sum = float3(0.0f, 0.0f, 0.0f);
	for (uint i = 0; i < nSamples; i++)
		sum += float3(kernel.Sample[i]);

	// Normalize the weights:
	for (uint i = 0; i < nSamples; i++) {
		kernel.Sample[i].x /= sum.x;
		kernel.Sample[i].y /= sum.y;
		kernel.Sample[i].z /= sum.z;
	}

	// Tweak them using the desired strength. The first one is:
	//     lerp(1.0, kernel[0].rgb, strength)
	kernel.Sample[0].x = (1.0f - a_profile.Strength.x) * 1.0f + a_profile.Strength.x * kernel.Sample[0].x;
	kernel.Sample[0].y = (1.0f - a_profile.Strength.y) * 1.0f + a_profile.Strength.y * kernel.Sample[0].y;
	kernel.Sample[0].z = (1.0f - a_profile.Strength.z) * 1.0f + a_profile.Strength.z * kernel.Sample[0].z;

	// The others:
	//     lerp(0.0, kernel[0].rgb, strength)
	for (uint i = 1; i < nSamples; i++) {
		kernel.Sample[i].x *= a_profile.Strength.x;
		kernel.Sample[i].y *= a_profile.Strength.y;
		kernel.Sample[i].z *= a_profile.Strength.z;
	}
}

void SubsurfaceScattering::DrawSSS()
{
	if (!validMaterials)
		return;

	ZoneScoped;
	TracyD3D11Zone(globals::state->tracyCtx, "Subsurface Scattering");

	validMaterials = false;

	auto* prepassShader = GetComputeShaderPrepass();
	auto* firstBlurShader = settings.SSMode == 0 ? GetComputeShaderHorizontalBlur() : GetComputeShaderBurley();
	auto* secondBlurShader = settings.SSMode == 0 ? GetComputeShaderVerticalBlur() : firstBlurShader;
	if (!prepassShader || !firstBlurShader || !secondBlurShader)
		return;

	auto dispatchCount = Util::GetScreenDispatchCount();

	{
		auto cameraData = globals::game::shadowState->GetRuntimeData().cameraData.getEye();

		blurCBData.SSSS_FOVY = atan(1.0f / cameraData.projMat.m[0][0]) * 2.0f * (180.0f / 3.14159265359f);

		auto profileData = [](const DiffusionProfile& a_profile, bool a_enabled) {
			return float4{ a_profile.BlurRadius, a_profile.Thickness, a_enabled ? 1.0f : 0.0f, 0.0f };
		};
		blurCBData.Profiles[kProfileBase] = profileData(settings.BaseProfile, true);
		blurCBData.Profiles[kProfileHuman] = profileData(settings.HumanProfile, true);
		blurCBData.Profiles[kProfileFoliage] = profileData(settings.FoliageProfile, settings.EnableFoliageProfile);
		blurCBData.Profiles[kProfileTranslucent] = profileData(settings.TranslucentProfile, settings.EnableTranslucentProfile);

		blurCBData.BurleySamples = settings.BurleySamples;
		// Burley always does full albedo removal/reapply; scatter mode only applies to Separable SSS.
		blurCBData.ScatterMode = (settings.SSMode == 0) ? (uint)std::clamp(settings.ScatterMode, (int)kPreScatter, (int)kPreAndPostScatter) : (uint)kPostScatter;

		blurCBData.MeanFreePath[kProfileBase] = settings.MeanFreePathBase;
		blurCBData.MeanFreePath[kProfileHuman] = settings.MeanFreePathHuman;
		blurCBData.MeanFreePath[kProfileFoliage] = settings.MeanFreePathFoliage;
		blurCBData.MeanFreePath[kProfileTranslucent] = settings.MeanFreePathTranslucent;

		blurCB->Update(blurCBData);
	}

	auto renderer = globals::game::renderer;
	auto context = globals::d3d::context;

	{
		ID3D11Buffer* buffer[1] = { blurCB->CB() };
		context->CSSetConstantBuffers(1, 1, buffer);
		context->CSSetSamplers(0, 1, &globals::deferred->pointSampler);

		// Consume the same active forward color target that SSGI composites into.
		auto main = renderer->GetRuntimeData().renderTargets[globals::deferred->forwardRenderTargets[0]];

		auto mask = renderer->GetRuntimeData().renderTargets[MASKS];
		auto albedo = renderer->GetRuntimeData().renderTargets[ALBEDO];
		auto normal = renderer->GetRuntimeData().renderTargets[NORMALROUGHNESS];

		ID3D11ShaderResourceView* views[5];
		views[0] = main.SRV;
		views[1] = Util::GetCurrentSceneDepthSRV(true);
		views[2] = mask.SRV;
		views[3] = albedo.SRV;
		views[4] = normal.SRV;

		context->CSSetShaderResources(0, ARRAYSIZE(views), views);

		// Pre-pass: remove albedo from diffuse, write to diffuseNoAlbedoTex
		{
			TracyD3D11Zone(globals::state->tracyCtx, "Subsurface Scattering - Prepass");

			ID3D11UnorderedAccessView* uav = diffuseNoAlbedoTex->uav.get();
			context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);

			context->CSSetShader(prepassShader, nullptr, 0);

			context->Dispatch(dispatchCount.x, dispatchCount.y, 1);

			uav = nullptr;
			context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
		}

		// Swap color input to pre-processed texture
		views[0] = diffuseNoAlbedoTex->srv.get();
		context->CSSetShaderResources(0, 1, views);

		if (settings.SSMode == 0)
			EnsureBlurHorizontalTemp();

		if (settings.SSMode == 0 && blurHorizontalTemp) {
			// Horizontal pass: diffuseNoAlbedoTex -> blurHorizontalTemp
			{
				TracyD3D11Zone(globals::state->tracyCtx, "Subsurface Scattering - Horizontal");

				ID3D11UnorderedAccessView* uav = blurHorizontalTemp->uav.get();
				context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);

				context->CSSetShader(firstBlurShader, nullptr, 0);

				globals::profiler->BeginPass("SubsurfaceScattering::HorizontalBlur");
				context->Dispatch(dispatchCount.x, dispatchCount.y, 1);
				globals::profiler->EndPass();

				uav = nullptr;
				context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
			}

			// Vertical pass: blurHorizontalTemp -> main
			{
				TracyD3D11Zone(globals::state->tracyCtx, "Subsurface Scattering - Vertical");

				views[0] = blurHorizontalTemp->srv.get();
				context->CSSetShaderResources(0, 1, views);

				ID3D11UnorderedAccessView* uavs[1] = { main.UAV };
				context->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);

				context->CSSetShader(secondBlurShader, nullptr, 0);

				globals::profiler->BeginPass("SubsurfaceScattering::VerticalBlur");
				context->Dispatch(dispatchCount.x, dispatchCount.y, 1);
				globals::profiler->EndPass();
			}
		} else if (settings.SSMode == 1) {
			// Burley pass: diffuseNoAlbedoTex -> main (SSS pixels only)
			{
				TracyD3D11Zone(globals::state->tracyCtx, "Subsurface Scattering - Burley");

				ID3D11UnorderedAccessView* uavs[1] = { main.UAV };
				context->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);

				context->CSSetShader(firstBlurShader, nullptr, 0);

				globals::profiler->BeginPass("SubsurfaceScattering::Burley");
				context->Dispatch(dispatchCount.x, dispatchCount.y, 1);
				globals::profiler->EndPass();
			}
		}
	}

	ID3D11Buffer* buffer = nullptr;
	context->CSSetConstantBuffers(1, 1, &buffer);

	ID3D11SamplerState* nullSampler = nullptr;
	context->CSSetSamplers(0, 1, &nullSampler);

	ID3D11ShaderResourceView* views[5]{ nullptr, nullptr, nullptr, nullptr, nullptr };
	context->CSSetShaderResources(0, ARRAYSIZE(views), views);

	ID3D11UnorderedAccessView* uavs[1]{ nullptr };
	context->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);

	ID3D11ComputeShader* shader = nullptr;
	context->CSSetShader(shader, nullptr, 0);
}

void SubsurfaceScattering::SetupResources()
{
	{
		blurCB = new ConstantBuffer(ConstantBufferDesc<BlurCB>(), "SSS::BlurCB");
	}

	auto renderer = globals::game::renderer;

	{
		auto main = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN];

		D3D11_TEXTURE2D_DESC texDesc{};
		main.texture->GetDesc(&texDesc);

		texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;

		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
		main.SRV->GetDesc(&srvDesc);

		D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
		main.UAV->GetDesc(&uavDesc);

		diffuseNoAlbedoTex = new Texture2D(texDesc, "SSS::DiffuseNoAlbedo");
		diffuseNoAlbedoTex->CreateSRV(srvDesc);
		diffuseNoAlbedoTex->CreateUAV(uavDesc);
	}

	if (settings.SSMode == 0)
		EnsureBlurHorizontalTemp();
}

void SubsurfaceScattering::EnsureBlurHorizontalTemp()
{
	if (blurHorizontalTemp || !diffuseNoAlbedoTex)
		return;

	D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
	diffuseNoAlbedoTex->srv->GetDesc(&srvDesc);
	D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
	diffuseNoAlbedoTex->uav->GetDesc(&uavDesc);

	blurHorizontalTemp = new Texture2D(diffuseNoAlbedoTex->desc, "SSS::BlurHorizontalTemp");
	blurHorizontalTemp->CreateSRV(srvDesc);
	blurHorizontalTemp->CreateUAV(uavDesc);
}

void SubsurfaceScattering::Reset()
{
	auto shaderManager = globals::game::smState;
	auto shaderCache = globals::shaderCache;
	shaderManager->characterLightEnabled = shaderCache->IsEnabled() ? settings.EnableCharacterLighting : true;

	if (updateKernels) {
		updateKernels = false;
		CalculateKernel(settings.BaseProfile, blurCBData.Kernels[kProfileBase]);
		CalculateKernel(settings.HumanProfile, blurCBData.Kernels[kProfileHuman]);
		CalculateKernel(settings.FoliageProfile, blurCBData.Kernels[kProfileFoliage]);
		CalculateKernel(settings.TranslucentProfile, blurCBData.Kernels[kProfileTranslucent]);
	}
}

void SubsurfaceScattering::OnSubsurfaceMaterial(bool a_foliage)
{
	if (a_foliage ? settings.EnableFoliageProfile : settings.EnableTranslucentProfile)
		validMaterials = true;
}

void SubsurfaceScattering::RestoreDefaultSettings()
{
	settings = {};
	updateKernels = true;
}

void SubsurfaceScattering::LoadSettings(json& o_json)
{
	settings = o_json;
	settings.SSMode = std::clamp(settings.SSMode, 0, 1);
	settings.ScatterMode = std::clamp(settings.ScatterMode, (int)kPreScatter, (int)kPreAndPostScatter);
	settings.BurleySamples = std::clamp(settings.BurleySamples, 1u, 64u);
	const Settings defaults{};
	settings.MeanFreePathBase.w = Util::ClampFinite(settings.MeanFreePathBase.w, defaults.MeanFreePathBase.w, 0.01f, 10.0f);
	settings.MeanFreePathHuman.w = Util::ClampFinite(settings.MeanFreePathHuman.w, defaults.MeanFreePathHuman.w, 0.01f, 10.0f);
	settings.MeanFreePathFoliage.w = Util::ClampFinite(settings.MeanFreePathFoliage.w, defaults.MeanFreePathFoliage.w, 0.01f, 10.0f);
	settings.MeanFreePathTranslucent.w = Util::ClampFinite(settings.MeanFreePathTranslucent.w, defaults.MeanFreePathTranslucent.w, 0.01f, 10.0f);
	updateKernels = true;
}

void SubsurfaceScattering::SaveSettings(json& o_json)
{
	o_json = settings;
}

void SubsurfaceScattering::ClearShaderCache()
{
	if (prepassSS) {
		prepassSS->Release();
		prepassSS = nullptr;
	}
	if (prepassBurleySS) {
		prepassBurleySS->Release();
		prepassBurleySS = nullptr;
	}
	if (horizontalSSBlur) {
		horizontalSSBlur->Release();
		horizontalSSBlur = nullptr;
	}
	if (verticalSSBlur) {
		verticalSSBlur->Release();
		verticalSSBlur = nullptr;
	}
	if (burleySS) {
		burleySS->Release();
		burleySS = nullptr;
	}
}

ID3D11ComputeShader* SubsurfaceScattering::GetComputeShaderPrepass()
{
	if (settings.SSMode == 1) {
		if (!prepassBurleySS)
			prepassBurleySS = (ID3D11ComputeShader*)Util::CompileShader(L"Data\\Shaders\\SubsurfaceScattering\\DiffuseExtractionCS.hlsl", { { "BURLEY", "" } }, "cs_5_0");
		return prepassBurleySS;
	}
	if (!prepassSS) {
		logger::debug("Compiling prepassSS");
		prepassSS = (ID3D11ComputeShader*)Util::CompileShader(L"Data\\Shaders\\SubsurfaceScattering\\DiffuseExtractionCS.hlsl", {}, "cs_5_0");
	}
	return prepassSS;
}

ID3D11ComputeShader* SubsurfaceScattering::GetComputeShaderHorizontalBlur()
{
	if (!horizontalSSBlur) {
		logger::debug("Compiling horizontalSSBlur");
		horizontalSSBlur = (ID3D11ComputeShader*)Util::CompileShader(L"Data\\Shaders\\SubsurfaceScattering\\SeparableSSSCS.hlsl", { { "HORIZONTAL", "" } }, "cs_5_0");
	}
	return horizontalSSBlur;
}

ID3D11ComputeShader* SubsurfaceScattering::GetComputeShaderVerticalBlur()
{
	if (!verticalSSBlur) {
		logger::debug("Compiling verticalSSBlur");
		verticalSSBlur = (ID3D11ComputeShader*)Util::CompileShader(L"Data\\Shaders\\SubsurfaceScattering\\SeparableSSSCS.hlsl", {}, "cs_5_0");
	}
	return verticalSSBlur;
}

ID3D11ComputeShader* SubsurfaceScattering::GetComputeShaderBurley()
{
	if (!burleySS) {
		logger::debug("Compiling burleySS");
		burleySS = (ID3D11ComputeShader*)Util::CompileShader(L"Data\\Shaders\\SubsurfaceScattering\\SeparableSSSCS.hlsl", { { "BURLEY", "" } }, "cs_5_0");
	}
	return burleySS;
}

void SubsurfaceScattering::DataLoaded()
{
	auto form = RE::TESForm::LookupByEditorID("IsBeastRace");
	isBeastRaceKeyword = form ? form->As<RE::BGSKeyword>() : nullptr;
	if (!isBeastRaceKeyword) {
		logger::warn("IsBeastRace keyword is unavailable; using the default SSS skin classification");
	}
}

void SubsurfaceScattering::PostPostLoad()
{
	Hooks::Install();
}

bool SubsurfaceScattering::SuppressCharacterLight(RE::BSGeometry* a_geometry) const
{
	if (!loaded || !settings.EnableCharacterLighting || !settings.CharacterLightingHumanoidOnly || !a_geometry)
		return false;
	auto* ref = a_geometry->GetUserData();
	auto* actor = ref ? ref->As<RE::Actor>() : nullptr;
	auto* race = actor ? actor->GetRace() : nullptr;
	return race && !race->data.flags.any(RE::RACE_DATA::Flag::kFaceGenHead);
}

RE::BSShaderProperty::RenderPassArray* SubsurfaceScattering::Hooks::BSLightingShaderProperty_GetRenderPasses::thunk(RE::BSLightingShaderProperty* a_property, RE::BSGeometry* a_geometry, std::uint32_t a_renderFlags, RE::BSShaderAccumulator* a_accumulator)
{
	constexpr auto kCharacterLighting = RE::BSShaderProperty::EShaderPropertyFlag::kCharacterLighting;
	if (!a_property->flags.any(kCharacterLighting) || !globals::features::subsurfaceScattering.SuppressCharacterLight(a_geometry))
		return func(a_property, a_geometry, a_renderFlags, a_accumulator);

	a_property->flags.reset(kCharacterLighting);
	auto* renderPasses = func(a_property, a_geometry, a_renderFlags, a_accumulator);
	a_property->flags.set(kCharacterLighting);
	return renderPasses;
}

void SubsurfaceScattering::BSLightingShader_SetupSkin(RE::BSRenderPass* a_pass)
{
	auto deferred = globals::deferred;
	auto state = globals::state;

	if (deferred->deferredPass) {
		if (a_pass->shaderProperty->flags.any(RE::BSShaderProperty::EShaderPropertyFlag::kFace, RE::BSShaderProperty::EShaderPropertyFlag::kFaceGenRGBTint)) {
			bool isBeastRace = true;

			auto geometry = a_pass->geometry;
			if (isBeastRaceKeyword)
				if (auto userData = geometry->GetUserData())
					if (auto actor = userData->As<RE::Actor>())
						if (auto race = actor->GetRace())
							isBeastRace = race->HasKeyword(isBeastRaceKeyword);

			validMaterials = true;

			if (isBeastRace)
				state->permutationData.ExtraShaderDescriptor |= (uint)State::ExtraShaderDescriptors::IsBeastRace;
			else
				state->permutationData.ExtraShaderDescriptor &= ~(uint)State::ExtraShaderDescriptors::IsBeastRace;
		}
	}
}

void SubsurfaceScattering::Hooks::BSLightingShader_SetupGeometry::thunk(RE::BSShader* This, RE::BSRenderPass* Pass, uint32_t RenderFlags)
{
	globals::features::subsurfaceScattering.BSLightingShader_SetupSkin(Pass);
	func(This, Pass, RenderFlags);
}

#undef I18N_KEY_PREFIX
