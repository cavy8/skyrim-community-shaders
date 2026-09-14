// Run from the repository root after compiling with the VS x64 toolchain:
// cl /nologo /EHsc /std:c++20 tools/test-neural-category-blending.cpp /Fe:build/test-neural-category-blending.exe /Fo:build/test-neural-category-blending.obj /link d3d11.lib d3dcompiler.lib
//
// Draws through the production NeuralRenderingCategories blend helpers and HLSL encoding on
// D3D11 WARP. Masks2 is R16G16_UNORM: vertex AO in R, material category in G. Checks that
// - the deferred table's R matches upstream's R16_UNORM Masks2 (RT7 mirroring RT0) bit for bit,
//   and G takes the source category only from unblended draws;
// - the category redraw state changes nothing but G, which takes the source category exactly
//   where binary coverage is 1;
// - the forward category state leaves R and RT0-6 as they were, and G follows coverage.
// The deferred table is rebuilt here the way Deferred::OverrideBlendStates builds it.
#include "../src/Features/Upscaling/CategoryBlend.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <d3dcompiler.h>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

namespace
{
	void Check(HRESULT result)
	{
		if (FAILED(result))
			throw std::runtime_error("D3D11 call failed: " + std::to_string(result));
	}

	void Require(bool condition, const std::string& message)
	{
		if (!condition)
			throw std::runtime_error(message);
	}

	uint16_t Unorm16(float value)
	{
		return static_cast<uint16_t>(std::lround(value * 65535.0f));
	}

	uint16_t StoredCategory(uint32_t category)
	{
		return static_cast<uint16_t>(category * 257);  // category / 255 in 16-bit unorm
	}

	struct Surface
	{
		float ao;
		uint32_t category;
	};

	struct RenderTarget
	{
		ComPtr<ID3D11Texture2D> texture;
		ComPtr<ID3D11Texture2D> staging;
		ComPtr<ID3D11RenderTargetView> view;
	};
}

int main()
try {
	ComPtr<ID3D11Device> device;
	ComPtr<ID3D11DeviceContext> context;
	Check(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0,
		D3D11_SDK_VERSION, &device, nullptr, &context));
	std::ifstream include("package/Shaders/Common/NeuralRenderingCategories.hlsli");
	if (!include)
		throw std::runtime_error("Run from the repository root");
	std::string shader{ std::istreambuf_iterator<char>(include), {} };
	shader += R"(
