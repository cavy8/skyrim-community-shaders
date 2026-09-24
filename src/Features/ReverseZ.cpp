#include "ReverseZ.h"

#include "Globals.h"
#include "State.h"
#include "Utils/D3D.h"
#include "Utils/Game.h"
#include "Utils/UI.h"

#include <atomic>
#include <cstring>
#include <shared_mutex>
#include <unordered_map>
#include <unordered_set>

#define I18N_KEY_PREFIX "feature.reverse_z."

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	ReverseZ::Settings,
	EnableReverseZ)

namespace
{
	constexpr RE::RENDER_TARGETS_DEPTHSTENCIL::RENDER_TARGET_DEPTHSTENCIL kReverseTargets[] = {
		RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN,
		RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN_COPY,
		RE::RENDER_TARGETS_DEPTHSTENCIL::kDECAL_OCCLUSION,
		RE::RENDER_TARGETS_DEPTHSTENCIL::kPOST_ZPREPASS_COPY,
		RE::RENDER_TARGETS_DEPTHSTENCIL::kPOST_WATER_COPY,
	};

	std::shared_mutex g_viewMutex;
	std::unordered_map<ID3D11DepthStencilView*, bool> g_viewIsReverse;

	bool ProbeReverseView(ID3D11DepthStencilView* a_view)
	{
		winrt::com_ptr<ID3D11Resource> resource;
		a_view->GetResource(resource.put());
		if (!resource)
			return false;
		winrt::com_ptr<ID3D11Texture2D> texture;
		if (FAILED(resource->QueryInterface(__uuidof(ID3D11Texture2D), texture.put_void())))
			return false;
		D3D11_TEXTURE2D_DESC desc{};
		texture->GetDesc(&desc);
		return desc.Format == DXGI_FORMAT_R32G8X24_TYPELESS;
	}

	std::mutex g_stateMutex;
	std::unordered_map<ID3D11DepthStencilState*, winrt::com_ptr<ID3D11DepthStencilState>> g_reversedStates;

	std::mutex g_rasterMutex;
	std::unordered_map<ID3D11RasterizerState*, winrt::com_ptr<ID3D11RasterizerState>> g_reversedRasterStates;

	ID3D11DepthStencilState* g_requestedState = nullptr;
	ID3D11DepthStencilState* g_boundState = nullptr;
	UINT g_requestedStencilRef = 0;
	ID3D11RasterizerState* g_requestedRasterState = nullptr;
	ID3D11RasterizerState* g_boundRasterState = nullptr;
	bool g_reverseTargetBound = false;
	bool g_projectionReversed = false;

	D3D11_VIEWPORT g_requestedViewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
	D3D11_VIEWPORT g_mappedViewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
	UINT g_requestedViewportCount = 0;

	bool ShouldFlip()
	{
		return g_reverseTargetBound && g_projectionReversed;
	}

	D3D11_VIEWPORT CanonicalViewport(const D3D11_VIEWPORT& a_viewport)
	{
		D3D11_VIEWPORT canonical = a_viewport;
		if (a_viewport.MinDepth > 0.0f && a_viewport.MinDepth != a_viewport.MaxDepth) {
			canonical.MinDepth = 0.0f;
			canonical.MaxDepth = a_viewport.MaxDepth >= 1.0f ? 1.0f - a_viewport.MinDepth : a_viewport.MaxDepth;
		}
		return canonical;
	}

	D3D11_VIEWPORT MirrorViewportDepth(const D3D11_VIEWPORT& a_viewport)
	{
		D3D11_VIEWPORT mirrored = a_viewport;
		mirrored.MinDepth = 1.0f - a_viewport.MaxDepth;
		mirrored.MaxDepth = 1.0f - a_viewport.MinDepth;
		return mirrored;
	}

	DXGI_FORMAT ToFloatDepthFormat(DXGI_FORMAT a_format)
	{
		switch (a_format) {
		case DXGI_FORMAT_R24G8_TYPELESS:
			return DXGI_FORMAT_R32G8X24_TYPELESS;
		case DXGI_FORMAT_D24_UNORM_S8_UINT:
			return DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
		case DXGI_FORMAT_R24_UNORM_X8_TYPELESS:
			return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
		case DXGI_FORMAT_X24_TYPELESS_G8_UINT:
			return DXGI_FORMAT_X32_TYPELESS_G8X24_UINT;
		default:
			return a_format;
		}
	}

	D3D11_COMPARISON_FUNC FlipComparison(D3D11_COMPARISON_FUNC a_func)
	{
		switch (a_func) {
		case D3D11_COMPARISON_LESS:
			return D3D11_COMPARISON_GREATER;
		case D3D11_COMPARISON_LESS_EQUAL:
			return D3D11_COMPARISON_GREATER_EQUAL;
		case D3D11_COMPARISON_GREATER:
			return D3D11_COMPARISON_LESS;
		case D3D11_COMPARISON_GREATER_EQUAL:
			return D3D11_COMPARISON_LESS_EQUAL;
		default:
			return a_func;
		}
	}

