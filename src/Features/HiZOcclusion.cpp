#include "HiZOcclusion.h"

#include "Features/TerrainBlending.h"
#include "Menu.h"
#include "Menu/Fonts.h"
#include "ShaderCache.h"
#include "State.h"
#include "Util.h"
#include "Utils/D3D.h"
#include "Utils/Game.h"
#include "Utils/UI.h"

#include <RE/B/BSLightingShaderProperty.h>
#include <RE/B/BSShaderProperty.h>
#include <RE/N/NiBound.h>
#include <RE/P/PlayerCharacter.h>

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	HiZOcclusion::Settings,
	enableHiZViewer,
	hizViewerMip,
	hizViewerScale,
	enableHiZCulling,
	conservativeBias,
	showCullingStats,
	debugMode,
	enableBoundsViewer,
	boundsMaxObjects,
	showVisTestPassed,
	showVisInsideBounds,
	showVisInvalidRadius,
	showCulledFrustum,
	showCulledNoEarlyOut,
	cullNoEarlyOut,
	consecutiveOccludedThreshold,
	shadowSweepDistance,
	guardAngle,
	motionMarginFrames,
	cullRenderMode)

#define I18N_KEY_PREFIX "feature.hiz_occlusion."

namespace
{
	float Angle(const DirectX::XMFLOAT3& a, const DirectX::XMFLOAT3& b)
	{
		const float d = std::clamp(a.x * b.x + a.y * b.y + a.z * b.z, -1.0f, 1.0f);
		return DirectX::XMConvertToDegrees(std::acos(d));
	}

	float Distance(const DirectX::XMFLOAT3& a, const DirectX::XMFLOAT3& b)
	{
		const float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
		return std::sqrt(dx * dx + dy * dy + dz * dz);
	}

	float GuardBandUV(float projScale, float guardDegrees)
	{
		if (projScale <= 0.0f)
			return 0.0f;
		const float halfFov = std::atan(1.0f / projScale);
		const float guarded = std::min(halfFov + DirectX::XMConvertToRadians(guardDegrees), DirectX::XMConvertToRadians(89.0f));
		return 0.5f * projScale * (std::tan(guarded) - 1.0f / projScale);
	}
}

HiZOcclusion::~HiZOcclusion()
{
	// Release Hi-Z pyramid resources
	if (hiZSRV) {
		hiZSRV->Release();
		hiZSRV = nullptr;
	}
	for (auto* v : hiZSRVsPerMip) {
		if (v)
			v->Release();
	}
	hiZSRVsPerMip.clear();
	for (auto* u : hiZUAVs) {
		if (u)
			u->Release();
	}
	hiZUAVs.clear();
	if (hiZTexture) {
		hiZTexture->Release();
		hiZTexture = nullptr;
	}

	// Release compute shaders
	if (hiZBuildLevel0CS) {
		hiZBuildLevel0CS->Release();
		hiZBuildLevel0CS = nullptr;
	}
	if (hiZDownsampleCS) {
		hiZDownsampleCS->Release();
		hiZDownsampleCS = nullptr;
	}
	if (hiZTestCS) {
		hiZTestCS->Release();
		hiZTestCS = nullptr;
	}
	if (hiZTestCSDebug) {
		hiZTestCSDebug->Release();
		hiZTestCSDebug = nullptr;
	}

	// Release GPU culling resources
	if (geometryBoundsSRV) {
		geometryBoundsSRV->Release();
		geometryBoundsSRV = nullptr;
	}
	if (geometryBoundsBuffer) {
		geometryBoundsBuffer->Release();
		geometryBoundsBuffer = nullptr;
	}
	if (visibilityResultsUAV) {
		visibilityResultsUAV->Release();
		visibilityResultsUAV = nullptr;
	}
	if (visibilityResultsBuffer) {
		visibilityResultsBuffer->Release();
		visibilityResultsBuffer = nullptr;
	}
	if (hiZTestParamsBuffer) {
		hiZTestParamsBuffer->Release();
		hiZTestParamsBuffer = nullptr;
	}
	if (hiZSampler) {
		hiZSampler->Release();
		hiZSampler = nullptr;
	}

	// Release async readback staging buffers
	for (int i = 0; i < AsyncReadbackState::BUFFER_COUNT; ++i) {
		if (readbackState.stagingBuffers[i]) {
			readbackState.stagingBuffers[i]->Release();
			readbackState.stagingBuffers[i] = nullptr;
		}
		if (readbackState.completionQueries[i]) {
			readbackState.completionQueries[i]->Release();
			readbackState.completionQueries[i] = nullptr;
		}
	}

	// Release debug buffers
	ReleaseDebugBuffer();

	// Release bounds overlay resources
	ReleaseBoundsOverlayResources();

	// Release readback buffers
	if (visibilityReadbackBuffer) {
		visibilityReadbackBuffer->Release();
		visibilityReadbackBuffer = nullptr;
	}

	// Release GPU timestamp queries
	for (uint32_t i = 0; i < GPU_TIMING_BUFFER_COUNT; ++i) {
		if (gpuTimingQueries[i].disjointQuery) {
			gpuTimingQueries[i].disjointQuery->Release();
			gpuTimingQueries[i].disjointQuery = nullptr;
		}
		if (gpuTimingQueries[i].beginTimestamp) {
			gpuTimingQueries[i].beginTimestamp->Release();
			gpuTimingQueries[i].beginTimestamp = nullptr;
		}
		if (gpuTimingQueries[i].endTimestamp) {
			gpuTimingQueries[i].endTimestamp->Release();
			gpuTimingQueries[i].endTimestamp = nullptr;
		}
		gpuTimingQueries[i].pending = false;
	}
}

bool HiZOcclusion::SetupBoundsOverlayResources(uint32_t width, uint32_t height)
{
	auto device = globals::d3d::device;
	if (!device || width == 0 || height == 0)
		return false;

	// Release old
	ReleaseBoundsOverlayResources();

	D3D11_TEXTURE2D_DESC td{};
	td.Width = width;
	td.Height = height;
	td.MipLevels = 1;
	td.ArraySize = 1;
	td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	td.SampleDesc.Count = 1;
	td.Usage = D3D11_USAGE_DEFAULT;
	td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;

	if (FAILED(device->CreateTexture2D(&td, nullptr, &boundsOverlayTex)))
		return false;
	Util::SetResourceName(boundsOverlayTex, "HiZOcclusion::BoundsOverlay");

	D3D11_SHADER_RESOURCE_VIEW_DESC srd{};
	srd.Format = td.Format;
	srd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
	srd.Texture2D.MostDetailedMip = 0;
	srd.Texture2D.MipLevels = 1;
	if (FAILED(device->CreateShaderResourceView(boundsOverlayTex, &srd, &boundsOverlaySRV)))
		return false;
	Util::SetResourceName(boundsOverlaySRV, "HiZOcclusion::BoundsOverlay SRV");

	D3D11_UNORDERED_ACCESS_VIEW_DESC uavd{};
	uavd.Format = td.Format;
	uavd.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
	uavd.Texture2D.MipSlice = 0;
	if (FAILED(device->CreateUnorderedAccessView(boundsOverlayTex, &uavd, &boundsOverlayUAV)))
		return false;
	Util::SetResourceName(boundsOverlayUAV, "HiZOcclusion::BoundsOverlay UAV");

	boundsOverlayW = width;
	boundsOverlayH = height;
	return true;
}

void HiZOcclusion::ReleaseBoundsOverlayResources()
{
	if (boundsOverlayUAV) {
		boundsOverlayUAV->Release();
		boundsOverlayUAV = nullptr;
	}
	if (boundsOverlaySRV) {
		boundsOverlaySRV->Release();
		boundsOverlaySRV = nullptr;
	}
	if (boundsOverlayTex) {
		boundsOverlayTex->Release();
		boundsOverlayTex = nullptr;
	}
	boundsOverlayW = boundsOverlayH = 0;
}

void HiZOcclusion::ClearBoundsOverlay()
{
	if (!boundsOverlayUAV)
		return;
	auto ctx = globals::d3d::context;
	static const float clearColor[4] = { 0.f, 0.f, 0.f, 0.f };
	ctx->ClearUnorderedAccessViewFloat(boundsOverlayUAV, clearColor);
}

