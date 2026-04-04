#include "NeckSeamFix.h"

#include "Deferred.h"
#include "ShaderCache.h"
#include "State.h"

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	NeckSeamFix::Settings,
	SearchRadius,
	DepthThreshold,
	BlendStrength)

// =============================================================================
// DrawSettings
// =============================================================================

void NeckSeamFix::DrawSettings()
{
	if (ImGui::TreeNodeEx("Settings", ImGuiTreeNodeFlags_DefaultOpen)) {
		ImGui::SliderFloat("Search Radius", &settings.SearchRadius, 1.0f, 4.0f, "%.1f px");
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text(
				"Maximum pixel radius searched around each candidate gap pixel.\n"
				"Higher values catch wider seams but may produce false positives.");
		}

		ImGui::SliderFloat("Depth Threshold", &settings.DepthThreshold, 0.001f, 0.05f, "%.4f");
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text(
				"Maximum linearised depth difference (in game units) between the\n"
				"gap pixel and its skin neighbours.  Lower = more conservative.");
		}

		ImGui::SliderFloat("Blend Strength", &settings.BlendStrength, 0.0f, 1.0f, "%.2f");
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text(
				"How completely the gap pixel is overwritten with the averaged\n"
				"neighbour colour.  1.0 = full replacement.");
		}

		ImGui::Spacing();
		ImGui::Spacing();
		ImGui::TreePop();
	}
}

// =============================================================================
// SetupResources
// =============================================================================

void NeckSeamFix::SetupResources()
{
	neckSeamCB = new ConstantBuffer(ConstantBufferDesc<NeckSeamCB>());
}

// =============================================================================
// DrawSeamFix  (main render call)
// =============================================================================

void NeckSeamFix::DrawSeamFix()
{
	ZoneScoped;
	TracyD3D11Zone(globals::state->tracyCtx, "Neck Seam Fix");

	auto renderer = globals::game::renderer;
	auto context = globals::d3d::context;

	// Upload constant buffer
	{
		NeckSeamCB cbData{};
		cbData.SearchRadius = settings.SearchRadius;
		cbData.DepthThreshold = settings.DepthThreshold;
		cbData.BlendStrength = settings.BlendStrength;
		cbData.pad = 0.0f;
		neckSeamCB->Update(cbData);
	}

	// Bind constant buffer at slot 1  (slot 0 is reserved for PerFrame by convention)
	{
		ID3D11Buffer* cb[1] = { neckSeamCB->CB() };
		context->CSSetConstantBuffers(1, 1, cb);
	}

	auto main = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN];
	auto masks = renderer->GetRuntimeData().renderTargets[MASKS];

	// SRV inputs
	{
		ID3D11ShaderResourceView* srvs[2]{
			Util::GetCurrentSceneDepthSRV(true),  // t0 — depth
			masks.SRV,                            // t1 — MASKS (skin flag in .x)
		};
		context->CSSetShaderResources(0, ARRAYSIZE(srvs), srvs);
	}

	// UAV output  (read-modify-write on the main colour target)
	{
		ID3D11UnorderedAccessView* uav[1] = { main.UAV };
		context->CSSetUnorderedAccessViews(0, 1, uav, nullptr);
	}

	auto shader = GetComputeShader();
	context->CSSetShader(shader, nullptr, 0);

	auto dispatchCount = Util::GetScreenDispatchCount();
	context->Dispatch(dispatchCount.x, dispatchCount.y, 1);

	// --- Unbind ---
	ID3D11Buffer* nullCB[1] = { nullptr };
	context->CSSetConstantBuffers(1, 1, nullCB);

	ID3D11ShaderResourceView* nullSRVs[2]{ nullptr, nullptr };
	context->CSSetShaderResources(0, ARRAYSIZE(nullSRVs), nullSRVs);

	ID3D11UnorderedAccessView* nullUAV[1] = { nullptr };
	context->CSSetUnorderedAccessViews(0, 1, nullUAV, nullptr);

	context->CSSetShader(nullptr, nullptr, 0);
}

// =============================================================================
// Settings serialisation
// =============================================================================

void NeckSeamFix::LoadSettings(json& o_json)
{
	settings = o_json;
}

void NeckSeamFix::SaveSettings(json& o_json)
{
	o_json = settings;
}

void NeckSeamFix::RestoreDefaultSettings()
{
	settings = {};
}

// =============================================================================
// Shader cache
// =============================================================================

void NeckSeamFix::ClearShaderCache()
{
	if (neckSeamCS) {
		neckSeamCS->Release();
		neckSeamCS = nullptr;
	}
}

ID3D11ComputeShader* NeckSeamFix::GetComputeShader()
{
	if (!neckSeamCS) {
		logger::debug("Compiling NeckSeamFixCS");
		neckSeamCS = static_cast<ID3D11ComputeShader*>(
			Util::CompileShader(L"Data\\Shaders\\NeckSeamFix\\NeckSeamFixCS.hlsl", {}, "cs_5_0"));
	}
	return neckSeamCS;
}