	std::atomic<bool> g_reversalActive{ false };

	enum class ZAxis
	{
		kUnknown,
		kRow2,
		kCol2,
	};

	ZAxis DetectZAxis(const Matrix& a_proj)
	{
		if (std::abs(a_proj.m[3][3]) > 1e-4f)
			return ZAxis::kUnknown;
		const bool col2 = std::abs(std::abs(a_proj.m[3][2]) - 1.0f) < 1e-3f;
		const bool row2 = std::abs(std::abs(a_proj.m[2][3]) - 1.0f) < 1e-3f;
		if (col2 && !row2)
			return ZAxis::kCol2;
		if (row2 && !col2)
			return ZAxis::kRow2;
		return ZAxis::kUnknown;
	}

	bool AlreadyReversed(const Matrix& a_proj, ZAxis a_axis)
	{
		const float perspective = a_axis == ZAxis::kCol2 ? a_proj.m[3][2] : a_proj.m[2][3];
		return a_proj.m[2][2] * perspective <= 0.0f;
	}

	void ReverseClipZ(Matrix& a_matrix, ZAxis a_axis)
	{
		if (a_axis == ZAxis::kCol2) {
			for (int col = 0; col < 4; ++col)
				a_matrix.m[2][col] = a_matrix.m[3][col] - a_matrix.m[2][col];
		} else {
			for (int row = 0; row < 4; ++row)
				a_matrix.m[row][2] = a_matrix.m[row][3] - a_matrix.m[row][2];
		}
	}

	void LogMatrix(const char* a_tag, const Matrix& a_proj, ZAxis a_axis)
	{
		logger::info("ReverseZ: {} z axis {}", a_tag,
			a_axis == ZAxis::kCol2 ? "m[2][*] (column-vector)" : a_axis == ZAxis::kRow2 ? "m[*][2] (row-vector)" :
																								 "UNKNOWN");
		for (int r = 0; r < 4; ++r)
			logger::info("ReverseZ: {} projMat[{}] = {:.6f} {:.6f} {:.6f} {:.6f}", a_tag, r,
				a_proj.m[r][0], a_proj.m[r][1], a_proj.m[r][2], a_proj.m[r][3]);
	}
}

std::pair<std::string, std::vector<std::string>> ReverseZ::GetFeatureSummary()
{
	return {
		T(TKEY("summary"),
			"Renders the scene with an inverted, floating-point depth buffer so that depth "
			"precision is spread evenly across the view distance instead of being concentrated "
			"near the camera. This removes most distant z-fighting on terrain, LOD and large "
			"meshes.\n\nThe main depth buffer is reallocated at a wider format, which costs "
			"additional video memory and depth bandwidth. Changing this setting requires a "
			"game restart."),
		{ T(TKEY("summary.1"), "Removes distant z-fighting and LOD flicker"),
			T(TKEY("summary.2"), "Uniform depth precision across the whole view distance"),
			T(TKEY("summary.3"), "Improves depth-based effects at long range"),
			T(TKEY("summary.4"), "Costs extra video memory and depth bandwidth") }
	};
}

void ReverseZ::LoadSettings(json& o_json)
{
	settings = o_json;
}

void ReverseZ::SaveSettings(json& o_json)
{
	o_json = settings;
}

void ReverseZ::RestoreDefaultSettings()
{
	settings = {};
}

std::span<const Util::Settings::RestartFieldInfo> ReverseZ::GetRestartRequiredFields() const
{
	static constexpr Util::Settings::RestartFieldInfo fields[] = {
		{ "EnableReverseZ", "Reverse Z-Buffer", offsetof(Settings, EnableReverseZ), sizeof(bool) },
	};
	return fields;
}

void ReverseZ::LatchBootState()
{
	if (bootLatched)
		return;
	bootLatched = true;
	activeThisBoot = loaded && settings.EnableReverseZ && !REL::Module::IsVR();
	logger::info("ReverseZ: {}", activeThisBoot ? "enabled" : "disabled");
}

bool ReverseZ::HasShaderDefine(RE::BSShader::Type)
{
	LatchBootState();
	return activeThisBoot;
}

void ReverseZ::DataLoaded()
{
	LatchBootState();
}

bool ReverseZ::ValidateCache(CSimpleIniA& a_ini)
{
	if (!Feature::ValidateCache(a_ini))
		return false;

	LatchBootState();
	const bool cached = a_ini.GetBoolValue(GetShortName().c_str(), "ReverseZActive", false);
	if (cached != activeThisBoot) {
		logger::info("Reverse Z state changed; invalidating shader cache");
		return false;
	}
	if (a_ini.GetLongValue(GetShortName().c_str(), "UtilityDefines", 0) != kUtilityDefinesRevision) {
		logger::info("Reverse Z utility shader defines changed; invalidating shader cache");
		return false;
	}
	return true;
}