void HiZOcclusion::DrawSettings()
{
	const auto& theme = Menu::GetSingleton()->GetTheme();

	// Main toggle at the top - always visible
	ImGui::Checkbox(T(TKEY("enable_culling"), "Enable Occlusion Culling"), &settings.enableHiZCulling);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("enable_culling_tooltip"), "Skip rendering objects hidden behind other geometry.\nImproves performance by reducing unnecessary draw calls."));
	}

	if (!settings.enableHiZCulling) {
		ImGui::TextDisabled("%s", T(TKEY("enable_culling_hint"), "Enable occlusion culling to access settings."));
		return;
	}

	ImGui::Separator();

	if (ImGui::BeginTabBar("##HiZTabs", ImGuiTabBarFlags_None)) {
		// General Settings Tab
		if (MenuFonts::BeginTabItemWithFont(T(TKEY("tab_settings"), "Settings"), Menu::FontRole::Subheading)) {
			if (ImGui::BeginChild("##HiZSettingsFrame", { 0, 0 }, ImGuiChildFlags_Borders)) {
				// Culling Options
				ImGui::SeparatorText(T(TKEY("culling_options"), "Culling Options"));

				ImGui::SliderFloat(T(TKEY("conservative_bias"), "Conservative Bias"), &settings.conservativeBias, 0.000f, 0.050f, "%.3f");
				if (auto _tt = Util::HoverTooltipWrapper()) {
					ImGui::Text("%s", T(TKEY("conservative_bias_tooltip"),
										  "How aggressively objects are culled, as a fraction of the occluder's distance.\n"
										  "Lower = more culling, better performance, but may cause pop-in.\n"
										  "Higher = safer, fewer artifacts, but less performance gain."));
				}

				// Render mode culling toggles
				ImGui::Text("%s", T(TKEY("render_mode_culling"), "Render Mode Culling:"));
				if (ImGui::BeginTable("##HiZRenderModes", 3, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg)) {
					ImGui::TableSetupColumn(T(TKEY("render_mode_column_pass"), "Pass"));
					ImGui::TableSetupColumn(T(TKEY("render_mode_column_test"), "Test"));
					ImGui::TableSetupColumn(T(TKEY("render_mode_column_calls"), "Calls"));
					ImGui::TableHeadersRow();
					for (uint32_t i = 0; i < static_cast<uint32_t>(settings.cullRenderMode.size()); i++) {
						const uint8_t pass = GetPassKind(i);
						const uint32_t calls = stats.renderModeCalls[i].load();
						if (!pass && !calls)
							continue;

						ImGui::PushID(static_cast<int>(i));
						ImGui::TableNextRow();
						ImGui::TableNextColumn();
						{
							auto _disabled = Util::DisableGuard(!pass);
							const auto label = std::format("{} ({})", GetRenderModeName(i), i);
							bool enabled = pass && settings.cullRenderMode[i];
							if (ImGui::Checkbox(label.c_str(), &enabled))
								settings.cullRenderMode[i] = enabled;
						}
						ImGui::TableNextColumn();
						if (pass == kCameraPass)
							ImGui::TextUnformatted(T(TKEY("render_mode_test_camera"), "Camera occlusion"));
						else if (pass == kSunShadowPass)
							ImGui::TextUnformatted(T(TKEY("render_mode_test_shadow"), "Shadow + caster occlusion"));
						else
							ImGui::TextDisabled("%s", T(TKEY("render_mode_test_unsupported"), "Not cullable"));
						ImGui::TableNextColumn();
						ImGui::Text("%u", calls);
						ImGui::PopID();
					}
					ImGui::EndTable();
				}

				ImGui::SliderFloat(T(TKEY("shadow_sweep_distance"), "Shadow Reach"), &settings.shadowSweepDistance, 0.0f, 16384.0f, "%.0f");
				if (auto _tt = Util::HoverTooltipWrapper()) {
					ImGui::Text("%s", T(TKEY("shadow_sweep_distance_tooltip"),
										  "How far a caster's shadow is followed along the sun direction.\n"
										  "A caster is only dropped from the shadow map when it and its shadow are hidden. 0 disables shadow culling."));
				}

				ImGui::SliderFloat(T(TKEY("guard_angle"), "Turn Guard (degrees)"), &settings.guardAngle, 0.0f, 45.0f, "%.1f");
				if (auto _tt = Util::HoverTooltipWrapper()) {
					ImGui::Text("%s", T(TKEY("guard_angle_tooltip"),
										  "Shadows just outside the view are kept so turning does not reveal missing shadows.\n"
										  "Shadow culling also resets when the camera turns further than this before new results arrive."));
				}

				ImGui::SliderFloat(T(TKEY("motion_margin_frames"), "Motion Margin (frames)"), &settings.motionMarginFrames, 0.0f, 10.0f, "%.1f");
				if (auto _tt = Util::HoverTooltipWrapper()) {
					ImGui::Text("%s", T(TKEY("motion_margin_frames_tooltip"),
										  "Grows every bound by this many frames of camera movement to hide readback latency.\n"
										  "Higher = less pop-in while moving, less culling."));
				}

				int threshold = static_cast<int>(settings.consecutiveOccludedThreshold);
				if (ImGui::SliderInt(T(TKEY("consecutive_threshold"), "Consecutive Occluded Threshold"), &threshold, 1, 100)) {
					settings.consecutiveOccludedThreshold = static_cast<uint32_t>(threshold);
				}
				if (auto _tt = Util::HoverTooltipWrapper()) {
					ImGui::Text("%s", T(TKEY("consecutive_threshold_tooltip"),
										  "Number of consecutive occluded test results required before an object is hidden.\n"
										  "1 = hide after first failed test (aggressive, may cause pop-in). Higher values are safer but slower to react."));
				}

				// Performance Statistics
				ImGui::Spacing();
				ImGui::SeparatorText(T(TKEY("performance"), "Performance"));

				// Early culling (best savings - prevents all CPU work)
				if (stats.earlyCulledCount > 0) {
					ImGui::TextColored(theme.StatusPalette.SuccessColor, "%s %u", T(TKEY("objects_culled_early"), "Objects culled early:"), stats.earlyCulledCount.load());
					if (auto _tt = Util::HoverTooltipWrapper()) {
						ImGui::Text("%s", T(TKEY("objects_culled_early_tooltip"), "Culled during scene traversal - maximum CPU savings."));
					}
				}

				// Status
				ImGui::Spacing();
				ImGui::Text("%s %s", T(TKEY("status"), "Status:"), HiZStatusToString(status));
				if (!statusMessage.empty()) {
					ImGui::TextColored(theme.StatusPalette.Warning, "  %s", statusMessage.c_str());
				}

				ImGui::Text("%s", T(TKEY("accumulator_registrations"), "Accumulator registrations:"));
				ImGui::Indent();
				ImGui::Text("%s %u", T(TKEY("main_pass"), "Main pass:"), stats.accumRegisterCalls.load());
				ImGui::Unindent();
			}
			ImGui::EndChild();
			ImGui::EndTabItem();
		}

		// Developer/Debug Tab
		if (MenuFonts::BeginTabItemWithFont(T(TKEY("tab_developer"), "Developer"), Menu::FontRole::Subheading)) {
			if (ImGui::BeginChild("##HiZDevFrame", { 0, 0 }, ImGuiChildFlags_Borders)) {
				// Debug Visualization
				ImGui::SeparatorText(T(TKEY("visualization"), "Visualization"));

				ImGui::Checkbox(T(TKEY("debug_mode"), "Debug mode"), &settings.debugMode);
				if (auto _tt = Util::HoverTooltipWrapper()) {
					ImGui::Text("%s", T(TKEY("debug_mode_tooltip"), "Enables debug mode."));
				}

				ImGui::Checkbox(T(TKEY("show_pyramid_viewer"), "Show Depth Pyramid Viewer"), &settings.enableHiZViewer);
				if (auto _tt = Util::HoverTooltipWrapper()) {
					ImGui::Text("%s", T(TKEY("show_pyramid_viewer_tooltip"), "Opens a window showing the hierarchical depth buffer."));
				}

				if (settings.enableHiZViewer) {
					ImGui::Indent();
					uint32_t maxMip = hiZMipCount > 0 ? (hiZMipCount - 1) : 0;
					if (settings.hizViewerMip > maxMip)
						settings.hizViewerMip = maxMip;
					ImGui::SliderInt(T(TKEY("mip_level"), "Mip Level"), reinterpret_cast<int*>(&settings.hizViewerMip), 0, static_cast<int>(maxMip));
					ImGui::SliderFloat(T(TKEY("display_scale"), "Display Scale"), &settings.hizViewerScale, 0.1f, 4.0f, "%.1fx");
					ImGui::Unindent();
				}

				ImGui::Spacing();
				ImGui::Checkbox(T(TKEY("show_bounds_overlay"), "Show Bounds Overlay"), &settings.enableBoundsViewer);
				if (auto _tt = Util::HoverTooltipWrapper()) {
					ImGui::Text("%s", T(TKEY("show_bounds_overlay_tooltip"), "Draw colored outlines around objects showing their culling status."));
				}

				if (settings.enableBoundsViewer) {
					ImGui::Indent();
					ImGui::SliderInt(T(TKEY("max_objects"), "Max Objects"), reinterpret_cast<int*>(&settings.boundsMaxObjects), 8, 2048);

					ImGui::Text("%s", T(TKEY("show"), "Show:"));
					ImGui::Checkbox(T(TKEY("visible_green"), "Visible (green)"), &settings.showVisTestPassed);
					ImGui::SameLine();
					ImGui::Text(": %u", stats.visTestPassed);

					ImGui::Checkbox(T(TKEY("undecided_teal"), "Undecided (teal)"), &settings.showVisInsideBounds);
					ImGui::SameLine();
					ImGui::Text(": %u", stats.visInsideBounds);

					ImGui::Checkbox(T(TKEY("invalid_bounds_cyan"), "Invalid bounds (cyan)"), &settings.showVisInvalidRadius);
					ImGui::SameLine();
					ImGui::Text(": %u", stats.visInvalidRadius);

					ImGui::Spacing();
					ImGui::Checkbox(T(TKEY("outside_magenta"), "Outside view (magenta)"), &settings.showCulledFrustum);
					ImGui::SameLine();
					ImGui::Text(": %u", stats.culledFrustum);

					ImGui::Spacing();
					ImGui::Text("%s", T(TKEY("cull"), "Cull:"));
					ImGui::Checkbox(T(TKEY("occluded_red"), "Occluded (red)"), &settings.cullNoEarlyOut);
					ImGui::SameLine();
					ImGui::Text(": %u", stats.culledNoEarlyOut);
					ImGui::Unindent();
				}

				// Detailed Statistics
				ImGui::Spacing();
				ImGui::SeparatorText(T(TKEY("statistics"), "Statistics"));

				ImGui::Text("%s %u", T(TKEY("frame"), "Frame:"), globals::state ? globals::state->frameCount : 0);
				ImGui::Text("%s %u", T(TKEY("geometry_tested"), "Geometry tested:"), stats.geometryListSize);
				ImGui::Text("%s %u", T(TKEY("total_tests"), "Total tests:"), stats.totalTested);
				ImGui::Text("%s %zu", T(TKEY("occluded_set_size"), "Occluded set size:"), cameraOcclusion.occluded.size());
				ImGui::Text("%s %zu", T(TKEY("shadow_occluded_set_size"), "Shadow casters culled:"), shadowOcclusion.occluded.size());

				if (stats.staleFrameCount > 0) {
					ImGui::TextColored(theme.StatusPalette.Warning, "%s %u", T(TKEY("results_stale"), "Results stale (frames):"), stats.staleFrameCount);
				} else {
					ImGui::Text("%s", T(TKEY("results_fresh"), "Results: Fresh"));
				}

				// Timing
				ImGui::Spacing();
				ImGui::SeparatorText(T(TKEY("timing"), "Timing (microseconds)"));
				ImGui::Text("%s %.1f", T(TKEY("timing_gpu_culling"), "GPU culling:"), stats.gpuCullingTimeMs * 1000.0f);
				ImGui::Text("%s %.1f", T(TKEY("timing_copy_results"), "Copy results:"), stats.copyTimeMs * 1000.0f);
				ImGui::Text("%s %.1f", T(TKEY("timing_map_buffer"), "Map buffer:"), stats.mapTimeMs * 1000.0f);
				ImGui::Text("%s %.1f", T(TKEY("timing_copy_data"), "Copy data:"), stats.copyDataTimeMs * 1000.0f);
				ImGui::Text("%s %.1f", T(TKEY("timing_unmap"), "Unmap:"), stats.unmapTimeMs * 1000.0f);
				ImGui::Text("%s %.1f", T(TKEY("timing_total_readback"), "Total readback:"), stats.readbackTimeMs * 1000.0f);
			}
			ImGui::EndChild();
			ImGui::EndTabItem();
		}

		ImGui::EndTabBar();
	}

	// Separate viewer window
	if (settings.enableHiZViewer) {
		if (ImGui::Begin(T(TKEY("pyramid_window"), "Hi-Z Depth Pyramid###HiZDepthPyramid"), &settings.enableHiZViewer)) {
			if (hiZTexture) {
				uint32_t mip = settings.hizViewerMip;
				uint32_t w = std::max(1u, hiZWidth >> mip);
				uint32_t h = std::max(1u, hiZHeight >> mip);
				ImVec2 size = ImVec2(w * settings.hizViewerScale, h * settings.hizViewerScale);

				ImGui::Text("%s %u (%ux%u)", T(TKEY("mip"), "Mip"), mip, w, h);
				if (mip < hiZSRVsPerMip.size() && hiZSRVsPerMip[mip]) {
					ImGui::Image(reinterpret_cast<ImTextureID>(hiZSRVsPerMip[mip]), size);
				} else {
					ImGui::TextDisabled("%s", T(TKEY("mip_unavailable"), "Mip level not available"));
				}
			} else {
				ImGui::TextDisabled("%s", T(TKEY("pyramid_not_ready"), "Depth pyramid not ready"));
			}
		}
		ImGui::End();
	}

	stats.Reset();
}

