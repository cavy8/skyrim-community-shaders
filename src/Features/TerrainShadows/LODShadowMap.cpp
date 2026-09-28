#include "LODShadowMap.h"

#include <cmath>
#include <cstring>

#include "Features/ReverseZ.h"
#include "ShaderCache.h"
#include "State.h"
#include "Util.h"

namespace
{
	constexpr double kCascadeRadius[LODShadowMap::kCascadeCount] = { 16384.0, 49152.0, 163840.0 };
	constexpr double kDepthHalfRange = 400000.0;
	constexpr double kConstantBias = 32.0;
	constexpr float kRecaptureAngle = 0.2f;
	constexpr float kFadeStartAngle = 1.0f;
	constexpr float kFadeEndAngle = 3.0f;
	constexpr float kRecaptureDistance = 2048.0f;
	constexpr auto kRefreshInterval = std::chrono::seconds(10);
	constexpr auto kCaptureTimeout = std::chrono::seconds(3);
	constexpr uint32_t kAllFaces = 0x3F;

	LODShadowMap* g_activeCapture = nullptr;

	struct ID3D11DeviceContext_DrawIndexed
	{
		static void thunk(ID3D11DeviceContext* This, UINT IndexCount, UINT StartIndexLocation, INT BaseVertexLocation)
		{
			func(This, IndexCount, StartIndexLocation, BaseVertexLocation);
			if (auto* capture = g_activeCapture; capture && This == globals::d3d::context && capture->BeginCaptureDraw(This)) {
				func(This, IndexCount, StartIndexLocation, BaseVertexLocation);
				capture->EndCaptureDraw(This);
			}
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct ID3D11DeviceContext_DrawIndexedInstanced
	{
		static void thunk(ID3D11DeviceContext* This, UINT IndexCountPerInstance, UINT InstanceCount, UINT StartIndexLocation, INT BaseVertexLocation, UINT StartInstanceLocation)
		{
			func(This, IndexCountPerInstance, InstanceCount, StartIndexLocation, BaseVertexLocation, StartInstanceLocation);
			if (auto* capture = g_activeCapture; capture && This == globals::d3d::context && capture->BeginCaptureDraw(This)) {
				func(This, IndexCountPerInstance, InstanceCount, StartIndexLocation, BaseVertexLocation, StartInstanceLocation);
				capture->EndCaptureDraw(This);
			}
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	bool GetLightDirection(RE::NiPoint3& a_direction)
	{
		auto* shaderManager = globals::game::smState;
		if (!shaderManager || !shaderManager->shadowSceneNode[0])
			return false;
		auto* sunLight = shaderManager->shadowSceneNode[0]->GetRuntimeData().sunLight;
		if (!sunLight)
			return false;
		auto* light = skyrim_cast<RE::NiDirectionalLight*>(sunLight->light.get());
		if (!light)
			return false;
		a_direction = light->GetWorldDirection();
		if (a_direction.z > 0.0f)
			a_direction = -a_direction;
		const float length = a_direction.Length();
		if (length < 1e-4f)
			return false;
		a_direction /= length;
		return true;
	}

	RE::TESWorldSpace* GetWorldSpace()
	{
		auto* tes = globals::game::tes;
		return tes ? tes->GetRuntimeData2().worldSpace : nullptr;
	}

	float AngleDegrees(const RE::NiPoint3& a_lhs, const RE::NiPoint3& a_rhs)
	{
		return std::acos(std::clamp(a_lhs.Dot(a_rhs), -1.0f, 1.0f)) * (180.0f / RE::NI_PI);
	}

	double Dot(const double (&a_lhs)[3], const RE::NiPoint3& a_rhs)
	{
		return a_lhs[0] * a_rhs.x + a_lhs[1] * a_rhs.y + a_lhs[2] * a_rhs.z;
	}
}

void LODShadowMap::InstallHooks()
{
	static bool installed = false;
	if (installed || !globals::d3d::context)
		return;
	installed = true;
	stl::detour_vfunc<12, ID3D11DeviceContext_DrawIndexed>(globals::d3d::context);
	stl::detour_vfunc<20, ID3D11DeviceContext_DrawIndexedInstanced>(globals::d3d::context);
	logger::info("[Terrain Shadows] Installed LOD shadow capture draw hooks");
}

void LODShadowMap::CompileShaders()
{
	if (shadersFailed || (captureGS && capturePS && captureAlphaPS))
		return;
	if (!captureGS)
		captureGS.attach(reinterpret_cast<ID3D11GeometryShader*>(Util::CompileShader(L"Data\\Shaders\\TerrainShadows\\LODShadowCapture.hlsl", {}, "gs_5_0")));
	if (!capturePS)
		capturePS.attach(reinterpret_cast<ID3D11PixelShader*>(Util::CompileShader(L"Data\\Shaders\\TerrainShadows\\LODShadowCapture.hlsl", {}, "ps_5_0")));
	if (!captureAlphaPS)
		captureAlphaPS.attach(reinterpret_cast<ID3D11PixelShader*>(Util::CompileShader(L"Data\\Shaders\\TerrainShadows\\LODShadowCapture.hlsl", { { "ALPHA_TEST", "" } }, "ps_5_0")));
	if (!captureGS || !capturePS || !captureAlphaPS) {
		shadersFailed = true;
		logger::error("[Terrain Shadows] Failed to compile the LOD shadow capture shaders");
	}
}

void LODShadowMap::ClearShaderCache()
{
	shadersFailed = false;
	captureGS = nullptr;
	capturePS = nullptr;
	captureAlphaPS = nullptr;
}

void LODShadowMap::ReleaseTargets()
{
	if (auto* context = globals::d3d::context) {
		ID3D11ShaderResourceView* nullView = nullptr;
		context->PSSetShaderResources(61, 1, &nullView);
		context->CSSetShaderResources(61, 1, &nullView);
	}
	targets[0].reset();
	targets[1].reset();
	resolution = 0;
	publishedValid = false;
	capturing = false;
}

bool LODShadowMap::EnsureResources(uint32_t a_resolution)
{
	CompileShaders();
	if (!captureGS || !capturePS || !captureAlphaPS)
		return false;

	auto* device = globals::d3d::device;

	if (!captureCB)
		captureCB = std::make_unique<ConstantBuffer>(ConstantBufferDesc<CaptureCB>(), "TerrainShadows::LODShadowCaptureCB");

	if (!depthState) {
		D3D11_DEPTH_STENCIL_DESC desc{};
		desc.DepthEnable = TRUE;
		desc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
		desc.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
		if (FAILED(device->CreateDepthStencilState(&desc, depthState.put())))
			return false;
		Util::SetResourceName(depthState.get(), "TerrainShadows::LODShadowDepthState");
	}

	if (!rasterState) {
		D3D11_RASTERIZER_DESC desc{};
		desc.FillMode = D3D11_FILL_SOLID;
		desc.CullMode = D3D11_CULL_NONE;
		desc.DepthClipEnable = FALSE;
		if (FAILED(device->CreateRasterizerState(&desc, rasterState.put())))
			return false;
		Util::SetResourceName(rasterState.get(), "TerrainShadows::LODShadowRasterState");
	}

	if (targets[0] && targets[1] && resolution == a_resolution)
		return true;

	ReleaseTargets();

	D3D11_TEXTURE2D_DESC textureDesc{};
	textureDesc.Width = a_resolution;
	textureDesc.Height = a_resolution;
	textureDesc.MipLevels = 1;
	textureDesc.ArraySize = kCascadeCount;
	textureDesc.Format = DXGI_FORMAT_R16_TYPELESS;
	textureDesc.SampleDesc.Count = 1;
	textureDesc.Usage = D3D11_USAGE_DEFAULT;
	textureDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_DEPTH_STENCIL;

	D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
	srvDesc.Format = DXGI_FORMAT_R16_UNORM;
	srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
	srvDesc.Texture2DArray.MipLevels = 1;
	srvDesc.Texture2DArray.ArraySize = kCascadeCount;

	D3D11_DEPTH_STENCIL_VIEW_DESC dsvDesc{};
	dsvDesc.Format = DXGI_FORMAT_D16_UNORM;
	dsvDesc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2DARRAY;
	dsvDesc.Texture2DArray.ArraySize = kCascadeCount;

	try {
		for (uint32_t i = 0; i < 2; ++i) {
			targets[i] = std::make_unique<Texture2D>(textureDesc, i == 0 ? "TerrainShadows::LODShadowCascadesA" : "TerrainShadows::LODShadowCascadesB");
			targets[i]->CreateSRV(srvDesc);
			targets[i]->CreateDSV(dsvDesc);
		}
	} catch (const std::exception& e) {
		logger::error("[Terrain Shadows] Failed to create LOD shadow cascades: {}", e.what());
		targets[0].reset();
		targets[1].reset();
		return false;
	}

	resolution = a_resolution;
	return true;
}

LODShadowMap::LightSpace LODShadowMap::MakeLightSpace(const RE::NiPoint3& a_direction, const RE::NiPoint3& a_origin) const
{
	LightSpace space;
	space.direction = a_direction;
	space.origin = a_origin;

	const double forward[3] = { a_direction.x, a_direction.y, a_direction.z };
	const double up[3] = { 0.0, std::abs(forward[2]) > 0.99 ? 1.0 : 0.0, std::abs(forward[2]) > 0.99 ? 0.0 : 1.0 };
	double right[3] = {
		up[1] * forward[2] - up[2] * forward[1],
		up[2] * forward[0] - up[0] * forward[2],
		up[0] * forward[1] - up[1] * forward[0]
	};
	const double rightLength = std::sqrt(right[0] * right[0] + right[1] * right[1] + right[2] * right[2]);
	for (double& component : right)
		component /= rightLength;
	const double top[3] = {
		forward[1] * right[2] - forward[2] * right[1],
		forward[2] * right[0] - forward[0] * right[2],
		forward[0] * right[1] - forward[1] * right[0]
	};

	for (int i = 0; i < 3; ++i) {
		space.axes[0][i] = right[i];
		space.axes[1][i] = top[i];
		space.axes[2][i] = forward[i];
	}

	for (uint32_t cascade = 0; cascade < kCascadeCount; ++cascade) {
		const double texel = 2.0 * kCascadeRadius[cascade] / resolution;
		space.cascadeCenter[cascade][0] = std::round(Dot(space.axes[0], a_origin) / texel) * texel;
		space.cascadeCenter[cascade][1] = std::round(Dot(space.axes[1], a_origin) / texel) * texel;
	}
	space.depthCenter = Dot(space.axes[2], a_origin);
	return space;
}

void LODShadowMap::BuildReceiverData(float a_strength)
{
	receiver.strength = publishedValid ? a_strength : 0.0f;
	receiver.resolution = static_cast<float>(std::max(resolution, 1u));
	if (!publishedValid)
		return;

	const double depthScale = 0.5 / kDepthHalfRange;
	receiver.axisX = { static_cast<float>(published.axes[0][0]), static_cast<float>(published.axes[0][1]), static_cast<float>(published.axes[0][2]), 0.0f };
	receiver.axisY = { static_cast<float>(published.axes[1][0]), static_cast<float>(published.axes[1][1]), static_cast<float>(published.axes[1][2]), 0.0f };
	receiver.axisZ = {
		static_cast<float>(published.axes[2][0] * depthScale),
		static_cast<float>(published.axes[2][1] * depthScale),
		static_cast<float>(published.axes[2][2] * depthScale),
		static_cast<float>(0.5 - published.depthCenter * depthScale)
	};

	float bias[kCascadeCount]{};
	for (uint32_t cascade = 0; cascade < kCascadeCount; ++cascade) {
		const double scale = 0.5 / kCascadeRadius[cascade];
		receiver.cascades[cascade] = {
			static_cast<float>(scale),
			static_cast<float>(0.5 - published.cascadeCenter[cascade][0] * scale),
			static_cast<float>(-scale),
			static_cast<float>(0.5 + published.cascadeCenter[cascade][1] * scale)
		};
		bias[cascade] = static_cast<float>(2.0 * kCascadeRadius[cascade] / resolution * depthScale);
	}
	receiver.depthBias = { bias[0], bias[1], bias[2], static_cast<float>(kConstantBias * depthScale) };
}

void LODShadowMap::Update(bool a_enabled, uint32_t a_resolution)
{
	enabled = false;

	auto* worldSpace = GetWorldSpace();
	auto* sky = globals::game::sky;
	RE::NiPoint3 direction;
	int reason = 0;
	if (!a_enabled)
		reason = 1;
	else if (!globals::shaderCache->IsEnabled())
		reason = 2;
	else if (!worldSpace || !sky || sky->mode.get() != RE::Sky::Mode::kFull)
		reason = 3;
	else if (!GetLightDirection(direction))
		reason = 4;
	else if (!EnsureResources(a_resolution))
		reason = 5;

	if (reason != inactiveReason) {
		static constexpr const char* reasons[] = { "active", "disabled", "shader cache disabled", "not in an exterior with a sky", "no directional light", "resource or shader creation failed" };
		logger::info("[Terrain Shadows] LOD shadows: {}", reasons[reason]);
		inactiveReason = reason;
	}

	if (reason != 0) {
		capturing = false;
		capturePending = false;
		BuildReceiverData(0.0f);
		return;
	}

	enabled = true;

	const auto now = std::chrono::steady_clock::now();
	if (capturing && now - captureStart > kCaptureTimeout) {
		capturing = false;
		if (++timeoutCount <= 5)
			logger::info("[Terrain Shadows] LOD shadow capture timed out with faces {:#x}, {} draws captured", capturedFaces, capturedDraws);
	}

	float strength = 0.0f;
	bool stale = !publishedValid || publishedWorldSpace != worldSpace;
	if (!stale) {
		auto* camera = RE::Main::WorldRootCamera();
		const RE::NiPoint3 cameraPosition = camera ? camera->world.translate : published.origin;
		const float angle = AngleDegrees(direction, published.direction);
		const float distance = cameraPosition.GetDistance(published.origin);
		const float coverage = static_cast<float>(kCascadeRadius[kCascadeCount - 1]);
		const float angleFade = std::clamp((kFadeEndAngle - angle) / (kFadeEndAngle - kFadeStartAngle), 0.0f, 1.0f);
		const float coverageFade = std::clamp((0.75f * coverage - distance) / (0.25f * coverage), 0.0f, 1.0f);
		strength = angleFade * coverageFade;
		stale = angle > kRecaptureAngle || distance > kRecaptureDistance || now - publishedTime > kRefreshInterval;
	}

	capturePending = stale && !capturing;
	BuildReceiverData(strength);
}

void LODShadowMap::StartCapture(const RE::NiPoint3& a_origin)
{
	RE::NiPoint3 direction;
	if (!GetLightDirection(direction))
		return;

	building = MakeLightSpace(direction, a_origin);
	capturing = true;
	capturePending = false;
	capturedFaces = 0;
	capturedDraws = 0;
	skippedShader = 0;
	skippedTopology = 0;
	skippedVertexShader = 0;
	skippedCamera = 0;
	captureRowsValid = false;
	captureStart = std::chrono::steady_clock::now();

	ReverseZ::SetHookPassthrough(true);
	globals::d3d::context->ClearDepthStencilView(targets[1 - activeTarget]->dsv.get(), D3D11_CLEAR_DEPTH, 1.0f, 0);
	ReverseZ::SetHookPassthrough(false);
}

void LODShadowMap::BeginFace(const RE::NiAVObject* a_camera, uint32_t a_faceMask)
{
	faceActive = false;
	g_activeCapture = nullptr;
	if (++faceCount == 1)
		logger::info("[Terrain Shadows] First reflection cubemap face: mask {:#x}, LOD shadows {}", a_faceMask, enabled ? "active" : "inactive");
	if (!enabled || !a_camera || a_faceMask == 0 || (a_faceMask & ~kAllFaces) != 0)
		return;

	if (!capturing) {
		if (!capturePending)
			return;
		StartCapture(a_camera->world.translate);
		if (!capturing)
			return;
	}

	faceActive = true;
	faceMask = a_faceMask;
	g_activeCapture = this;
}

void LODShadowMap::EndFace()
{
	if (!faceActive)
		return;

	faceActive = false;
	g_activeCapture = nullptr;
	capturedFaces |= faceMask;
	if (capturedFaces == kAllFaces)
		Publish();
}

void LODShadowMap::Publish()
{
	activeTarget = 1 - activeTarget;
	published = building;
	publishedValid = true;
	publishedWorldSpace = GetWorldSpace();
	publishedTime = std::chrono::steady_clock::now();
	lastCaptureDraws = capturedDraws;
	++publishCount;
	capturing = false;

	if (publishCount <= 3 || publishCount % 100 == 0)
		logger::info("[Terrain Shadows] LOD shadow capture {}: {} draws, skipped {} shader / {} topology / {} vertex shader / {} camera, origin ({:.0f}, {:.0f}, {:.0f}), light ({:.3f}, {:.3f}, {:.3f})",
			publishCount, capturedDraws, skippedShader, skippedTopology, skippedVertexShader, skippedCamera,
			published.origin.x, published.origin.y, published.origin.z, published.direction.x, published.direction.y, published.direction.z);

	BuildReceiverData(1.0f);
	Bind(globals::d3d::context);
}

void LODShadowMap::Bind(ID3D11DeviceContext* a_context) const
{
	if (!a_context || !targets[activeTarget])
		return;
	ID3D11ShaderResourceView* view = targets[activeTarget]->srv.get();
	a_context->PSSetShaderResources(61, 1, &view);
	a_context->CSSetShaderResources(61, 1, &view);
}

bool LODShadowMap::UpdateCaptureRows()
{
	const auto& frame = globals::game::frameBufferCached.data;
	if (captureRowsValid && std::memcmp(&lastViewProj, &frame.CameraViewProj, sizeof(Matrix)) == 0 && std::memcmp(&lastPosAdjust, &frame.CameraPosAdjust, sizeof(float4)) == 0)
		return true;

	lastViewProj = frame.CameraViewProj;
	lastPosAdjust = frame.CameraPosAdjust;
	captureRowsValid = false;

	const auto& m = frame.CameraViewProj.m;
	const double a[3][3] = {
		{ m[0][0], m[0][1], m[0][2] },
		{ m[1][0], m[1][1], m[1][2] },
		{ m[3][0], m[3][1], m[3][2] }
	};
	const double t[3] = { m[0][3], m[1][3], m[3][3] };

	const double cofactor[3][3] = {
		{ a[1][1] * a[2][2] - a[1][2] * a[2][1], a[1][2] * a[2][0] - a[1][0] * a[2][2], a[1][0] * a[2][1] - a[1][1] * a[2][0] },
		{ a[0][2] * a[2][1] - a[0][1] * a[2][2], a[0][0] * a[2][2] - a[0][2] * a[2][0], a[0][1] * a[2][0] - a[0][0] * a[2][1] },
		{ a[0][1] * a[1][2] - a[0][2] * a[1][1], a[0][2] * a[1][0] - a[0][0] * a[1][2], a[0][0] * a[1][1] - a[0][1] * a[1][0] }
	};
	const double determinant = a[0][0] * cofactor[0][0] + a[0][1] * cofactor[0][1] + a[0][2] * cofactor[0][2];
	if (std::abs(determinant) < 1e-30)
		return false;

	double inverse[3][3];
	for (int row = 0; row < 3; ++row)
		for (int column = 0; column < 3; ++column)
			inverse[row][column] = cofactor[column][row] / determinant;

	const double posAdjust[3] = { frame.CameraPosAdjust.x, frame.CameraPosAdjust.y, frame.CameraPosAdjust.z };

	auto makeRow = [&](const double (&a_gradient)[3], double a_offset) {
		double coefficient[3];
		for (int column = 0; column < 3; ++column)
			coefficient[column] = inverse[0][column] * a_gradient[0] + inverse[1][column] * a_gradient[1] + inverse[2][column] * a_gradient[2];
		double constant = a_offset;
		for (int i = 0; i < 3; ++i)
			constant += a_gradient[i] * posAdjust[i] - coefficient[i] * t[i];
		return float4(static_cast<float>(coefficient[0]), static_cast<float>(coefficient[1]), static_cast<float>(coefficient[2]), static_cast<float>(constant));
	};

	CaptureCB data{};
	for (uint32_t cascade = 0; cascade < kCascadeCount; ++cascade) {
		const double inverseRadius = 1.0 / kCascadeRadius[cascade];
		const double gradientX[3] = { building.axes[0][0] * inverseRadius, building.axes[0][1] * inverseRadius, building.axes[0][2] * inverseRadius };
		const double gradientY[3] = { building.axes[1][0] * inverseRadius, building.axes[1][1] * inverseRadius, building.axes[1][2] * inverseRadius };
		data.CascadeRowX[cascade] = makeRow(gradientX, -building.cascadeCenter[cascade][0] * inverseRadius);
		data.CascadeRowY[cascade] = makeRow(gradientY, -building.cascadeCenter[cascade][1] * inverseRadius);
	}
	const double depthScale = 0.5 / kDepthHalfRange;
	const double gradientZ[3] = { building.axes[2][0] * depthScale, building.axes[2][1] * depthScale, building.axes[2][2] * depthScale };
	data.DepthRow = makeRow(gradientZ, 0.5 - building.depthCenter * depthScale);

	captureCB->Update(data);
	captureRowsValid = true;
	return true;
}

bool LODShadowMap::BeginCaptureDraw(ID3D11DeviceContext* a_context)
{
	if (!faceActive)
		return false;

	auto* state = globals::state;
	auto* shader = state->currentShader;
	if (!shader)
		return false;

	bool alphaTest = true;
	switch (shader->shaderType.get()) {
	case RE::BSShader::Type::Lighting:
		if (state->currentPixelDescriptor & static_cast<uint32_t>(SIE::ShaderCache::LightingShaderFlags::WorldMap))
			return false;
		alphaTest = (state->currentPixelDescriptor & static_cast<uint32_t>(SIE::ShaderCache::LightingShaderFlags::DoAlphaTest)) != 0;
		break;
	case RE::BSShader::Type::DistantTree:
		break;
	default:
		++skippedShader;
		return false;
	}

	D3D11_PRIMITIVE_TOPOLOGY topology = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
	a_context->IAGetPrimitiveTopology(&topology);
	if (topology != D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST && topology != D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP) {
		++skippedTopology;
		return false;
	}

	ID3D11VertexShader* boundVertexShader = nullptr;
	a_context->VSGetShader(&boundVertexShader, nullptr, nullptr);
	bool knownVertexShader = false;
	if (boundVertexShader) {
		auto* engineVertexShader = *globals::game::currentVertexShader;
		knownVertexShader = engineVertexShader && boundVertexShader == reinterpret_cast<ID3D11VertexShader*>(engineVertexShader->shader);
		if (!knownVertexShader) {
			auto* replacementVertexShader = globals::shaderCache->GetVertexShader(*shader, state->modifiedVertexDescriptor);
			knownVertexShader = replacementVertexShader && boundVertexShader == reinterpret_cast<ID3D11VertexShader*>(replacementVertexShader->shader);
		}
		boundVertexShader->Release();
	}
	if (!knownVertexShader) {
		++skippedVertexShader;
		return false;
	}
	if (!UpdateCaptureRows()) {
		++skippedCamera;
		return false;
	}

	ReverseZ::SetHookPassthrough(true);

	saved = {};
	a_context->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, saved.renderTargets, &saved.depthStencil);
	a_context->OMGetDepthStencilState(&saved.depthState, &saved.stencilRef);
	a_context->RSGetState(&saved.rasterState);
	saved.viewportCount = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
	a_context->RSGetViewports(&saved.viewportCount, saved.viewports);
	a_context->GSGetShader(&saved.geometryShader, nullptr, nullptr);
	a_context->GSGetConstantBuffers(0, 1, &saved.geometryBuffer);
	a_context->PSGetShader(&saved.pixelShader, nullptr, nullptr);

	const D3D11_VIEWPORT viewport{ 0.0f, 0.0f, static_cast<float>(resolution), static_cast<float>(resolution), 0.0f, 1.0f };
	ID3D11Buffer* captureBuffer = captureCB->CB();
	a_context->OMSetRenderTargets(0, nullptr, targets[1 - activeTarget]->dsv.get());
	a_context->OMSetDepthStencilState(depthState.get(), 0);
	a_context->RSSetState(rasterState.get());
	a_context->RSSetViewports(1, &viewport);
	a_context->GSSetShader(captureGS.get(), nullptr, 0);
	a_context->GSSetConstantBuffers(0, 1, &captureBuffer);
	a_context->PSSetShader(alphaTest ? captureAlphaPS.get() : capturePS.get(), nullptr, 0);

	++capturedDraws;
	return true;
}

void LODShadowMap::EndCaptureDraw(ID3D11DeviceContext* a_context)
{
	UINT renderTargetCount = 0;
	for (UINT i = 0; i < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i)
		if (saved.renderTargets[i])
			renderTargetCount = i + 1;

	a_context->OMSetRenderTargets(renderTargetCount, saved.renderTargets, saved.depthStencil);
	a_context->OMSetDepthStencilState(saved.depthState, saved.stencilRef);
	a_context->RSSetState(saved.rasterState);
	if (saved.viewportCount > 0)
		a_context->RSSetViewports(saved.viewportCount, saved.viewports);
	a_context->GSSetShader(saved.geometryShader, nullptr, 0);
	a_context->GSSetConstantBuffers(0, 1, &saved.geometryBuffer);
	a_context->PSSetShader(saved.pixelShader, nullptr, 0);

	ReverseZ::SetHookPassthrough(false);

	for (auto* view : saved.renderTargets)
		if (view)
			view->Release();
	if (saved.depthStencil)
		saved.depthStencil->Release();
	if (saved.depthState)
		saved.depthState->Release();
	if (saved.rasterState)
		saved.rasterState->Release();
	if (saved.geometryShader)
		saved.geometryShader->Release();
	if (saved.geometryBuffer)
		saved.geometryBuffer->Release();
	if (saved.pixelShader)
		saved.pixelShader->Release();
	saved = {};
}

void LODShadowMap::DrawStatus() const
{
	ImGui::Text("LOD shadows: %s, %u cubemap faces seen, %u timeouts", enabled ? "active" : "inactive", faceCount, timeoutCount);
	if (!publishedValid) {
		ImGui::TextUnformatted(capturing ? "LOD shadows: capturing" : "LOD shadows: no capture yet (needs a water reflection cubemap)");
		return;
	}
	const auto age = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - publishedTime).count() / 1000.0f;
	ImGui::Text("LOD shadows: %u captures, last %.1fs ago, %u draws, strength %.2f", publishCount, age, lastCaptureDraws, receiver.strength);
	ImGui::Text("Skipped draws: %u shader, %u topology, %u vertex shader, %u camera", skippedShader, skippedTopology, skippedVertexShader, skippedCamera);
}

void LODShadowMap::DrawDebugView()
{
	if (!publishedValid || !targets[activeTarget])
		return;

	if (!debugView || debugView->desc.Width != resolution) {
		D3D11_TEXTURE2D_DESC desc = targets[activeTarget]->desc;
		desc.ArraySize = 1;
		desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
		srvDesc.Format = DXGI_FORMAT_R16_UNORM;
		srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
		srvDesc.Texture2D.MipLevels = 1;
		try {
			debugView = std::make_unique<Texture2D>(desc, "TerrainShadows::LODShadowDebugView");
			debugView->CreateSRV(srvDesc);
		} catch (const std::exception& e) {
			logger::error("[Terrain Shadows] Failed to create the LOD shadow debug view: {}", e.what());
			debugView.reset();
			return;
		}
	}

	ImGui::SliderInt("Cascade", &debugCascade, 0, static_cast<int>(kCascadeCount) - 1);
	globals::d3d::context->CopySubresourceRegion(debugView->resource.get(), 0, 0, 0, 0, targets[activeTarget]->resource.get(), D3D11CalcSubresource(0, static_cast<UINT>(debugCascade), 1), nullptr);
	ImGui::Image(debugView->srv.get(), { 512.0f, 512.0f });
}