void ReverseZ::WriteDiskCacheInfo(CSimpleIniA& a_ini)
{
	Feature::WriteDiskCacheInfo(a_ini);
	LatchBootState();
	a_ini.SetBoolValue(GetShortName().c_str(), "ReverseZActive", activeThisBoot);
	a_ini.SetLongValue(GetShortName().c_str(), "UtilityDefines", kUtilityDefinesRevision);
}

void ReverseZ::SetupDepthTargets()
{
	LatchBootState();
	if (!activeThisBoot)
		return;

	auto* renderer = globals::game::renderer;
	auto* device = globals::d3d::device;
	if (!renderer || !device)
		return;

	{
		std::unique_lock lock(g_viewMutex);
		g_viewIsReverse.clear();
	}

	convertedTargetMask = 0;
	size_t converted = 0;
	for (auto target : kReverseTargets) {
		auto& data = renderer->GetDepthStencilData().depthStencils[target];
		if (!data.texture) {
			logger::debug("ReverseZ: depth target {} is not allocated on this runtime", static_cast<int>(target));
			continue;
		}

		D3D11_TEXTURE2D_DESC texDesc{};
		data.texture->GetDesc(&texDesc);
		if (texDesc.Format != DXGI_FORMAT_R24G8_TYPELESS) {
			logger::warn("ReverseZ: depth target {} has format {}, not converting", static_cast<int>(target), static_cast<int>(texDesc.Format));
			continue;
		}

		texDesc.Format = ToFloatDepthFormat(texDesc.Format);

		winrt::com_ptr<ID3D11Texture2D> newTexture;
		if (FAILED(device->CreateTexture2D(&texDesc, nullptr, newTexture.put()))) {
			logger::error("ReverseZ: failed to allocate floating-point depth for target {}", static_cast<int>(target));
			continue;
		}
		Util::SetResourceName(newTexture.get(), "ReverseZ::Depth%d", static_cast<int>(target));

		bool viewsOk = true;
		std::vector<winrt::com_ptr<ID3D11DepthStencilView>> newViews(8);
		std::vector<winrt::com_ptr<ID3D11DepthStencilView>> newReadOnlyViews(8);
		winrt::com_ptr<ID3D11ShaderResourceView> newDepthSRV;
		winrt::com_ptr<ID3D11ShaderResourceView> newStencilSRV;

		for (size_t i = 0; i < 8 && viewsOk; ++i) {
			if (!data.views[i])
				continue;
			D3D11_DEPTH_STENCIL_VIEW_DESC desc{};
			data.views[i]->GetDesc(&desc);
			desc.Format = ToFloatDepthFormat(desc.Format);
			viewsOk = SUCCEEDED(device->CreateDepthStencilView(newTexture.get(), &desc, newViews[i].put()));
		}
		for (size_t i = 0; i < 8 && viewsOk; ++i) {
			if (!data.readOnlyViews[i])
				continue;
			D3D11_DEPTH_STENCIL_VIEW_DESC desc{};
			data.readOnlyViews[i]->GetDesc(&desc);
			desc.Format = ToFloatDepthFormat(desc.Format);
			viewsOk = SUCCEEDED(device->CreateDepthStencilView(newTexture.get(), &desc, newReadOnlyViews[i].put()));
		}
		if (viewsOk && data.depthSRV) {
			D3D11_SHADER_RESOURCE_VIEW_DESC desc{};
			data.depthSRV->GetDesc(&desc);
			desc.Format = ToFloatDepthFormat(desc.Format);
			viewsOk = SUCCEEDED(device->CreateShaderResourceView(newTexture.get(), &desc, newDepthSRV.put()));
		}
		if (viewsOk && data.stencilSRV) {
			D3D11_SHADER_RESOURCE_VIEW_DESC desc{};
			data.stencilSRV->GetDesc(&desc);
			desc.Format = ToFloatDepthFormat(desc.Format);
			viewsOk = SUCCEEDED(device->CreateShaderResourceView(newTexture.get(), &desc, newStencilSRV.put()));
		}

		if (!viewsOk) {
			logger::error("ReverseZ: failed to rebuild views for depth target {}; leaving it unconverted", static_cast<int>(target));
			continue;
		}

		for (size_t i = 0; i < 8; ++i) {
			if (data.views[i]) {
				data.views[i]->Release();
				data.views[i] = newViews[i].detach();
				Util::SetResourceName(data.views[i], "ReverseZ::Depth%d DSV%zu", static_cast<int>(target), i);
			}
			if (data.readOnlyViews[i]) {
				data.readOnlyViews[i]->Release();
				data.readOnlyViews[i] = newReadOnlyViews[i].detach();
				Util::SetResourceName(data.readOnlyViews[i], "ReverseZ::Depth%d RODSV%zu", static_cast<int>(target), i);
			}
		}
		if (data.depthSRV) {
			data.depthSRV->Release();
			data.depthSRV = newDepthSRV.detach();
			Util::SetResourceName(data.depthSRV, "ReverseZ::Depth%d SRV", static_cast<int>(target));
		}
		if (data.stencilSRV) {
			data.stencilSRV->Release();
			data.stencilSRV = newStencilSRV.detach();
			Util::SetResourceName(data.stencilSRV, "ReverseZ::Depth%d StencilSRV", static_cast<int>(target));
		}
		data.texture->Release();
		data.texture = newTexture.detach();

		convertedTargetMask |= 1u << static_cast<uint32_t>(target);
		++converted;
	}

	depthTargetsConverted = converted > 0;
	logger::info("ReverseZ: converted {} depth targets to D32_FLOAT_S8X24_UINT", converted);
	InstallRuntimeHooks();
}