void HiZOcclusion::DrawOverlay()
{
	// Draw bounds overlay full-screen when enabled
	// This is called every frame regardless of menu state
	if (settings.enableBoundsViewer && boundsOverlaySRV) {
		ImGuiViewport* vp = ImGui::GetMainViewport();
		ImVec2 p0 = vp->Pos;
		ImVec2 p1 = ImVec2(vp->Pos.x + vp->Size.x, vp->Pos.y + vp->Size.y);

		// Draw overlay image full-screen
		// Tint alpha controls opacity (e.g., 0x80 = 50%)
		ImU32 tint = IM_COL32(255, 255, 255, 192);
		ImGui::GetBackgroundDrawList()->AddImage(
			reinterpret_cast<ImTextureID>(boundsOverlaySRV),
			p0, p1, ImVec2(0, 0), ImVec2(1, 1), tint);

		// Optional: red border around whole screen
		ImGui::GetBackgroundDrawList()->AddRect(p0, p1, IM_COL32(255, 0, 0, 255));
	}
}

bool HiZOcclusion::IsOverlayVisible() const
{
	// Overlay is visible when bounds viewer is enabled and resource exists
	return settings.enableBoundsViewer && boundsOverlaySRV != nullptr;
}

void HiZOcclusion::LoadSettings(json& o_json)
{
	settings = o_json;
	settings.conservativeBias = std::clamp(settings.conservativeBias, 0.0f, 0.05f);
	settings.consecutiveOccludedThreshold = std::clamp(settings.consecutiveOccludedThreshold, 1u, 100u);
	settings.boundsMaxObjects = std::clamp(settings.boundsMaxObjects, 8u, 2048u);
	settings.hizViewerScale = std::clamp(settings.hizViewerScale, 0.1f, 4.0f);
	settings.shadowSweepDistance = std::clamp(settings.shadowSweepDistance, 0.0f, 16384.0f);
	settings.guardAngle = std::clamp(settings.guardAngle, 0.0f, 45.0f);
	settings.motionMarginFrames = std::clamp(settings.motionMarginFrames, 0.0f, 10.0f);
}

void HiZOcclusion::SaveSettings(json& o_json)
{
	o_json = settings;
}

void HiZOcclusion::RestoreDefaultSettings()
{
	settings = {};
}

// Preserve Feature base-class contract
void HiZOcclusion::SetupResources()
{
	InitShaders();
}

void HiZOcclusion::InitShaders()
{
	// Ensure we have a valid device before attempting shader compilation
	auto device = globals::d3d::device;
	if (!device) {
		status = HiZStatus::Error;
		statusMessage = "No D3D device available";
		logger::error("{}", statusMessage);
		return;
	}

	status = HiZStatus::CompilingShaders;
	statusMessage.clear();

	std::vector<std::pair<const char*, const char*>> shaderDefines;

	// Compile Hi-Z build shaders with error handling
	if (!hiZBuildLevel0CS) {
		try {
			hiZBuildLevel0CS = (ID3D11ComputeShader*)Util::CompileShader(L"Data\\Shaders\\HiZOcclusion\\HiZBuildLevel0CS.hlsl", shaderDefines, "cs_5_0");
			if (!hiZBuildLevel0CS) {
				status = HiZStatus::Error;
				statusMessage = "Failed to compile HiZBuildLevel0CS";
				logger::error("{}", statusMessage);
				return;
			}
		} catch (const std::exception& e) {
			status = HiZStatus::Error;
			statusMessage = std::string("HiZBuildLevel0CS compilation exception: ") + e.what();
			logger::error("{}", statusMessage);
			return;
		}
	}

	if (!hiZDownsampleCS) {
		try {
			hiZDownsampleCS = (ID3D11ComputeShader*)Util::CompileShader(L"Data\\Shaders\\HiZOcclusion\\HiZDownsampleCS.hlsl", shaderDefines, "cs_5_0");
			if (!hiZDownsampleCS) {
				status = HiZStatus::Error;
				statusMessage = "Failed to compile HiZDownsampleCS";
				logger::error("{}", statusMessage);
				return;
			}
		} catch (const std::exception& e) {
			status = HiZStatus::Error;
			statusMessage = std::string("HiZDownsampleCS compilation exception: ") + e.what();
			logger::error("{}", statusMessage);
			return;
		}
	}

	// Compile production shader variant (no debug overlay code)
	if (!hiZTestCS) {
		try {
			hiZTestCS = (ID3D11ComputeShader*)Util::CompileShader(L"Data\\Shaders\\HiZOcclusion\\HiZTestCS.hlsl", shaderDefines, "cs_5_0");
			if (!hiZTestCS) {
				status = HiZStatus::Error;
				statusMessage = "Failed to compile HiZTestCS";
				logger::error("{}", statusMessage);
				return;
			}
		} catch (const std::exception& e) {
			status = HiZStatus::Error;
			statusMessage = std::string("HiZTestCS compilation exception: ") + e.what();
			logger::error("{}", statusMessage);
			return;
		}
	}

	// Compile debug shader variant (includes overlay visualization)
	// Only compiled when needed to avoid unnecessary shader bloat in production
	if (!hiZTestCSDebug && (settings.debugMode || settings.enableBoundsViewer)) {
		auto debugDefines = shaderDefines;
		debugDefines.push_back({ "ENABLE_DEBUG_OVERLAY", "" });
		try {
			hiZTestCSDebug = (ID3D11ComputeShader*)Util::CompileShader(L"Data\\Shaders\\HiZOcclusion\\HiZTestCS.hlsl", debugDefines, "cs_5_0");
			if (!hiZTestCSDebug) {
				logger::warn("Failed to compile HiZTestCSDebug (non-fatal, debug overlay unavailable)");
				// Non-fatal: production shader still works
			}
		} catch (const std::exception& e) {
			logger::warn("HiZTestCSDebug compilation exception (non-fatal): {}", e.what());
			// Non-fatal: production shader still works
		}
	}

	resourcesSetup = true;
	status = HiZStatus::ShadersReady;
	statusMessage.clear();

	// Skip resource validation for a few frames after shader compilation
	// to avoid crashes during device state transitions
	skipValidationThisFrame = true;
}

void HiZOcclusion::ClearShaderCache()
{
	if (hiZBuildLevel0CS) {
		hiZBuildLevel0CS->Release();
		hiZBuildLevel0CS = nullptr;
	}
	if (hiZDownsampleCS) {
		hiZDownsampleCS->Release();
		hiZDownsampleCS = nullptr;
	}
	if (hiZTestCS) {
		hiZTestCS->Release();
		hiZTestCS = nullptr;
	}
	if (hiZTestCSDebug) {
		hiZTestCSDebug->Release();
		hiZTestCSDebug = nullptr;
	}
	resourcesSetup = false;
	debugShaderCompileAttempted = false;
}

void HiZOcclusion::Reset()
{
	{
		std::lock_guard<std::mutex> lock(threadVectorsMutex);
		for (auto* threadVec : allThreadVectors) {
			if (threadVec)
				threadVec->clear();
		}
	}

	grabbedReference = nullptr;
	if (auto* player = RE::PlayerCharacter::GetSingleton()) {
		if (auto grabbed = player->GetPlayerRuntimeData().grabData.grabbedObject.get())
			grabbedReference = grabbed.get();
	}

	if (!settings.enableHiZCulling) {
		if (wasEnabled) {
			// Restore all app-culled geometry and clear occlusion tracking
			ClearOcclusionState();

			// Release and clear all resources
			ReleaseBoundsOverlayResources();
			ReleaseDebugBuffer();
			UnbindD3DResources();
			// Reset stats
			stats.frameIndex = 0;
			stats.totalTested = 0;
			stats.geometryListSize = 0;
			stats.visTestPassed = 0;
			stats.visInsideBounds = 0;
			stats.visInvalidRadius = 0;
			stats.defaultValue = 0;
			stats.culledFrustum = 0;
			stats.culledNoEarlyOut = 0;
			stats.resourceSetupDurationMS = 0.0f;
			stats.recreateDurationMS = 0.0f;
			wasEnabled = false;
		}
	} else {
		if (!wasEnabled) {
			wasEnabled = true;
		}
	}
}

void HiZOcclusion::Prepass()
{
	if (!settings.enableHiZCulling) {
		return;
	}

	if (settings.debugMode) {
		logger::debug("HIZ Prepass - frame={}, pendingGeometry={}",
			globals::state->frameCount, pendingGeometry.size());
	}

	if (settings.enableBoundsViewer && boundsOverlayUAV) {
		overlayUpdatedThisFrame = false;
	}

	if (!resourcesSetup) {
		auto start = std::chrono::high_resolution_clock::now();
		InitShaders();
		auto end = std::chrono::high_resolution_clock::now();
		const double durationMs = std::chrono::duration<double, std::milli>(end - start).count();
		stats.resourceSetupDurationMS = static_cast<float>(durationMs);
	}

	if ((settings.debugMode || settings.enableBoundsViewer) && !hiZTestCSDebug && !debugShaderCompileAttempted) {
		debugShaderCompileAttempted = true;
		InitShaders();
	}

	// Early return if shader compilation failed
	if (!resourcesSetup) {
		status = HiZStatus::Error;
		statusMessage = "Shader compilation failed";
		return;
	}

	status = HiZStatus::ResourcesReady;
	statusMessage.clear();

	if (!InitHiZResources()) {
		// InitHiZResources sets appropriate status - don't spam logs
		return;
	}

	// Setup GPU culling resources if not already done
	if (!geometryBoundsBuffer || !hiZTestParamsBuffer || !hiZSampler || !visibilityResultsBuffer) {
		if (!SetupGPUCullingResources()) {
			// SetupGPUCullingResources sets appropriate status
			return;
		}
	}

	UpdateCameraMotion();

	// Prepare consolidated geometry list for testing
	ConsolidatePendingGeometry();

	// Update geometry list size stat
	stats.geometryListSize = static_cast<uint32_t>(pendingGeometry.size());

	// Reserve vector capacity
	if (pendingGeometry.capacity() < 16384) {
		pendingGeometry.reserve(16384);
		geometryBounds.reserve(16384);
	}

	if (readbackState.numPendingReads > 0 || !pendingGeometry.empty()) {
		ExecuteVisibilityTests();
	}

	if (!shadowOcclusion.occluded.empty() && Angle(appliedShadowForward, currentCameraForward) + frameRotation > settings.guardAngle)
		ClearShadowOcclusionState();

	// Only process visibility tests if we have geometry from previous frame
	if (!pendingGeometry.empty()) {
		// Clean up resources that we are finished with
		pendingGeometry.clear();
		pendingPasses.clear();
		pendingGeometryIndex.clear();
		geometryBounds.clear();
	} else {
		logger::debug("Frame {} - No pending geometry to process in Prepass", globals::state->frameCount);
	}

	if (settings.enableBoundsViewer && boundsOverlayUAV && !overlayUpdatedThisFrame) {
		ClearBoundsOverlay();
	}

	UnbindD3DResources();

	status = HiZStatus::Running;
	statusMessage.clear();
}