cbuffer Test : register(b0) { float AO; uint Category; float Alpha; float Padding; };
float4 VS(uint id : SV_VertexID) : SV_Position {
    return float4(id == 2 ? 3 : -1, id == 1 ? 3 : -1, 0, 1);
}
struct Output { float4 Color : SV_Target0; float4 Masks2 : SV_Target7; };
Output PS(float4 position : SV_Position) {
    Output result;
    result.Color = float4(0.25, 0.5, 0.75, Alpha);
    result.Masks2 = float4(AO, NeuralRenderingCategories::Encode(Category), 0, Alpha);
    return result;
})";
	auto compile = [&](const char* entry, const char* profile) {
		ComPtr<ID3DBlob> bytecode, errors;
		auto result = D3DCompile(shader.data(), shader.size(), nullptr, nullptr, nullptr, entry, profile,
			D3DCOMPILE_ENABLE_STRICTNESS, 0, &bytecode, &errors);
		if (errors)
			std::cerr << static_cast<const char*>(errors->GetBufferPointer());
		Check(result);
		return bytecode;
	};
	auto vsCode = compile("VS", "vs_5_0"), psCode = compile("PS", "ps_5_0");
	ComPtr<ID3D11VertexShader> vs;
	ComPtr<ID3D11PixelShader> ps;
	Check(device->CreateVertexShader(vsCode->GetBufferPointer(), vsCode->GetBufferSize(), nullptr, &vs));
	Check(device->CreatePixelShader(psCode->GetBufferPointer(), psCode->GetBufferSize(), nullptr, &ps));
	context->VSSetShader(vs.Get(), nullptr, 0);
	context->PSSetShader(ps.Get(), nullptr, 0);
	context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	D3D11_RASTERIZER_DESC raster{};
	raster.FillMode = D3D11_FILL_SOLID;
	raster.CullMode = D3D11_CULL_NONE;
	ComPtr<ID3D11RasterizerState> rasterState;
	Check(device->CreateRasterizerState(&raster, &rasterState));
	context->RSSetState(rasterState.Get());
	D3D11_VIEWPORT viewport{ 0, 0, 1, 1, 0, 1 };
	context->RSSetViewports(1, &viewport);

	auto makeTarget = [&](DXGI_FORMAT format) {
		RenderTarget target;
		D3D11_TEXTURE2D_DESC desc{};
		desc.Width = desc.Height = desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
		desc.Format = format;
		desc.BindFlags = D3D11_BIND_RENDER_TARGET;
		Check(device->CreateTexture2D(&desc, nullptr, &target.texture));
		Check(device->CreateRenderTargetView(target.texture.Get(), nullptr, &target.view));
		desc.BindFlags = 0;
		desc.Usage = D3D11_USAGE_STAGING;
		desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
		Check(device->CreateTexture2D(&desc, nullptr, &target.staging));
		return target;
	};
	auto masks2 = makeTarget(DXGI_FORMAT_R16G16_UNORM);
	auto upstreamMasks2 = makeTarget(DXGI_FORMAT_R16_UNORM);
	auto color = makeTarget(DXGI_FORMAT_R32G32B32A32_FLOAT);
	auto read = [&]<typename T>(RenderTarget& target, T* values, size_t count) {
		context->CopyResource(target.staging.Get(), target.texture.Get());
		D3D11_MAPPED_SUBRESOURCE mapped{};
		Check(context->Map(target.staging.Get(), 0, D3D11_MAP_READ, 0, &mapped));
		std::memcpy(values, mapped.pData, sizeof(T) * count);
		context->Unmap(target.staging.Get(), 0);
	};

	D3D11_BUFFER_DESC bufferDesc{};
	bufferDesc.ByteWidth = 16;
	bufferDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
	ComPtr<ID3D11Buffer> constants;
	Check(device->CreateBuffer(&bufferDesc, nullptr, &constants));
	auto* buffer = constants.Get();
	context->PSSetConstantBuffers(0, 1, &buffer);

	static constexpr float colorClear[4]{ 0.125f, 0.25f, 0.375f, 0.5f };
	struct Result
	{
		std::array<uint16_t, 2> masks2;
		std::array<float, 4> color;
	};
	// Clears to dest, draws source with the given alpha and returns what a_target and RT0 hold.
	auto draw = [&](const D3D11_BLEND_DESC& desc, RenderTarget& target, Surface dest, Surface source, float alpha) {
		ComPtr<ID3D11BlendState> blend;
		Check(device->CreateBlendState(&desc, &blend));
		context->OMSetBlendState(blend.Get(), nullptr, ~0u);
		const float clear[4]{ dest.ao, dest.category / 255.0f, 0, 0 };
		context->ClearRenderTargetView(target.view.Get(), clear);
		context->ClearRenderTargetView(color.view.Get(), colorClear);
		struct
		{
			float ao;
			uint32_t category;
			float alpha;
			float padding;
		} data{ source.ao, source.category, alpha, 0 };
		context->UpdateSubresource(constants.Get(), 0, nullptr, &data, 0, 0);
		ID3D11RenderTargetView* views[8]{ color.view.Get(), nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, target.view.Get() };
		context->OMSetRenderTargets(8, views, nullptr);
		context->Draw(3, 0);
		context->OMSetRenderTargets(0, nullptr, nullptr);
		Result result{};
		read(target, result.masks2.data(), &target == &masks2 ? 2 : 1);
		read(color, result.color.data(), 4);
		return result;
	};

	struct BlendMode
	{
		const char* name;
		BOOL enable;
		D3D11_BLEND source;
		D3D11_BLEND dest;
	};
	static constexpr std::array<BlendMode, 5> blendModes{ {
		{ "opaque", false, D3D11_BLEND_ONE, D3D11_BLEND_ZERO },
		{ "alpha", true, D3D11_BLEND_SRC_ALPHA, D3D11_BLEND_INV_SRC_ALPHA },
		{ "premultiplied", true, D3D11_BLEND_ONE, D3D11_BLEND_INV_SRC_ALPHA },
		{ "additive", true, D3D11_BLEND_ONE, D3D11_BLEND_ONE },
		{ "multiplicative", true, D3D11_BLEND_DEST_COLOR, D3D11_BLEND_ZERO },
	} };
	auto makeForwardDesc = [](const BlendMode& mode, bool independent) {
		D3D11_BLEND_DESC desc{};
		desc.IndependentBlendEnable = independent;
		desc.RenderTarget[0] = { mode.enable, mode.source, mode.dest, D3D11_BLEND_OP_ADD,
			D3D11_BLEND_ONE, D3D11_BLEND_INV_SRC_ALPHA, D3D11_BLEND_OP_ADD, D3D11_COLOR_WRITE_ENABLE_ALL };
		// An independent native forward state may disable writes in unused targets.
		for (unsigned i = 1; i < 8; ++i)
			desc.RenderTarget[i] = { false, D3D11_BLEND_ONE, D3D11_BLEND_ZERO, D3D11_BLEND_OP_ADD,
				D3D11_BLEND_ONE, D3D11_BLEND_ZERO, D3D11_BLEND_OP_ADD, 0 };
		return desc;
	};
	// Deferred::OverrideBlendStates: every target mirrors RT0, then Masks2 takes its write mask.
	auto makeDeferredDesc = [&](const BlendMode& mode, bool upstream) {
		auto desc = makeForwardDesc(mode, false);
		desc.IndependentBlendEnable = true;
		for (unsigned i = 1; i < 8; ++i)
			desc.RenderTarget[i] = desc.RenderTarget[0];
		if (!upstream)
			desc.RenderTarget[7].RenderTargetWriteMask = NeuralRenderingCategories::GetDeferredMasks2WriteMask(desc.RenderTarget[0]);
		return desc;
	};

	// Baseline: letting a blended draw write the category corrupts it.
	{
		auto desc = makeDeferredDesc(blendModes[3], true);
		auto result = draw(desc, masks2, { 0, 6 }, { 0, 2 }, 1.0f);
		Require(result.masks2[1] == StoredCategory(8), "Baseline did not reproduce additive Hair + Equipment -> 8");
		std::cout << "Baseline reproduced: additive Hair 2 over Equipment 6 stores 8 (decodes as Everything Else)\n";
	}

	unsigned checks = 0;
	static constexpr std::array<float, 3> aoValues{ 0.0f, 0.25f, 0.999f };
	for (const auto& mode : blendModes) {
		const auto deferred = makeDeferredDesc(mode, false);
		const auto upstream = makeDeferredDesc(mode, true);
		const auto redraw = NeuralRenderingCategories::MakeCategoryRedrawBlendDesc(deferred);
		for (float ao : aoValues) {
			for (uint32_t source = 0; source < 7; ++source) {
				for (uint32_t dest = 0; dest < 7; ++dest) {
					const Surface sourceSurface{ ao, source }, destSurface{ 1.0f - ao, dest };
					const std::string label = std::string(mode.name) + " source " + std::to_string(source) + " dest " + std::to_string(dest);

					for (float alpha : { 0.0f, 0.25f, 1.0f }) {
						auto ours = draw(deferred, masks2, destSurface, sourceSurface, alpha);
						auto theirs = draw(upstream, upstreamMasks2, destSurface, sourceSurface, alpha);
						Require(ours.masks2[0] == theirs.masks2[0], "Deferred AO differs from upstream: " + label);
						Require(ours.color == theirs.color, "Deferred RT0 differs from upstream: " + label);
						Require(ours.masks2[1] == StoredCategory(mode.enable ? dest : source), "Deferred category wrong: " + label);
						++checks;
					}

					for (float coverage : { 0.0f, 1.0f }) {
						auto result = draw(redraw, masks2, destSurface, sourceSurface, coverage);
						Require(result.masks2[0] == Unorm16(destSurface.ao), "Category redraw changed AO: " + label);
						Require(std::memcmp(result.color.data(), colorClear, sizeof(colorClear)) == 0, "Category redraw changed RT0: " + label);
						Require(result.masks2[1] == StoredCategory(coverage > 0 ? source : dest), "Category redraw category wrong: " + label);
						++checks;
					}

					for (bool independent : { false, true }) {
						const auto original = makeForwardDesc(mode, independent);
						const auto forward = NeuralRenderingCategories::MakeForwardCategoryBlendDesc(original);
						for (unsigned i = 0; i < 7; ++i) {
							const auto& expected = original.RenderTarget[independent ? i : 0];
							Require(std::memcmp(&forward.RenderTarget[i], &expected, sizeof(expected)) == 0, "Changed an existing forward target");
						}
						for (float coverage : { 0.0f, 1.0f }) {
							auto result = draw(forward, masks2, destSurface, sourceSurface, coverage);
							Require(result.masks2[0] == Unorm16(destSurface.ao), "Forward category write changed AO: " + label);
							Require(result.masks2[1] == StoredCategory(!mode.enable || coverage > 0 ? source : dest), "Forward category wrong: " + label);
							++checks;
						}
					}
				}
			}
		}
	}
	std::cout << "PASS: " << checks << " D3D11 WARP checks; AO and RT0 match upstream, categories never blend\n";
	return 0;
} catch (const std::exception& error) {
	std::cerr << error.what() << '\n';
	return 1;
}