bool ReverseZ::IsReverseDepthView(ID3D11DepthStencilView* a_view) const
{
	if (!a_view || !g_reversalActive.load(std::memory_order_relaxed))
		return false;
	{
		std::shared_lock lock(g_viewMutex);
		if (auto it = g_viewIsReverse.find(a_view); it != g_viewIsReverse.end())
			return it->second;
	}
	const bool reverse = ProbeReverseView(a_view);
	std::unique_lock lock(g_viewMutex);
	g_viewIsReverse.insert_or_assign(a_view, reverse);
	return reverse;
}

ID3D11DepthStencilView* ReverseZ::ResolveCubemapFaceDepthView(ID3D11RenderTargetView* a_renderTarget, ID3D11DepthStencilView* a_depthView) const
{
	if (!a_renderTarget || !a_depthView || !IsReverseDepthView(a_depthView))
		return a_depthView;
	auto* renderer = globals::game::renderer;
	if (!renderer)
		return a_depthView;
	const auto& cubemap = renderer->GetRendererData().cubemapRenderTargets[RE::RENDER_TARGETS_CUBEMAP::kREFLECTIONS];
	for (auto* faceView : cubemap.cubeSideRTV) {
		if (faceView == a_renderTarget) {
			auto* cubemapDepth = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kCUBEMAP_REFLECTIONS].views[0];
			return cubemapDepth ? cubemapDepth : a_depthView;
		}
	}
	return a_depthView;
}

ID3D11RasterizerState* ReverseZ::GetReversedRasterizerState(ID3D11RasterizerState* a_state)
{
	if (!a_state)
		return nullptr;

	std::scoped_lock lock(g_rasterMutex);
	if (auto it = g_reversedRasterStates.find(a_state); it != g_reversedRasterStates.end())
		return it->second ? it->second.get() : a_state;

	D3D11_RASTERIZER_DESC desc{};
	a_state->GetDesc(&desc);
	if (desc.DepthBias == 0 && desc.SlopeScaledDepthBias == 0.0f && desc.DepthBiasClamp == 0.0f) {
		g_reversedRasterStates.insert_or_assign(a_state, nullptr);
		return a_state;
	}
	desc.DepthBias = -desc.DepthBias;
	desc.SlopeScaledDepthBias = -desc.SlopeScaledDepthBias;
	desc.DepthBiasClamp = -desc.DepthBiasClamp;

	winrt::com_ptr<ID3D11RasterizerState> reversed;
	if (FAILED(globals::d3d::device->CreateRasterizerState(&desc, reversed.put()))) {
		g_reversedRasterStates.insert_or_assign(a_state, nullptr);
		return a_state;
	}
	Util::SetResourceName(reversed.get(), "ReverseZ::RasterizerState");
	auto* raw = reversed.get();
	g_reversedRasterStates.insert_or_assign(a_state, std::move(reversed));
	return raw;
}

ID3D11DepthStencilState* ReverseZ::GetReversedState(ID3D11DepthStencilState* a_state)
{
	if (!a_state)
		return nullptr;

	std::scoped_lock lock(g_stateMutex);
	if (auto it = g_reversedStates.find(a_state); it != g_reversedStates.end())
		return it->second ? it->second.get() : a_state;

	D3D11_DEPTH_STENCIL_DESC desc{};
	a_state->GetDesc(&desc);
	desc.DepthFunc = FlipComparison(desc.DepthFunc);

	winrt::com_ptr<ID3D11DepthStencilState> reversed;
	if (FAILED(globals::d3d::device->CreateDepthStencilState(&desc, reversed.put()))) {
		g_reversedStates.insert_or_assign(a_state, nullptr);
		return a_state;
	}
	Util::SetResourceName(reversed.get(), "ReverseZ::DepthStencilState");
	auto* raw = reversed.get();
	g_reversedStates.insert_or_assign(a_state, std::move(reversed));
	return raw;
}