void HiZOcclusion::ConsolidatePendingGeometry()
{
	// Clear global consolidated containers
	pendingGeometry.clear();
	pendingPasses.clear();
	pendingGeometryIndex.clear();

	// Consolidate thread-local lists
	{
		std::lock_guard<std::mutex> lock(threadVectorsMutex);

		// Count total size first to pre-allocate memory
		size_t totalCount = 0;
		for (auto* threadVec : allThreadVectors) {
			if (threadVec) {
				totalCount += threadVec->size();
			}
		}
		pendingGeometry.reserve(totalCount);
		pendingPasses.reserve(totalCount);
		pendingGeometryIndex.reserve(totalCount);

		// Merge vectors and deduplicate
		for (auto* threadVec : allThreadVectors) {
			if (threadVec) {
				for (const auto& record : *threadVec) {
					auto [it, inserted] = pendingGeometryIndex.try_emplace(record.geometry, pendingGeometry.size());
					if (inserted) {
						pendingGeometry.emplace_back(record.geometry);
						pendingPasses.push_back(record.passes);
					} else {
						pendingPasses[it->second] |= record.passes;
					}
				}
				// Clear the thread-local vector for the next frame
				threadVec->clear();
			}
		}
	}
}

ID3D11ShaderResourceView* HiZOcclusion::GetSourceDepthSRV() const
{
	auto& terrainBlending = globals::features::terrainBlending;
	if (terrainBlending.loaded && terrainBlending.settings.Enabled && terrainBlending.prepassSRVBackup)
		return terrainBlending.prepassSRVBackup;
	return globals::game::renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kPOST_ZPREPASS_COPY].depthSRV;
}

bool HiZOcclusion::InitHiZResources()
{
	auto renderer = globals::game::renderer;
	auto context = globals::d3d::context;

	// Ensure resources are ready
	if (!renderer) {
		logger::error("Renderer not ready");
		status = HiZStatus::Error;
		statusMessage = "Renderer not available";
		return false;
	}

	auto& depth = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kPOST_ZPREPASS_COPY];
	auto* depthSRV = GetSourceDepthSRV();
	if (!depthSRV || !depth.texture) {
		logger::error("no depth texture SRV");
		status = HiZStatus::Error;
		statusMessage = "Depth texture SRV unavailable";
		return false;
	}

	D3D11_SHADER_RESOURCE_VIEW_DESC depthSRVDesc{};
	depthSRV->GetDesc(&depthSRVDesc);

	// Check if depth format is compatible
	if (depthSRVDesc.Format != DXGI_FORMAT_R24_UNORM_X8_TYPELESS &&
		depthSRVDesc.Format != DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS &&
		depthSRVDesc.Format != DXGI_FORMAT_R32_FLOAT &&
		depthSRVDesc.Format != DXGI_FORMAT_R16_UNORM &&
		!unexpectedDepthFormatLogged) {
		logger::warn("Unexpected depth format: {}", static_cast<int>(depthSRVDesc.Format));
		unexpectedDepthFormatLogged = true;
	}

	D3D11_TEXTURE2D_DESC depthDesc{};
	depth.texture->GetDesc(&depthDesc);

	if (!depthDesc.Width || !depthDesc.Height) {
		logger::error("depth texture has invalid dimensions");
		status = HiZStatus::Error;
		statusMessage = "Invalid depth texture dimensions";
		return false;
	}

	float2 screenSize{ (float)globals::game::graphicsState->screenWidth, (float)globals::game::graphicsState->screenHeight };
	auto renderSize = Util::ConvertToDynamic(screenSize);
	uint32_t desiredW = std::clamp<uint32_t>(static_cast<uint32_t>(std::lround(renderSize.x)), 1u, depthDesc.Width);
	uint32_t desiredH = std::clamp<uint32_t>(static_cast<uint32_t>(std::lround(renderSize.y)), 1u, depthDesc.Height);

	// Build Hi-Z pyramid from the depth buffer
	// Ensure Hi-Z texture exists and matches current depth dimensions
	auto device = globals::d3d::device;
	if (!device) {
		logger::error("no D3D device");
		status = HiZStatus::Error;
		statusMessage = "D3D device unavailable";
		return false;
	}

	// Detailed diagnostics for resource recreation
	const bool textureNull = (hiZTexture == nullptr);
	const bool widthMismatch = (hiZWidth != desiredW);
	const bool heightMismatch = (hiZHeight != desiredH);
	const bool needRecreate = textureNull || widthMismatch || heightMismatch;

	if (needRecreate) {
		auto startRecreateTimer = std::chrono::high_resolution_clock::now();

		// Compute mip count for the new texture
		uint32_t w = desiredW;
		uint32_t h = desiredH;
		uint32_t newMipCount = 1;
		while (w > 1 || h > 1) {
			w = std::max(1u, w >> 1);
			h = std::max(1u, h >> 1);
			++newMipCount;
		}

		// Create new resources into temporaries
		ID3D11Texture2D* newTexture = nullptr;
		ID3D11ShaderResourceView* newSRV = nullptr;
		std::vector<ID3D11ShaderResourceView*> newSRVsPerMip;
		std::vector<ID3D11UnorderedAccessView*> newUAVs;
		newSRVsPerMip.reserve(newMipCount);
		newUAVs.reserve(newMipCount);

		D3D11_TEXTURE2D_DESC tdesc{};
		tdesc.Width = desiredW;
		tdesc.Height = desiredH;
		tdesc.MipLevels = newMipCount;
		tdesc.ArraySize = 1;
		tdesc.Format = DXGI_FORMAT_R32_FLOAT;
		tdesc.SampleDesc.Count = 1;
		tdesc.Usage = D3D11_USAGE_DEFAULT;
		tdesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
		HRESULT hr = device->CreateTexture2D(&tdesc, nullptr, &newTexture);
		if (FAILED(hr) || !newTexture) {
			status = HiZStatus::Error;
			statusMessage = "Failed to create Hi-Z texture";
			logger::error("{}", statusMessage);
			if (newTexture)
				newTexture->Release();
			return false;
		}
		Util::SetResourceName(newTexture, "HiZOcclusion::HiZPyramid");

		D3D11_SHADER_RESOURCE_VIEW_DESC sdesc{};
		sdesc.Format = DXGI_FORMAT_R32_FLOAT;
		sdesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
		sdesc.Texture2D.MostDetailedMip = 0;
		sdesc.Texture2D.MipLevels = newMipCount;
		HRESULT srvResult = device->CreateShaderResourceView(newTexture, &sdesc, &newSRV);
		if (FAILED(srvResult) || !newSRV) {
			status = HiZStatus::Error;
			statusMessage = "Failed to create Hi-Z SRV";
			logger::error("{}", statusMessage);
			if (newSRV)
				newSRV->Release();
			if (newTexture)
				newTexture->Release();
			return false;
		}
		Util::SetResourceName(newSRV, "HiZOcclusion::HiZPyramid SRV");

		bool perMipOk = true;
		for (uint32_t i = 0; i < newMipCount; ++i) {
			ID3D11ShaderResourceView* srvMip = nullptr;
			D3D11_SHADER_RESOURCE_VIEW_DESC sM{};
			sM.Format = DXGI_FORMAT_R32_FLOAT;
			sM.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
			sM.Texture2D.MostDetailedMip = i;
			sM.Texture2D.MipLevels = 1;
			HRESULT srvMipResult = device->CreateShaderResourceView(newTexture, &sM, &srvMip);
			if (FAILED(srvMipResult) || !srvMip) {
				perMipOk = false;
			} else {
				Util::SetResourceName(srvMip, "HiZOcclusion::HiZPyramid Mip %u SRV", i);
				newSRVsPerMip.push_back(srvMip);
			}

			ID3D11UnorderedAccessView* uavMip = nullptr;
			D3D11_UNORDERED_ACCESS_VIEW_DESC uM{};
			uM.Format = DXGI_FORMAT_R32_FLOAT;
			uM.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
			uM.Texture2D.MipSlice = i;
			HRESULT uavMipResult = device->CreateUnorderedAccessView(newTexture, &uM, &uavMip);
			if (FAILED(uavMipResult) || !uavMip) {
				perMipOk = false;
			} else {
				Util::SetResourceName(uavMip, "HiZOcclusion::HiZPyramid Mip %u UAV", i);
				newUAVs.push_back(uavMip);
			}

			if (!perMipOk)
				break;
		}

		if (!perMipOk || newSRVsPerMip.size() != newMipCount || newUAVs.size() != newMipCount) {
			status = HiZStatus::Error;
			statusMessage = "Failed to create per-mip views";
			logger::error("{}", statusMessage);
			for (auto* v : newSRVsPerMip) {
				if (v)
					v->Release();
			}
			for (auto* u : newUAVs) {
				if (u)
					u->Release();
			}
			if (newSRV)
				newSRV->Release();
			if (newTexture)
				newTexture->Release();
			return false;
		}

		// Success: release old and swap in new resources
		if (hiZSRV) {
			hiZSRV->Release();
			hiZSRV = nullptr;
		}
		for (auto* v : hiZSRVsPerMip) {
			if (v)
				v->Release();
		}
		hiZSRVsPerMip.clear();
		for (auto* u : hiZUAVs) {
			if (u)
				u->Release();
		}
		hiZUAVs.clear();
		if (hiZTexture) {
			hiZTexture->Release();
			hiZTexture = nullptr;
		}

		hiZTexture = newTexture;
		hiZSRV = newSRV;
		hiZSRVsPerMip = std::move(newSRVsPerMip);
		hiZUAVs = std::move(newUAVs);
		hiZWidth = desiredW;
		hiZHeight = desiredH;
		hiZMipCount = newMipCount;
		resourceCreationFrame = globals::state->frameCount;
		resourcesValid = true;

		// Validate all SRVs are non-null
		// Note: D3D11 COM calls return HRESULTs, they don't throw C++ exceptions,
		// so we rely on null checks rather than try-catch blocks.
		bool allSRVsValid = true;
		for (uint32_t i = 0; i < hiZMipCount; ++i) {
			if (!hiZSRVsPerMip[i]) {
				logger::error("SRV for mip {} is null!", i);
				allSRVsValid = false;
			}
		}

		// If validation failed, mark resources as needing recreation
		if (!allSRVsValid) {
			logger::warn("SRV validation failed - resources may need recreation on next frame");
			status = HiZStatus::ValidationFailed;
			statusMessage = "SRV validation failed, will retry";
			resourcesValid = false;
			// Skip validation on next few frames to prevent repeated crashes
			skipValidationThisFrame = true;
		}
		auto endRecreateTimer = std::chrono::high_resolution_clock::now();
		const double recreateDuration = std::chrono::duration<double, std::milli>(endRecreateTimer - startRecreateTimer).count();
		stats.recreateDurationMS = static_cast<float>(recreateDuration);
	}

	// Build level 0 from depth with safety checks
	{
		// Verify all required resources are valid before proceeding
		if (!hiZBuildLevel0CS) {
			logger::error("hiZBuildLevel0CS is null - cannot build Hi-Z pyramid");
			status = HiZStatus::Error;
			statusMessage = "Build shader unavailable";
			return false;
		}

		if (hiZUAVs.empty() || !hiZUAVs[0]) {
			logger::error("hiZUAVs[0] is null - cannot build Hi-Z pyramid");
			status = HiZStatus::Error;
			statusMessage = "UAV[0] unavailable";
			return false;
		}

		ID3D11ShaderResourceView* srvs[1] = { depthSRV };
		context->CSSetShaderResources(0, 1, srvs);
		ID3D11UnorderedAccessView* uavs[1] = { hiZUAVs[0] };
		context->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);
		context->CSSetShader(hiZBuildLevel0CS, nullptr, 0);

		const uint32_t tgX = 16, tgY = 16;
		uint32_t groupsX = (hiZWidth + tgX - 1) / tgX;
		uint32_t groupsY = (hiZHeight + tgY - 1) / tgY;
		context->Dispatch(groupsX, groupsY, 1);

		// Unbind (must pass arrays, not raw nullptr, when Count > 0)
		ID3D11UnorderedAccessView* nullUAVs_lvl0[1] = { nullptr };
		context->CSSetUnorderedAccessViews(0, 1, nullUAVs_lvl0, nullptr);
		ID3D11ShaderResourceView* nullSRVs_lvl0[1] = { nullptr };
		context->CSSetShaderResources(0, 1, nullSRVs_lvl0);
		context->CSSetShader(nullptr, nullptr, 0);
	}

	// Downsample pyramid with farthest-depth reduction
	uint32_t srcW = desiredW;
	uint32_t srcH = desiredH;
	for (uint32_t mip = 0; mip + 1 < hiZMipCount; ++mip) {
		uint32_t dstW = std::max(1u, srcW >> 1);
		uint32_t dstH = std::max(1u, srcH >> 1);

		// Safety checks for each mip level
		if (mip >= hiZSRVsPerMip.size() || !hiZSRVsPerMip[mip]) {
			logger::error("hiZSRVsPerMip[{}] is null - aborting pyramid build", mip);
			status = HiZStatus::Error;
			statusMessage = "Null SRV at mip " + std::to_string(mip);
			break;
		}

		if (mip + 1 >= hiZUAVs.size() || !hiZUAVs[mip + 1]) {
			logger::error("hiZUAVs[{}] is null - aborting pyramid build", mip + 1);
			status = HiZStatus::Error;
			statusMessage = "Null UAV at mip " + std::to_string(mip + 1);
			break;
		}

		if (!hiZDownsampleCS) {
			logger::error("hiZDownsampleCS is null - aborting pyramid build");
			status = HiZStatus::Error;
			statusMessage = "Downsample shader unavailable";
			break;
		}

		ID3D11ShaderResourceView* srvIn[1] = { hiZSRVsPerMip[mip] };
		context->CSSetShaderResources(0, 1, srvIn);
		ID3D11UnorderedAccessView* uavOut[1] = { hiZUAVs[mip + 1] };
		context->CSSetUnorderedAccessViews(0, 1, uavOut, nullptr);
		context->CSSetShader(hiZDownsampleCS, nullptr, 0);

		const uint32_t tgX = 16, tgY = 16;
		uint32_t groupsX = (dstW + tgX - 1) / tgX;
		uint32_t groupsY = (dstH + tgY - 1) / tgY;
		context->Dispatch(groupsX, groupsY, 1);

		// Unbind for next level (must pass arrays, not raw nullptr)
		ID3D11UnorderedAccessView* nullUAVs_ds[1] = { nullptr };
		context->CSSetUnorderedAccessViews(0, 1, nullUAVs_ds, nullptr);
		ID3D11ShaderResourceView* nullSRVs_ds[1] = { nullptr };
		context->CSSetShaderResources(0, 1, nullSRVs_ds);
		context->CSSetShader(nullptr, nullptr, 0);

		srcW = dstW;
		srcH = dstH;
	}

	return true;
}

