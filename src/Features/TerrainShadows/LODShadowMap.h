#pragma once

#include "Buffer.h"

#include <chrono>

class LODShadowMap
{
public:
	static constexpr uint32_t kCascadeCount = 3;
	static constexpr uint32_t kCaptureCount = 2;

	struct ReceiverCapture
	{
		float4 axisX{};
		float4 axisY{};
		float4 axisZ{};
		float4 cascades[kCascadeCount]{};
	};
	static_assert(sizeof(ReceiverCapture) % 16 == 0);

	struct ReceiverData
	{
		float strength = 0.0f;
		float resolution = 1.0f;
		float blend = 1.0f;
		ReceiverCapture captures[kCaptureCount]{};
		float4 depthBias{};
	};

	static void InstallHooks();

	void Update(bool a_enabled, uint32_t a_resolution);
	void ClearShaderCache();
	void BeginFace(const RE::NiAVObject* a_camera, uint32_t a_faceMask);
	void EndFace();
	void Bind(ID3D11DeviceContext* a_context) const;
	void DrawStatus() const;
	void DrawDebugView();

	bool BeginCaptureDraw(ID3D11DeviceContext* a_context);
	void EndCaptureDraw(ID3D11DeviceContext* a_context);

	[[nodiscard]] const ReceiverData& GetReceiverData() const { return receiver; }

private:
	struct alignas(16) CaptureCB
	{
		float4 CascadeRowX[kCascadeCount];
		float4 CascadeRowY[kCascadeCount];
		float4 DepthRow;
	};
	static_assert(sizeof(CaptureCB) % 16 == 0);

	struct LightSpace
	{
		double axes[3][3]{};
		double cascadeCenter[kCascadeCount][2]{};
		double depthCenter = 0.0;
		RE::NiPoint3 direction;
		RE::NiPoint3 origin;
	};

	struct SavedState
	{
		ID3D11RenderTargetView* renderTargets[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
		ID3D11DepthStencilView* depthStencil = nullptr;
		ID3D11DepthStencilState* depthState = nullptr;
		UINT stencilRef = 0;
		ID3D11RasterizerState* rasterState = nullptr;
		D3D11_VIEWPORT viewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
		UINT viewportCount = 0;
		ID3D11GeometryShader* geometryShader = nullptr;
		ID3D11Buffer* geometryBuffer = nullptr;
		ID3D11PixelShader* pixelShader = nullptr;
	};

	bool EnsureResources(uint32_t a_resolution);
	void ReleaseTargets();
	void CompileShaders();
	void StartCapture(const RE::NiPoint3& a_origin);
	void Publish();
	bool UpdateCaptureRows();
	void BuildReceiverData(float a_strength, float a_blend);
	[[nodiscard]] LightSpace MakeLightSpace(const RE::NiPoint3& a_direction, const RE::NiPoint3& a_origin) const;
	[[nodiscard]] static ReceiverCapture MakeReceiverCapture(const LightSpace& a_space);

	std::unique_ptr<Texture2D> targets[2];
	uint32_t activeTarget = 0;
	uint32_t resolution = 0;
	std::unique_ptr<ConstantBuffer> captureCB;
	winrt::com_ptr<ID3D11GeometryShader> captureGS;
	winrt::com_ptr<ID3D11PixelShader> capturePS;
	winrt::com_ptr<ID3D11PixelShader> captureAlphaPS;
	winrt::com_ptr<ID3D11DepthStencilState> depthState;
	winrt::com_ptr<ID3D11RasterizerState> rasterState;

	bool shadersFailed = false;
	bool enabled = false;
	bool capturePending = false;
	bool capturing = false;
	bool faceActive = false;
	bool captureRowsValid = false;
	bool publishedValid = false;
	bool previousValid = false;
	float blendDuration = 1.0f;
	uint32_t faceMask = 0;
	uint32_t capturedFaces = 0;
	uint32_t capturedDraws = 0;
	uint32_t lastCaptureDraws = 0;
	uint32_t publishCount = 0;
	uint32_t faceCount = 0;
	uint32_t timeoutCount = 0;
	uint32_t skippedShader = 0;
	uint32_t skippedTopology = 0;
	uint32_t skippedVertexShader = 0;
	uint32_t skippedCamera = 0;
	uint32_t handledTransitionGeneration = 0;
	int inactiveReason = -1;
	int debugCascade = 0;
	std::unique_ptr<Texture2D> debugView;
	std::chrono::steady_clock::time_point captureStart{};
	std::chrono::steady_clock::time_point publishedTime{};
	RE::TESWorldSpace* publishedWorldSpace = nullptr;
	LightSpace building;
	LightSpace published;
	LightSpace previous;
	Matrix lastViewProj;
	float4 lastPosAdjust;
	ReceiverData receiver;
	SavedState saved;
};