void ReverseZ::ApplyReverseProjection(void* a_cameraStateEntry, const RE::NiCamera* a_camera)
{
	if (!activeThisBoot || !a_cameraStateEntry || !a_camera)
		return;

	constexpr std::ptrdiff_t kViewDataOffset = 0x10;
	auto& viewData = *reinterpret_cast<RE::BSGraphics::ViewData*>(static_cast<std::byte*>(a_cameraStateEntry) + kViewDataOffset);

	const auto axis = DetectZAxis(viewData.projMat);
	const bool cameraMatched = a_camera == RE::Main::WorldRootCamera();

	if (!cameraMatched || axis == ZAxis::kUnknown || AlreadyReversed(viewData.projMat, axis))
		return;

	const bool first = !g_reversalActive.exchange(true);
	if (first) {
		LogMatrix("before", viewData.projMat, axis);
		if (axis == ZAxis::kRow2) {
			const float m22 = viewData.projMat.m[2][2];
			const float m32 = viewData.projMat.m[3][2];
			const auto cam = Util::GetCameraData();
			logger::info("ReverseZ: matrix near/far = {:.3f}/{:.3f}, CameraData near/far = {:.3f}/{:.3f}",
				-m32 / m22, -m32 / (m22 - 1.0f), cam.y, cam.x);
		}
	}

	ReverseClipZ(viewData.projMat, axis);
	ReverseClipZ(viewData.projMatrixUnjittered, axis);
	ReverseClipZ(viewData.viewProjMat, axis);
	ReverseClipZ(viewData.viewProjMatrixUnjittered, axis);

	if (first)
		LogMatrix("after", viewData.projMat, axis);
}

bool ReverseZ::IsConvertedDepthTarget(uint32_t a_target) const
{
	return a_target < 32 && ((convertedTargetMask >> a_target) & 1u) != 0;
}

bool ReverseZ::ExpectPublishedReversal(const RE::NiCamera* a_camera, bool a_renderingCubemap) const
{
	if (!activeThisBoot || !a_camera || !g_reversalActive.load(std::memory_order_relaxed))
		return false;
	if (a_camera == RE::Main::WorldRootCamera())
		return true;
	if (a_renderingCubemap)
		return false;
	auto* shadowState = globals::game::shadowState;
	if (!shadowState || !IsConvertedDepthTarget(shadowState->GetRuntimeData().depthStencil))
		return false;
	return !a_camera->GetRuntimeData2().viewFrustum.bOrtho;
}

void ReverseZ::ReversePublishedProjection()
{
	if (!activeThisBoot || !g_reversalActive.load(std::memory_order_relaxed))
		return;

	auto* shadowState = globals::game::shadowState;
	if (!shadowState)
		return;

	auto& runtimeData = shadowState->GetRuntimeData();
	if (!IsConvertedDepthTarget(runtimeData.depthStencil))
		return;

	auto& viewData = *reinterpret_cast<RE::BSGraphics::ViewData*>(&runtimeData.cameraData);
	const auto axis = DetectZAxis(viewData.projMat);
	if (axis == ZAxis::kUnknown || AlreadyReversed(viewData.projMat, axis))
		return;

	static bool logged = false;
	if (!logged) {
		logged = true;
		logger::info("ReverseZ: reversing a non-world camera published for depth target {}", runtimeData.depthStencil);
		LogMatrix("published before", viewData.projMat, axis);
	}

	ReverseClipZ(viewData.projMat, axis);
	ReverseClipZ(viewData.projMatrixUnjittered, axis);
	ReverseClipZ(viewData.viewProjMat, axis);
	ReverseClipZ(viewData.viewProjMatrixUnjittered, axis);
}

namespace
{
	void ReverseUploadedProjection(Matrix& a_matrix)
	{
		for (int col = 0; col < 4; ++col)
			a_matrix.m[2][col] = a_matrix.m[3][col] - a_matrix.m[2][col];
	}

	void ReverseUploadedInverse(Matrix& a_matrix)
	{
		for (int row = 0; row < 4; ++row) {
			const float z = a_matrix.m[row][2];
			a_matrix.m[row][2] = -z;
			a_matrix.m[row][3] += z;
		}
	}
}

void ReverseZ::FixupMappedFrameBuffer(globals::FrameBuffer& a_frameBuffer)
{
	if (!activeThisBoot || !g_reversalActive.load(std::memory_order_relaxed) || !g_projectionReversed)
		return;

	const auto& proj = a_frameBuffer.CameraProj;
	const bool perspective = std::abs(proj.m[3][3]) < 1e-4f && std::abs(std::abs(proj.m[3][2]) - 1.0f) < 1e-3f;
	if (!perspective || proj.m[2][2] * proj.m[3][2] < 0.0f)
		return;

	static bool logged = false;
	if (!logged) {
		logged = true;
		logger::info("ReverseZ: reversing an uploaded standard projection (near {:.3f}) for a reversed pass", -proj.m[2][3] / proj.m[2][2]);
	}

	ReverseUploadedProjection(a_frameBuffer.CameraProj);
	ReverseUploadedProjection(a_frameBuffer.CameraViewProj);
	ReverseUploadedProjection(a_frameBuffer.CameraViewProjUnjittered);
	ReverseUploadedProjection(a_frameBuffer.CameraProjUnjittered);
	ReverseUploadedInverse(a_frameBuffer.CameraProjUnjitteredInverse);
	ReverseUploadedInverse(a_frameBuffer.CameraViewProjInverse);
	ReverseUploadedInverse(a_frameBuffer.CameraProjInverse);
}