bool HiZOcclusion::SetupGPUCullingResources()
{
	auto device = globals::d3d::device;
	if (!device)
		return false;

	// Create geometry bounds buffer (input)
	D3D11_BUFFER_DESC bufferDesc = {};
	bufferDesc.ByteWidth = maxGeometryCount * sizeof(TestEntry);
	bufferDesc.Usage = D3D11_USAGE_DEFAULT;
	bufferDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
	bufferDesc.CPUAccessFlags = 0;
	bufferDesc.StructureByteStride = sizeof(TestEntry);
	bufferDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;

	HRESULT hr = device->CreateBuffer(&bufferDesc, nullptr, &geometryBoundsBuffer);
	if (FAILED(hr)) {
		logger::error("Failed to create geometry bounds buffer");
		return false;
	}
	Util::SetResourceName(geometryBoundsBuffer, "HiZOcclusion::GeometryBounds");

	// Create SRV for geometry bounds
	D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
	srvDesc.Format = DXGI_FORMAT_UNKNOWN;
	srvDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
	srvDesc.Buffer.NumElements = maxGeometryCount;

	HRESULT srvCreateResult = device->CreateShaderResourceView(geometryBoundsBuffer, &srvDesc, &geometryBoundsSRV);
	if (FAILED(srvCreateResult)) {
		logger::error("Failed to create geometry bounds SRV");
		return false;
	}
	Util::SetResourceName(geometryBoundsSRV, "HiZOcclusion::GeometryBounds SRV");

	// Create visibility results buffer (output, batch), one result code per element
	bufferDesc.ByteWidth = maxGeometryCount * sizeof(OcclusionResult);
	bufferDesc.Usage = D3D11_USAGE_DEFAULT;
	bufferDesc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
	bufferDesc.CPUAccessFlags = 0;  // No CPU access for UAV buffers
	bufferDesc.StructureByteStride = sizeof(OcclusionResult);
	bufferDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;

	HRESULT visibilityBufferResult = device->CreateBuffer(&bufferDesc, nullptr, &visibilityResultsBuffer);
	if (FAILED(visibilityBufferResult)) {
		logger::error("Failed to create visibility results buffer");
		return false;
	}
	Util::SetResourceName(visibilityResultsBuffer, "HiZOcclusion::VisibilityResults");

	// Create UAV for visibility results (batch)
	D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
	uavDesc.Format = DXGI_FORMAT_UNKNOWN;
	uavDesc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
	uavDesc.Buffer.NumElements = maxGeometryCount;

	HRESULT visibilityUAVResult = device->CreateUnorderedAccessView(visibilityResultsBuffer, &uavDesc, &visibilityResultsUAV);
	if (FAILED(visibilityUAVResult)) {
		logger::error("Failed to create visibility results UAV");
		return false;
	}
	Util::SetResourceName(visibilityResultsUAV, "HiZOcclusion::VisibilityResults UAV");

	// Create constant buffer for Hi-Z test parameters
	bufferDesc.ByteWidth = sizeof(HiZSettings);
	bufferDesc.Usage = D3D11_USAGE_DYNAMIC;
	bufferDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
	bufferDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
	bufferDesc.StructureByteStride = 0;
	bufferDesc.MiscFlags = 0;

	HRESULT paramsBufferResult = device->CreateBuffer(&bufferDesc, nullptr, &hiZTestParamsBuffer);
	if (FAILED(paramsBufferResult)) {
		logger::error("Failed to create Hi-Z test params buffer");
		return false;
	}
	Util::SetResourceName(hiZTestParamsBuffer, "HiZOcclusion::TestParams");

	// Create triple-buffered staging buffers for async readback
	D3D11_BUFFER_DESC readbackDesc = {};
	readbackDesc.ByteWidth = maxGeometryCount * sizeof(OcclusionResult);
	readbackDesc.Usage = D3D11_USAGE_STAGING;
	readbackDesc.BindFlags = 0;
	readbackDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
	readbackDesc.StructureByteStride = 0;
	readbackDesc.MiscFlags = 0;

	for (int i = 0; i < AsyncReadbackState::BUFFER_COUNT; ++i) {
		HRESULT rbhr = device->CreateBuffer(&readbackDesc, nullptr, &readbackState.stagingBuffers[i]);
		if (FAILED(rbhr)) {
			logger::error("Failed to create visibility readback buffer {}", i);
			// Clean up any buffers we created
			for (int j = 0; j < i; ++j) {
				if (readbackState.stagingBuffers[j]) {
					readbackState.stagingBuffers[j]->Release();
					readbackState.stagingBuffers[j] = nullptr;
				}
				if (readbackState.completionQueries[j]) {
					readbackState.completionQueries[j]->Release();
					readbackState.completionQueries[j] = nullptr;
				}
			}
			return false;
		}
		Util::SetResourceName(readbackState.stagingBuffers[i], "HiZOcclusion::VisibilityReadback %d", i);

		// Create event query for precise GPU completion detection
		D3D11_QUERY_DESC queryDesc = {};
		queryDesc.Query = D3D11_QUERY_EVENT;
		queryDesc.MiscFlags = 0;
		HRESULT queryHr = device->CreateQuery(&queryDesc, &readbackState.completionQueries[i]);
		if (FAILED(queryHr)) {
			logger::warn("Failed to create completion query {} - falling back to polling", i);
			readbackState.completionQueries[i] = nullptr;
			// Non-fatal: will fall back to DO_NOT_WAIT polling if query creation fails
		}

		readbackState.hasPendingRead[i] = false;
		readbackState.pendingFrameIndex[i] = 0;
	}
	readbackState.writeIndex = 0;
	readbackState.readIndex = 0;
	readbackState.numPendingReads = 0;

	// Create sampler for Hi-Z sampling
	D3D11_SAMPLER_DESC sampDesc = {};
	sampDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
	sampDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
	sampDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
	sampDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
	sampDesc.ComparisonFunc = D3D11_COMPARISON_NEVER;
	sampDesc.MinLOD = 0;
	sampDesc.MaxLOD = D3D11_FLOAT32_MAX;
	HRESULT sr = device->CreateSamplerState(&sampDesc, &hiZSampler);
	if (FAILED(sr)) {
		logger::error("Failed to create Hi-Z sampler");
		return false;
	}
	Util::SetResourceName(hiZSampler, "HiZOcclusion::PointSampler");

	// Create GPU timestamp queries for accurate timing (triple-buffered)
	for (uint32_t i = 0; i < GPU_TIMING_BUFFER_COUNT; ++i) {
		D3D11_QUERY_DESC timestampDesc = {};
		timestampDesc.Query = D3D11_QUERY_TIMESTAMP;
		timestampDesc.MiscFlags = 0;

		D3D11_QUERY_DESC disjointDesc = {};
		disjointDesc.Query = D3D11_QUERY_TIMESTAMP_DISJOINT;
		disjointDesc.MiscFlags = 0;

		if (FAILED(device->CreateQuery(&disjointDesc, &gpuTimingQueries[i].disjointQuery)) ||
			FAILED(device->CreateQuery(&timestampDesc, &gpuTimingQueries[i].beginTimestamp)) ||
			FAILED(device->CreateQuery(&timestampDesc, &gpuTimingQueries[i].endTimestamp))) {
			logger::warn("Failed to create GPU timestamp queries for timing slot {}", i);
			// Non-fatal: fall back to CPU timing
		}
		gpuTimingQueries[i].pending = false;
	}
	gpuTimingWriteIndex = 0;
	gpuTimingReadIndex = 0;

	// Debug output buffers
	if (settings.debugMode || settings.enableBoundsViewer) {
		// Only create debug buffer when actually debugging
		if (!debugResultsBuffer) {
			CreateDebugBuffer();
		}
	} else {
		// Release debug buffer when not needed
		ReleaseDebugBuffer();
	}
	return true;
}

void HiZOcclusion::CreateDebugBuffer()
{
	auto device = globals::d3d::device;
	if (!device)
		return;

	const uint32_t debugElementCount = maxGeometryCount;
	D3D11_BUFFER_DESC dbgDesc = {};
	dbgDesc.ByteWidth = debugElementCount * sizeof(HiZOcclusion::DebugData);
	dbgDesc.Usage = D3D11_USAGE_DEFAULT;
	dbgDesc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
	dbgDesc.CPUAccessFlags = 0;
	dbgDesc.StructureByteStride = sizeof(HiZOcclusion::DebugData);
	dbgDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;

	HRESULT dbr = device->CreateBuffer(&dbgDesc, nullptr, &debugResultsBuffer);
	if (FAILED(dbr)) {
		logger::warn("Failed to create debugResultsBuffer");
		return;
	}
	Util::SetResourceName(debugResultsBuffer, "HiZOcclusion::DebugResults");

	D3D11_UNORDERED_ACCESS_VIEW_DESC dbgUavDesc = {};
	dbgUavDesc.Format = DXGI_FORMAT_UNKNOWN;
	dbgUavDesc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
	dbgUavDesc.Buffer.NumElements = debugElementCount;
	if (SUCCEEDED(device->CreateUnorderedAccessView(debugResultsBuffer, &dbgUavDesc, &debugResultsUAV)))
		Util::SetResourceName(debugResultsUAV, "HiZOcclusion::DebugResults UAV");
}

void HiZOcclusion::ReleaseDebugBuffer()
{
	if (debugResultsBuffer) {
		debugResultsBuffer->Release();
		debugResultsBuffer = nullptr;
	}
	if (debugResultsUAV) {
		debugResultsUAV->Release();
		debugResultsUAV = nullptr;
	}
}