void ReverseZ::TracePublishedCamera(const RE::NiCamera* a_camera)
{
	static std::unordered_set<uint64_t> seen;
	constexpr size_t kMaxTraced = 64;
	if (!activeThisBoot || seen.size() >= kMaxTraced)
		return;

	auto* shadowState = globals::game::shadowState;
	if (!shadowState)
		return;

	auto& runtimeData = shadowState->GetRuntimeData();
	const auto& viewData = *reinterpret_cast<const RE::BSGraphics::ViewData*>(&runtimeData.cameraData);
	const auto axis = DetectZAxis(viewData.projMat);
	const char* convention = axis == ZAxis::kUnknown ? "orthographic" : AlreadyReversed(viewData.projMat, axis) ? "reversed" : "standard";

	const uint64_t key = reinterpret_cast<uint64_t>(a_camera) ^ (static_cast<uint64_t>(runtimeData.depthStencil & 0xFF) << 56) ^ (static_cast<uint64_t>(axis) << 48);
	if (!seen.insert(key).second)
		return;

	logger::info("ReverseZ: camera {}{} publishes {} projection with depth target {}{}",
		static_cast<const void*>(a_camera),
		a_camera == RE::Main::WorldRootCamera() ? " (world root)" : "",
		convention,
		static_cast<int32_t>(runtimeData.depthStencil),
		IsConvertedDepthTarget(runtimeData.depthStencil) ? " (float)" : "");
}

namespace
{
	struct BSGraphics_State_BuildCameraStateData
	{
		static void thunk(RE::BSGraphics::State* a_state, void* a_entry, RE::NiCamera* a_camera, char a_jitter)
		{
			func(a_state, a_entry, a_camera, a_jitter);
			globals::features::reverseZ.ApplyReverseProjection(a_entry, a_camera);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct ID3D11DeviceContext_OMSetDepthStencilState
	{
		static void thunk(ID3D11DeviceContext* This, ID3D11DepthStencilState* pDepthStencilState, UINT StencilRef)
		{
			if (This == globals::d3d::context) {
				g_requestedState = pDepthStencilState;
				g_requestedStencilRef = StencilRef;
				g_boundState = ShouldFlip() ? globals::features::reverseZ.GetReversedState(pDepthStencilState) : pDepthStencilState;
				func(This, g_boundState, StencilRef);
				return;
			}
			func(This, pDepthStencilState, StencilRef);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct ID3D11DeviceContext_RSSetState
	{
		static void thunk(ID3D11DeviceContext* This, ID3D11RasterizerState* pRasterizerState)
		{
			if (This == globals::d3d::context) {
				g_requestedRasterState = pRasterizerState;
				g_boundRasterState = ShouldFlip() ? globals::features::reverseZ.GetReversedRasterizerState(pRasterizerState) : pRasterizerState;
				func(This, g_boundRasterState);
				return;
			}
			func(This, pRasterizerState);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct ID3D11DeviceContext_RSSetViewports
	{
		static void thunk(ID3D11DeviceContext* This, UINT NumViewports, const D3D11_VIEWPORT* pViewports)
		{
			if (This != globals::d3d::context || !pViewports || NumViewports == 0 || NumViewports > D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE) {
				func(This, NumViewports, pViewports);
				return;
			}
			D3D11_VIEWPORT requested[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
			D3D11_VIEWPORT mapped[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
			const bool flip = ShouldFlip();
			for (UINT i = 0; i < NumViewports; ++i) {
				requested[i] = CanonicalViewport(pViewports[i]);
				mapped[i] = flip ? MirrorViewportDepth(requested[i]) : requested[i];
			}
			std::memcpy(g_requestedViewports, requested, sizeof(D3D11_VIEWPORT) * NumViewports);
			std::memcpy(g_mappedViewports, mapped, sizeof(D3D11_VIEWPORT) * NumViewports);
			g_requestedViewportCount = NumViewports;
			func(This, NumViewports, mapped);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	void ReapplyMappedStates(ID3D11DeviceContext* This)
	{
		const bool flip = ShouldFlip();
		if (g_requestedState) {
			g_boundState = flip ? globals::features::reverseZ.GetReversedState(g_requestedState) : g_requestedState;
			ID3D11DeviceContext_OMSetDepthStencilState::func(This, g_boundState, g_requestedStencilRef);
		}
		if (g_requestedRasterState) {
			g_boundRasterState = flip ? globals::features::reverseZ.GetReversedRasterizerState(g_requestedRasterState) : g_requestedRasterState;
			ID3D11DeviceContext_RSSetState::func(This, g_boundRasterState);
		}
		if (g_requestedViewportCount > 0) {
			for (UINT i = 0; i < g_requestedViewportCount; ++i)
				g_mappedViewports[i] = flip ? MirrorViewportDepth(g_requestedViewports[i]) : g_requestedViewports[i];
			ID3D11DeviceContext_RSSetViewports::func(This, g_requestedViewportCount, g_mappedViewports);
		}
	}

	void UpdateBoundDepthTarget(ID3D11DeviceContext* This, ID3D11DepthStencilView* pDepthStencilView)
	{
		if (This != globals::d3d::context)
			return;
		const bool reverseBound = globals::features::reverseZ.IsReverseDepthView(pDepthStencilView);
		if (reverseBound == g_reverseTargetBound)
			return;
		g_reverseTargetBound = reverseBound;
		ReapplyMappedStates(This);
	}

	bool PublishedProjectionIsReversed()
	{
		auto* shadowState = globals::game::shadowState;
		if (!shadowState)
			return false;
		const auto& proj = shadowState->GetRuntimeData().cameraData.getEye().projMat;
		const auto axis = DetectZAxis(proj);
		return axis != ZAxis::kUnknown && AlreadyReversed(proj, axis);
	}

	void SetProjectionReversed(bool a_reversed)
	{
		if (a_reversed == g_projectionReversed)
			return;
		g_projectionReversed = a_reversed;
		if (auto* context = globals::d3d::context)
			ReapplyMappedStates(context);
	}

	struct BSGraphics_State_SetCameraData
	{
		static void thunk(RE::BSGraphics::State* a_state, const RE::NiCamera* a_camera, std::uint32_t a_flags)
		{
			const bool renderingCubemap = globals::state && (globals::state->permutationData.ExtraShaderDescriptor & static_cast<uint32_t>(State::ExtraShaderDescriptors::IsReflections)) != 0;
			SetProjectionReversed(globals::features::reverseZ.ExpectPublishedReversal(a_camera, renderingCubemap));
			func(a_state, a_camera, a_flags);
			if (!renderingCubemap)
				globals::features::reverseZ.ReversePublishedProjection();
			globals::features::reverseZ.TracePublishedCamera(a_camera);
			SetProjectionReversed(g_reversalActive.load(std::memory_order_relaxed) && PublishedProjectionIsReversed());
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct ID3D11DeviceContext_ClearDepthStencilView
	{
		static void thunk(ID3D11DeviceContext* This, ID3D11DepthStencilView* pDepthStencilView, UINT ClearFlags, FLOAT Depth, UINT8 Stencil)
		{
			if (This == globals::d3d::context && g_projectionReversed && globals::features::reverseZ.IsReverseDepthView(pDepthStencilView))
				Depth = 1.0f - Depth;
			func(This, pDepthStencilView, ClearFlags, Depth, Stencil);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	ID3D11RenderTargetView* g_lastClearedCubeFace = nullptr;

	ID3D11DepthStencilView* ResolveFaceDepth(ID3D11DeviceContext* This, ID3D11RenderTargetView* a_renderTarget, ID3D11DepthStencilView* a_depthView)
	{
		auto* resolved = globals::features::reverseZ.ResolveCubemapFaceDepthView(a_renderTarget, a_depthView);
		if (resolved == a_depthView) {
			if (a_renderTarget)
				g_lastClearedCubeFace = nullptr;
			return a_depthView;
		}
		if (a_renderTarget != g_lastClearedCubeFace) {
			g_lastClearedCubeFace = a_renderTarget;
			ID3D11DeviceContext_ClearDepthStencilView::func(This, resolved, D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);
		}
		return resolved;
	}

	struct ID3D11DeviceContext_OMSetRenderTargets
	{
		static void thunk(ID3D11DeviceContext* This, UINT NumViews, ID3D11RenderTargetView* const* ppRenderTargetViews, ID3D11DepthStencilView* pDepthStencilView)
		{
			if (This == globals::d3d::context && NumViews > 0 && ppRenderTargetViews)
				pDepthStencilView = ResolveFaceDepth(This, ppRenderTargetViews[0], pDepthStencilView);
			func(This, NumViews, ppRenderTargetViews, pDepthStencilView);
			UpdateBoundDepthTarget(This, pDepthStencilView);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct ID3D11DeviceContext_OMSetRenderTargetsAndUnorderedAccessViews
	{
		static void thunk(ID3D11DeviceContext* This, UINT NumRTVs, ID3D11RenderTargetView* const* ppRenderTargetViews, ID3D11DepthStencilView* pDepthStencilView, UINT UAVStartSlot, UINT NumUAVs, ID3D11UnorderedAccessView* const* ppUnorderedAccessViews, const UINT* pUAVInitialCounts)
		{
			if (This == globals::d3d::context && NumRTVs != D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL && NumRTVs > 0 && ppRenderTargetViews)
				pDepthStencilView = ResolveFaceDepth(This, ppRenderTargetViews[0], pDepthStencilView);
			func(This, NumRTVs, ppRenderTargetViews, pDepthStencilView, UAVStartSlot, NumUAVs, ppUnorderedAccessViews, pUAVInitialCounts);
			if (NumRTVs != D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL)
				UpdateBoundDepthTarget(This, pDepthStencilView);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct ID3D11DeviceContext_OMGetDepthStencilState
	{
		static void thunk(ID3D11DeviceContext* This, ID3D11DepthStencilState** ppDepthStencilState, UINT* pStencilRef)
		{
			func(This, ppDepthStencilState, pStencilRef);
			if (This != globals::d3d::context || !ppDepthStencilState || !*ppDepthStencilState)
				return;
			if (*ppDepthStencilState != g_boundState || !g_requestedState || g_requestedState == g_boundState)
				return;
			g_requestedState->AddRef();
			(*ppDepthStencilState)->Release();
			*ppDepthStencilState = g_requestedState;
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct ID3D11DeviceContext_RSGetState
	{
		static void thunk(ID3D11DeviceContext* This, ID3D11RasterizerState** ppRasterizerState)
		{
			func(This, ppRasterizerState);
			if (This != globals::d3d::context || !ppRasterizerState || !*ppRasterizerState)
				return;
			if (*ppRasterizerState != g_boundRasterState || !g_requestedRasterState || g_requestedRasterState == g_boundRasterState)
				return;
			g_requestedRasterState->AddRef();
			(*ppRasterizerState)->Release();
			*ppRasterizerState = g_requestedRasterState;
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct ID3D11DeviceContext_RSGetViewports
	{
		static void thunk(ID3D11DeviceContext* This, UINT* pNumViewports, D3D11_VIEWPORT* pViewports)
		{
			func(This, pNumViewports, pViewports);
			if (This != globals::d3d::context || !pNumViewports || !pViewports)
				return;
			const UINT count = std::min<UINT>(*pNumViewports, D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE);
			for (UINT i = 0; i < count; ++i)
				pViewports[i] = CanonicalViewport(pViewports[i]);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

}

void ReverseZ::InstallRuntimeHooks()
{
	if (runtimeHooksInstalled || !activeThisBoot)
		return;
	auto* context = globals::d3d::context;
	if (!context)
		return;
	runtimeHooksInstalled = true;

	stl::detour_vfunc<33, ID3D11DeviceContext_OMSetRenderTargets>(context);
	stl::detour_vfunc<34, ID3D11DeviceContext_OMSetRenderTargetsAndUnorderedAccessViews>(context);
	stl::detour_vfunc<36, ID3D11DeviceContext_OMSetDepthStencilState>(context);
	stl::detour_vfunc<43, ID3D11DeviceContext_RSSetState>(context);
	stl::detour_vfunc<44, ID3D11DeviceContext_RSSetViewports>(context);
	stl::detour_vfunc<53, ID3D11DeviceContext_ClearDepthStencilView>(context);
	stl::detour_vfunc<92, ID3D11DeviceContext_OMGetDepthStencilState>(context);
	stl::detour_vfunc<94, ID3D11DeviceContext_RSGetState>(context);
	stl::detour_vfunc<95, ID3D11DeviceContext_RSGetViewports>(context);

	logger::info("ReverseZ: installed depth-state, depth-clear and state read-back hooks");
}

void ReverseZ::PostPostLoad()
{
	LatchBootState();
	if (!activeThisBoot)
		return;
	stl::detour_thunk<BSGraphics_State_BuildCameraStateData>(REL::RelocationID(75711, 77520));
	stl::detour_thunk<BSGraphics_State_SetCameraData>(REL::RelocationID(75694, 77503));
	logger::info("ReverseZ: installed camera state build and publish hooks");
}

void ReverseZ::DrawSettings()
{
	ImGui::Checkbox(T(TKEY("enabled"), "Enable Reverse Z-Buffer"), &settings.EnableReverseZ);
	Util::HelpMarker(T(TKEY("enabled_tooltip"),
		"Inverts the depth range so precision is spread evenly over the view distance.\n"
		"Requires a game restart."));

	if (settings.EnableReverseZ != activeThisBoot)
		ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.2f, 1.0f), "%s", T(TKEY("restart_required"), "Restart the game to apply."));

	if (activeThisBoot && !depthTargetsConverted)
		ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%s", T(TKEY("status_failed"), "The depth buffer could not be reallocated; see the log."));
}

#undef I18N_KEY_PREFIX