bool HiZOcclusion::IsReadbackReady(uint32_t bufferIndex)
{
	auto context = globals::d3d::context;
	if (auto* query = readbackState.completionQueries[bufferIndex]) {
		BOOL done = FALSE;
		return context->GetData(query, &done, sizeof(done), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK && done;
	}

	D3D11_MAPPED_SUBRESOURCE mapped{};
	if (FAILED(context->Map(readbackState.stagingBuffers[bufferIndex], 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped)))
		return false;
	context->Unmap(readbackState.stagingBuffers[bufferIndex], 0);
	return true;
}

void HiZOcclusion::ReleaseReadbackSlot(uint32_t bufferIndex)
{
	if (!readbackState.hasPendingRead[bufferIndex])
		return;
	readbackState.hasPendingRead[bufferIndex] = false;
	readbackState.geometrySnapshots[bufferIndex].clear();
	readbackState.geometryCount[bufferIndex] = 0;
	readbackState.numPendingReads--;
}

void HiZOcclusion::ExecuteVisibilityTests()
{
	auto context = globals::d3d::context;
	auto device = globals::d3d::device;
	if (!context || !device) {
		logger::warn("ExecuteVisibilityTests: D3D context or device not initialized");
		return;
	}

	// Track whether we got fresh results this frame
	bool gotNewResults = false;

	if (readbackState.numPendingReads > 0) {
		auto readStart = std::chrono::high_resolution_clock::now();

		uint32_t newestReady = UINT32_MAX;
		while (readbackState.numPendingReads > 0) {
			const uint32_t bufferIdx = readbackState.readIndex;
			if (!readbackState.hasPendingRead[bufferIdx] || !IsReadbackReady(bufferIdx))
				break;
			if (newestReady != UINT32_MAX)
				ReleaseReadbackSlot(newestReady);
			newestReady = bufferIdx;
			readbackState.readIndex = (bufferIdx + 1) % AsyncReadbackState::BUFFER_COUNT;
		}

		if (newestReady != UINT32_MAX) {
			auto mapStart = std::chrono::high_resolution_clock::now();
			HRESULT hr = context->Map(readbackState.stagingBuffers[newestReady], 0, D3D11_MAP_READ, 0, &readbackState.mappedData[newestReady]);
			auto mapEnd = std::chrono::high_resolution_clock::now();
			stats.mapTimeMs = static_cast<float>(std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(mapEnd - mapStart).count());

			if (SUCCEEDED(hr)) {
				auto copyStart = std::chrono::high_resolution_clock::now();
				ProcessVisibilityResults(newestReady);
				gotNewResults = true;
				auto copyEnd = std::chrono::high_resolution_clock::now();
				stats.copyDataTimeMs = static_cast<float>(std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(copyEnd - copyStart).count());

				auto unmapStart = std::chrono::high_resolution_clock::now();
				context->Unmap(readbackState.stagingBuffers[newestReady], 0);
				auto unmapEnd = std::chrono::high_resolution_clock::now();
				stats.unmapTimeMs = static_cast<float>(std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(unmapEnd - unmapStart).count());
			}
			ReleaseReadbackSlot(newestReady);
		}

		auto readEnd = std::chrono::high_resolution_clock::now();
		stats.readbackTimeMs = static_cast<float>(std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(readEnd - readStart).count());
	}

	// Track stale frame count when no new results were obtained
	// The occluded sets persist from the previous frame automatically
	if (!gotNewResults && stats.lastResultFrame > 0) {
		stats.staleFrameCount = globals::state->frameCount - stats.lastResultFrame;
		if (settings.debugMode && stats.staleFrameCount > 0) {
			logger::debug("Frame {} - Using cached occlusion results from frame {} ({} frames stale, {} geometry occluded)",
				globals::state->frameCount, stats.lastResultFrame, stats.staleFrameCount, cameraOcclusion.occluded.size());
		}
	}

	// Dispatch new test if we have geometry to process
	if (!pendingGeometry.empty()) {
		geometryBounds.clear();
		pendingGeometrySnapshot.clear();
		geometryBounds.reserve(pendingGeometry.size());
		pendingGeometrySnapshot.reserve(pendingGeometry.size());

		DirectX::XMFLOAT4 sunSweep{ 0.0f, 0.0f, 0.0f, 0.0f };
		bool hasSunSweep = false;
		if (sunShadowDirectionCaptured.exchange(false, std::memory_order_acquire) && settings.shadowSweepDistance > 0.0f) {
			const RE::NiPoint3 direction{ sunShadowDirection[0].load(std::memory_order_relaxed), sunShadowDirection[1].load(std::memory_order_relaxed), sunShadowDirection[2].load(std::memory_order_relaxed) };
			const float length = direction.Length();
			if (length > 0.0f) {
				const float scale = settings.shadowSweepDistance / length;
				sunSweep = { direction.x * scale, direction.y * scale, direction.z * scale, 0.0f };
				hasSunSweep = true;
			}
		}

		for (size_t i = 0; i < pendingGeometry.size() && geometryBounds.size() < maxGeometryCount; ++i) {
			const auto& geometry = pendingGeometry[i];
			if (!geometry || geometry->worldBound.radius <= 0.0f)
				continue;

			const auto& worldBound = geometry->worldBound;
			const DirectX::XMFLOAT4 bounds{ worldBound.center.x, worldBound.center.y, worldBound.center.z, worldBound.radius };
			const uint8_t passes = pendingPasses[i];

			if (passes & kCameraPass) {
				geometryBounds.push_back({ bounds, { 0.0f, 0.0f, 0.0f, 0.0f } });
				pendingGeometrySnapshot.push_back({ geometry, kCameraPass });
			}
			if ((passes & kSunShadowPass) && hasSunSweep && geometryBounds.size() < maxGeometryCount) {
				geometryBounds.push_back({ bounds, sunSweep });
				pendingGeometrySnapshot.push_back({ geometry, kSunShadowPass });
			}
		}

		numGeometry = static_cast<uint32_t>(geometryBounds.size());

		if (numGeometry == 0) {
			return;
		}

		logger::debug("ExecuteVisibilityTests: Frame {} - Processing {} test entries", globals::state->frameCount, numGeometry);

		// Execute HiZ Tests for this frame
		DispatchComputeShader();

		// Check if we have a free staging buffer
		if (readbackState.numPendingReads >= AsyncReadbackState::BUFFER_COUNT) {
			logger::warn("All {} staging buffers are full! GPU readback is severely delayed. Skipping oldest buffer.",
				AsyncReadbackState::BUFFER_COUNT);
			const uint32_t oldestIdx = readbackState.readIndex;
			ReleaseReadbackSlot(oldestIdx);
			readbackState.readIndex = (oldestIdx + 1) % AsyncReadbackState::BUFFER_COUNT;
		}

		// Copy current frame results to next available staging buffer
		uint32_t writeIdx = readbackState.writeIndex;

		auto copyStart = std::chrono::high_resolution_clock::now();
		context->CopyResource(readbackState.stagingBuffers[writeIdx], visibilityResultsBuffer);

		// Issue event query to detect when GPU copy completes
		if (readbackState.completionQueries[writeIdx]) {
			context->End(readbackState.completionQueries[writeIdx]);
		}

		auto copyEnd = std::chrono::high_resolution_clock::now();
		stats.copyTimeMs = static_cast<float>(std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(copyEnd - copyStart).count());

		// Store geometry snapshot WITH this buffer so results match when read back
		readbackState.geometrySnapshots[writeIdx] = std::move(pendingGeometrySnapshot);
		pendingGeometrySnapshot.clear();
		readbackState.geometryCount[writeIdx] = numGeometry;
		readbackState.cameraForward[writeIdx] = currentCameraForward;

		// Mark buffer as pending
		readbackState.hasPendingRead[writeIdx] = true;
		readbackState.pendingFrameIndex[writeIdx] = globals::state->frameCount;
		readbackState.numPendingReads++;

		// Advance write index for next frame
		readbackState.writeIndex = (readbackState.writeIndex + 1) % AsyncReadbackState::BUFFER_COUNT;

		// Update statistics
		stats.frameIndex = globals::state->frameCount;
	}
}

void HiZOcclusion::UnbindD3DResources()
{
	auto context = globals::d3d::context;

	// Local null arrays for unbinding - avoids per-instance member overhead
	ID3D11Buffer* nullCBs[1] = { nullptr };
	ID3D11SamplerState* nullSamplers[1] = { nullptr };
	ID3D11ShaderResourceView* nullSRVs[2] = { nullptr, nullptr };
	ID3D11UnorderedAccessView* nullUAV[3] = { nullptr, nullptr, nullptr };

	context->CSSetShaderResources(0, 2, nullSRVs);               // t0 and t1
	context->CSSetUnorderedAccessViews(0, 3, nullUAV, nullptr);  // u0, u1, u2
	context->CSSetShader(nullptr, nullptr, 0);
	context->CSSetSamplers(0, 1, nullSamplers);
	context->CSSetConstantBuffers(0, 1, nullCBs);
}

void HiZOcclusion::DispatchComputeShader()
{
	auto* context = globals::d3d::context;
	auto* device = globals::d3d::device;
	auto* renderer = globals::game::renderer;
	if (!context || !device || !renderer) {
		logger::warn("DispatchComputeShader: missing D3D context/device/renderer");
		return;
	}

	if (!hiZTestCS || !geometryBoundsBuffer || !visibilityResultsBuffer || !hiZTestParamsBuffer || !hiZSRV || !hiZSampler) {
		logger::warn("DispatchComputeShader: required resources not ready");
		return;
	}

	// geometryBounds and pendingGeometrySnapshot are already populated by ExecuteVisibilityTests()
	if (numGeometry == 0 || geometryBounds.empty()) {
		return;
	}

	// Update the buffer for GPU
	const D3D11_BOX boundsBox{ 0, 0, 0, numGeometry * static_cast<UINT>(sizeof(TestEntry)), 1, 1 };
	context->UpdateSubresource(geometryBoundsBuffer, 0, &boundsBox, geometryBounds.data(), 0, 0);

	// Update constant buffer with camera parameters
	{
		D3D11_MAPPED_SUBRESOURCE mapped{};
		HiZSettings params{};
		params.hiZParams = DirectX::XMFLOAT4(
			static_cast<float>(hiZMipCount),
			settings.conservativeBias,
			static_cast<float>(numGeometry),
			settings.debugMode ? 1.0f : 0.0f);
		auto eyePos = Util::GetEyePosition();
		params.cameraWorldPos = DirectX::XMFLOAT3(eyePos.x, eyePos.y, eyePos.z);
		params.motionMargin = motionMargin;

		const auto& projection = globals::game::shadowState->GetRuntimeData().cameraData.getEye().projMat;
		params.guardBand = { GuardBandUV(std::abs(projection(0, 0)), settings.guardAngle), GuardBandUV(std::abs(projection(1, 1)), settings.guardAngle) };

		// Only pack overlay settings when debug/overlay is actually enabled
		// This avoids unnecessary CPU work per dispatch in production
		if (settings.enableBoundsViewer || settings.debugMode) {
			params.overlaySettings = DirectX::XMFLOAT4(
				settings.enableBoundsViewer ? 1.0f : 0.0f,
				static_cast<float>(settings.boundsMaxObjects),
				0.0f, 0.0f);

			// Pack color toggles into float4 (7 bits used)
			float toggleBits = 0.0f;
			if (settings.showVisTestPassed)
				toggleBits += 1.0f;  // bit 0
			if (settings.showVisInsideBounds)
				toggleBits += 2.0f;  // bit 1
			if (settings.showVisInvalidRadius)
				toggleBits += 4.0f;  // bit 2
			if (settings.showCulledFrustum)
				toggleBits += 8.0f;  // bit 3
			if (settings.showCulledNoEarlyOut)
				toggleBits += 16.0f;  // bit 4
			params.overlayColorToggles = DirectX::XMFLOAT4(toggleBits, 0.0f, 0.0f, 0.0f);
		}

		D3D11_TEXTURE2D_DESC texDesc{};
		renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN].texture->GetDesc(&texDesc);

		params.bufferDim = { (float)texDesc.Width, (float)texDesc.Height };

		if (SUCCEEDED(context->Map(hiZTestParamsBuffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
			memcpy(mapped.pData, &params, sizeof(HiZSettings));
			context->Unmap(hiZTestParamsBuffer, 0);
		} else {
			logger::warn("ExecuteVisibilityTests: failed to map hiZTestParamsBuffer");
			return;
		}

		if (settings.debugMode) {
			logger::debug(
				"HiZ Dispatch params: mipCount={}, bias={:.4f}, geomCount={}, "
				"camPos=({:.1f},{:.1f},{:.1f}), bufDim=({:.0f},{:.0f})",
				hiZMipCount, settings.conservativeBias, numGeometry,
				params.cameraWorldPos.x, params.cameraWorldPos.y, params.cameraWorldPos.z,
				params.bufferDim.x, params.bufferDim.y);

			// Log first 3 geometry bounds to verify world-space coords
			for (uint32_t i = 0; i < std::min<uint32_t>(3, numGeometry); ++i) {
				const auto& b = geometryBounds[i].bounds;
				const auto& sweep = geometryBounds[i].sweep;
				logger::debug("  bounds[{}]: center=({:.1f},{:.1f},{:.1f}) radius={:.1f} sweep=({:.1f},{:.1f},{:.1f})",
					i, b.x, b.y, b.z, b.w, sweep.x, sweep.y, sweep.z);
			}
		}
	}

	// Bind resources and dispatch Hi-Z test compute shader for batch processing
	{
		if (settings.enableBoundsViewer) {
			if (!boundsOverlayTex || boundsOverlayW != hiZWidth || boundsOverlayH != hiZHeight) {
				ReleaseBoundsOverlayResources();
				SetupBoundsOverlayResources(hiZWidth, hiZHeight);
			}
			ClearBoundsOverlay();
		}

		const bool overlayEnabled = settings.enableBoundsViewer && (boundsOverlayUAV != nullptr);

		UINT uavCount = overlayEnabled ? 3u : 2u;
		ID3D11UnorderedAccessView* uavs[3] = {
			visibilityResultsUAV,
			(settings.debugMode || settings.enableBoundsViewer) ? debugResultsUAV : nullptr,
			overlayEnabled ? boundsOverlayUAV : nullptr
		};
		context->CSSetUnorderedAccessViews(0, uavCount, uavs, nullptr);

		ID3D11ShaderResourceView* srvs[] = { hiZSRV, geometryBoundsSRV };
		context->CSSetShaderResources(0, 2, srvs);
		context->CSSetConstantBuffers(0, 1, &hiZTestParamsBuffer);

		ID3D11Buffer* frameBuffers[1]{ *globals::game::perFrame.get() };
		context->CSSetConstantBuffers(12, 1, frameBuffers);

		if (hiZSampler) {
			context->CSSetSamplers(0, 1, &hiZSampler);
		}

		// Bind the appropriate shader variant:
		// - hiZTestCS: production (lightweight, no debug overlay code compiled in)
		// - hiZTestCSDebug: debug variant with ENABLE_DEBUG_OVERLAY define
		// Fallback to production shader if debug variant isn't compiled yet
		ID3D11ComputeShader* activeCS = hiZTestCS;
		if ((settings.enableBoundsViewer || settings.debugMode) && hiZTestCSDebug) {
			activeCS = hiZTestCSDebug;
		}
		context->CSSetShader(activeCS, nullptr, 0);

		// Dispatch for batch processing with GPU timestamp profiling
		{
			const uint32_t threadGroupSize = 256;
			uint32_t numGroups = (numGeometry + threadGroupSize - 1) / threadGroupSize;

			// Try to read completed GPU timing from previous frames (async readback)
			auto& readQuery = gpuTimingQueries[gpuTimingReadIndex];
			if (readQuery.pending && readQuery.disjointQuery) {
				D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjointData = {};
				HRESULT disjointResult = context->GetData(readQuery.disjointQuery, &disjointData, sizeof(disjointData), D3D11_ASYNC_GETDATA_DONOTFLUSH);
				if (disjointResult == S_OK) {
					UINT64 beginTimestamp = 0, endTimestamp = 0;
					HRESULT beginResult = context->GetData(readQuery.beginTimestamp, &beginTimestamp, sizeof(beginTimestamp), D3D11_ASYNC_GETDATA_DONOTFLUSH);
					HRESULT endResult = context->GetData(readQuery.endTimestamp, &endTimestamp, sizeof(endTimestamp), D3D11_ASYNC_GETDATA_DONOTFLUSH);

					if (beginResult == S_OK && endResult == S_OK && !disjointData.Disjoint && disjointData.Frequency > 0) {
						// Calculate GPU time in milliseconds
						double gpuTimeMs = static_cast<double>(endTimestamp - beginTimestamp) / static_cast<double>(disjointData.Frequency) * 1000.0;
						stats.gpuCullingTimeMs = static_cast<float>(gpuTimeMs);
					}
					readQuery.pending = false;
					gpuTimingReadIndex = (gpuTimingReadIndex + 1) % GPU_TIMING_BUFFER_COUNT;
				}
			}

			// Start GPU timing for this frame's dispatch
			auto& writeQuery = gpuTimingQueries[gpuTimingWriteIndex];
			bool useGpuTiming = writeQuery.disjointQuery && writeQuery.beginTimestamp && writeQuery.endTimestamp && !writeQuery.pending;

			if (useGpuTiming) {
				context->Begin(writeQuery.disjointQuery);
				context->End(writeQuery.beginTimestamp);
			}

			context->Dispatch(numGroups, 1, 1);

			if (useGpuTiming) {
				context->End(writeQuery.endTimestamp);
				context->End(writeQuery.disjointQuery);
				writeQuery.pending = true;
				gpuTimingWriteIndex = (gpuTimingWriteIndex + 1) % GPU_TIMING_BUFFER_COUNT;
			}

			if (settings.enableBoundsViewer && boundsOverlayUAV) {
				overlayUpdatedThisFrame = true;
			}
		}

		// Unbind resources (must pass arrays of nulls)
		ID3D11ShaderResourceView* nullSRVs_tests[2] = { nullptr, nullptr };
		context->CSSetShaderResources(0, 2, nullSRVs_tests);
		ID3D11UnorderedAccessView* nullUAVs_tests[3] = { nullptr, nullptr, nullptr };
		context->CSSetUnorderedAccessViews(0, 3, nullUAVs_tests, nullptr);
		ID3D11SamplerState* nullSamplers_tests[1] = { nullptr };
		context->CSSetSamplers(0, 1, nullSamplers_tests);
		context->CSSetShader(nullptr, nullptr, 0);
	}
}

void HiZOcclusion::ProcessVisibilityResults(uint32_t bufferIndex)
{
	// Track when we received fresh results
	stats.lastResultFrame = globals::state->frameCount;
	stats.staleFrameCount = 0;

	// Read from the correct triple-buffered staging buffer
	const HiZOcclusion::OcclusionResult* visibilityData = static_cast<const HiZOcclusion::OcclusionResult*>(readbackState.mappedData[bufferIndex].pData);

	// Use the geometry snapshot that was stored with this buffer
	const auto& geometrySnapshot = readbackState.geometrySnapshots[bufferIndex];
	const uint32_t geometryCount = readbackState.geometryCount[bufferIndex];

	OcclusionTracker camera;
	OcclusionTracker shadow;
	camera.occluded.reserve(cameraOcclusion.occluded.size());
	camera.counts.reserve(cameraOcclusion.counts.size());
	shadow.occluded.reserve(shadowOcclusion.occluded.size());
	shadow.counts.reserve(shadowOcclusion.counts.size());

	for (uint32_t i = 0; i < geometryCount && i < geometrySnapshot.size(); ++i) {
		const auto& entry = geometrySnapshot[i];
		if (!entry.geometry)
			continue;
		stats.totalTested++;

		auto* geometry = entry.geometry.get();
		const bool shadowPass = entry.pass == kSunShadowPass;
		const auto& previous = shadowPass ? shadowOcclusion : cameraOcclusion;
		auto& next = shadowPass ? shadow : camera;
		bool keepState = false;
		bool occluded = false;

		switch (visibilityData[i].result) {
		case static_cast<uint32_t>(-3):  // Not culled: Test passed
			stats.visTestPassed++;
			break;
		case static_cast<uint32_t>(-2):  // Not culled: Undecidable
			stats.visInsideBounds++;
			break;
		case static_cast<uint32_t>(-1):  // Not culled: Invalid Radius
			stats.visInvalidRadius++;
			break;
		case 0u:  // default value
			stats.defaultValue++;
			keepState = true;
			break;
		case 1u:  // Outside the view
			stats.culledFrustum++;
			occluded = shadowPass;
			break;
		case 2u:  // Culled: No early out
			stats.culledNoEarlyOut++;
			occluded = settings.cullNoEarlyOut;
			break;
		default:
			keepState = true;
			break;
		}

		if (keepState) {
			if (auto it = previous.occluded.find(geometry); it != previous.occluded.end())
				next.occluded.emplace(geometry, it->second);
			if (auto it = previous.counts.find(geometry); it != previous.counts.end())
				next.counts.emplace(geometry, it->second);
			continue;
		}

		if (!occluded)
			continue;

		// Track consecutive frames for conservative culling
		uint32_t count = 1;
		if (auto it = previous.counts.find(geometry); it != previous.counts.end())
			count = it->second + 1;
		next.counts[geometry] = count;

		if (count >= settings.consecutiveOccludedThreshold)
			next.occluded.emplace(geometry, entry.geometry);
	}

	for (auto& [geometry, geo] : cameraOcclusion.occluded) {
		if (!camera.occluded.contains(geometry))
			SetOccludedFlag(geometry, false);
	}
	for (auto& [geometry, geo] : camera.occluded)
		SetOccludedFlag(geometry, true);

	cameraOcclusion = std::move(camera);
	shadowOcclusion = std::move(shadow);
	appliedShadowForward = readbackState.cameraForward[bufferIndex];

	if (settings.debugMode) {
		logger::debug(
			"Visibility results: passed={}, undecided={}, invalid={}, "
			"outside={}, occluded={}, default={}",
			stats.visTestPassed, stats.visInsideBounds, stats.visInvalidRadius,
			stats.culledFrustum, stats.culledNoEarlyOut, stats.defaultValue);
	}
}

void HiZOcclusion::SetOccludedFlag(RE::BSGeometry* geometry, bool occluded)
{
	auto& flags = geometry->GetFlags();
	const auto bits = occluded ? (flags.underlying() | kOccludedFlag) : (flags.underlying() & ~kOccludedFlag);
	flags = stl::enumeration<RE::NiAVObject::Flag, uint32_t>(static_cast<RE::NiAVObject::Flag>(bits));
}

void HiZOcclusion::ClearOcclusionState()
{
	for (auto& [geometry, geo] : cameraOcclusion.occluded)
		SetOccludedFlag(geometry, false);
	cameraOcclusion = {};
	shadowOcclusion = {};
}

void HiZOcclusion::ClearShadowOcclusionState()
{
	shadowOcclusion = {};
}

void HiZOcclusion::UpdateCameraMotion()
{
	auto* camera = RE::Main::WorldRootCamera();
	if (!camera)
		return;

	const auto& rotate = camera->world.rotate;
	const DirectX::XMFLOAT3 forward{ rotate.entry[0][0], rotate.entry[1][0], rotate.entry[2][0] };
	const auto eyePosition = Util::GetEyePosition();
	const DirectX::XMFLOAT3 position{ eyePosition.x, eyePosition.y, eyePosition.z };

	if (hasCameraHistory) {
		const float moved = Distance(position, lastEyePosition);
		frameRotation = Angle(forward, lastCameraForward);
		if (moved > kCameraCutDistance || frameRotation > kCameraCutAngle) {
			ClearOcclusionState();
			for (uint32_t i = 0; i < AsyncReadbackState::BUFFER_COUNT; ++i)
				ReleaseReadbackSlot(i);
			readbackState.readIndex = readbackState.writeIndex;
			motionMargin = 0.0f;
		} else {
			motionMargin = moved * settings.motionMarginFrames;
		}
	}

	currentCameraForward = forward;
	lastCameraForward = forward;
	lastEyePosition = position;
	hasCameraHistory = true;
}

uint8_t HiZOcclusion::GetPassKind(uint32_t renderMode)
{
	switch (renderMode) {
	case 0:
	case 12:
		return kCameraPass;
	case 14:
		return kSunShadowPass;
	default:
		return 0;
	}
}

const char* HiZOcclusion::GetRenderModeName(uint32_t renderMode)
{
	switch (renderMode) {
	case 0:
		return T(TKEY("render_mode_world"), "World");
	case 12:
		return T(TKEY("render_mode_sun_shadow_mask"), "Sun Shadow Mask");
	case 13:
		return T(TKEY("render_mode_spot_shadow_map"), "Spot Light Shadow Maps");
	case 14:
		return T(TKEY("render_mode_sun_shadow_cascades"), "Sun Shadow Cascades");
	case 15:
		return T(TKEY("render_mode_omni_shadow_map"), "Omni Light Shadow Maps");
	case 18:
		return T(TKEY("render_mode_local_map"), "Local Map");
	case 19:
		return T(TKEY("render_mode_local_map_second"), "Local Map (Second Pass)");
	case 22:
		return T(TKEY("render_mode_first_person"), "First Person");
	case 23:
		return T(TKEY("render_mode_screen_splatter"), "Screen Splatter");
	case 25:
		return T(TKEY("render_mode_cubemap_lod"), "Cubemap LOD");
	case 27:
		return T(TKEY("render_mode_decals"), "Decals");
	case 28:
		return T(TKEY("render_mode_precipitation_occlusion"), "Precipitation Occlusion");
	default:
		return T(TKEY("render_mode_unknown"), "Unknown");
	}
}

bool HiZOcclusion::IsMainViewRegistration(uint32_t renderMode, const RE::BSGraphics::BSShaderAccumulator* accum)
{
	if (renderMode == 12) {
		static REL::Relocation<RE::BSGraphics::BSShaderAccumulator**> worldShadowMaskAccumulator{ REL::ID(415008) };
		return accum == *worldShadowMaskAccumulator;
	}
	return accum->camera == RE::Main::WorldRootCamera();
}

void HiZOcclusion::CaptureSunShadowDirection(const RE::NiCamera* camera)
{
	if (!camera)
		return;
	const auto& rotate = camera->world.rotate;
	sunShadowDirection[0].store(rotate.entry[0][0], std::memory_order_relaxed);
	sunShadowDirection[1].store(rotate.entry[1][0], std::memory_order_relaxed);
	sunShadowDirection[2].store(rotate.entry[2][0], std::memory_order_relaxed);
	sunShadowDirectionCaptured.store(true, std::memory_order_release);
}

bool HiZOcclusion::IsGeometryOccluded(RE::BSGeometry* geometry) const
{
	if (!geometry)
		return false;
	return (geometry->GetFlags().underlying() & kOccludedFlag) != 0;
}

bool HiZOcclusion::IsShadowCasterOccluded(RE::BSGeometry* geometry) const
{
	return geometry && shadowOcclusion.occluded.contains(geometry);
}

bool HiZOcclusion::IsPlayerAttachedGeometry(RE::BSGeometry* geometry, RE::TESObjectREFR* refr) const
{
	auto* player = RE::PlayerCharacter::GetSingleton();
	if (!player)
		return false;

	if (refr == player || (grabbedReference && refr == grabbedReference))
		return true;

	auto* thirdPerson = player->Get3D(false);
	auto* firstPerson = player->Get3D(true);
	for (auto* node = geometry->parent; node; node = node->parent) {
		if (node == thirdPerson || node == firstPerson)
			return true;
	}
	return false;
}

#undef I18N_KEY_PREFIX
