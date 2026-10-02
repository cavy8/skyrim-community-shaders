// Standalone D3D11 WARP regression checks; run from a VS x64 developer prompt:
// cl /nologo /EHsc /std:c++17 tools/test-neural-temporal.cpp /Febuild/test-neural-temporal.exe /Fobuild/test-neural-temporal.obj /link d3d11.lib d3dcompiler.lib
// build/test-neural-temporal.exe (repository root must be the working directory)
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <array>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;
constexpr unsigned kSize = 8;
using Pixel = std::array<float, 4>;
using Image = std::array<Pixel, kSize * kSize>;

void Check(HRESULT result)
{
	if (FAILED(result))
		throw std::runtime_error("D3D failure: " + std::to_string(result));
}

struct Includes : ID3DInclude
{
	HRESULT Open(D3D_INCLUDE_TYPE, LPCSTR name, LPCVOID, LPCVOID* data, UINT* bytes) override
	{
		for (const auto* root : { "features/Neural Rendering/Shaders/", "package/Shaders/" }) {
			std::ifstream stream(std::string(root) + name, std::ios::binary | std::ios::ate);
			if (!stream)
				continue;
			*bytes = static_cast<UINT>(stream.tellg());
			auto* buffer = new char[*bytes];
			stream.seekg(0);
			stream.read(buffer, *bytes);
			*data = buffer;
			return S_OK;
		}
		return E_FAIL;
	}
	HRESULT Close(LPCVOID data) override
	{
		delete[] static_cast<const char*>(data);
		return S_OK;
	}
};

struct Texture
{
	ComPtr<ID3D11Texture2D> resource;
	ComPtr<ID3D11ShaderResourceView> srv;
	ComPtr<ID3D11UnorderedAccessView> uav;
};

struct Harness
{
	ComPtr<ID3D11Device> device;
	ComPtr<ID3D11DeviceContext> context;
	ComPtr<ID3D11Buffer> constants;
	ComPtr<ID3D11SamplerState> sampler;
	std::array<unsigned, 80> params{};
	unsigned checks = 0;

	Harness()
	{
		Check(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0,
			D3D11_SDK_VERSION, &device, nullptr, &context));
		D3D11_BUFFER_DESC buffer{};
		buffer.ByteWidth = sizeof(params);
		buffer.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
		Check(device->CreateBuffer(&buffer, nullptr, &constants));
		D3D11_SAMPLER_DESC desc{};
		desc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
		desc.AddressU = desc.AddressV = desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
		desc.MaxLOD = D3D11_FLOAT32_MAX;
		Check(device->CreateSamplerState(&desc, &sampler));
	}

	void SetFloat(unsigned index, float value) { std::memcpy(&params[index], &value, sizeof(value)); }
	void ResetParams()
	{
		params.fill(0);
		params[4] = params[5] = params[6] = params[7] = params[8] = params[9] = kSize;
		SetFloat(2, 1.0f);
		SetFloat(3, 1.0f);
		params[11] = 1;  // StaleAnswer
		params[15] = 1;  // Display gamma
		for (unsigned index = 16; index < 48; ++index)
			SetFloat(index, 1.0f);
		SetFloat(64, 1.0f);
		SetFloat(66, 1000000.0f);
		SetFloat(69, -1.0f);
		SetFloat(72, 1.0f);
	}

	Texture MakeTexture(const Image& image, bool destination = false)
	{
		Texture texture;
		D3D11_TEXTURE2D_DESC desc{};
		desc.Width = desc.Height = kSize;
		desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
		desc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
		desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | (destination ? D3D11_BIND_UNORDERED_ACCESS : 0);
		D3D11_SUBRESOURCE_DATA data{ image.data(), sizeof(Pixel) * kSize, 0 };
		Check(device->CreateTexture2D(&desc, &data, &texture.resource));
		Check(device->CreateShaderResourceView(texture.resource.Get(), nullptr, &texture.srv));
		if (destination)
			Check(device->CreateUnorderedAccessView(texture.resource.Get(), nullptr, &texture.uav));
		return texture;
	}

	ComPtr<ID3D11ComputeShader> Compile(const wchar_t* path, bool reverse)
	{
		Includes includes;
		D3D_SHADER_MACRO macros[]{ { "REVERSE_Z", "1" }, { nullptr, nullptr } };
		ComPtr<ID3DBlob> code, errors;
		auto result = D3DCompileFromFile(path, reverse ? macros : nullptr, &includes, "main", "cs_5_0",
			D3DCOMPILE_WARNINGS_ARE_ERRORS, 0, &code, &errors);
		if (errors)
			std::cerr << static_cast<const char*>(errors->GetBufferPointer());
		Check(result);
		ComPtr<ID3D11ComputeShader> shader;
		Check(device->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &shader));
		return shader;
	}

	Image Run(ID3D11ComputeShader* shader, const std::array<ID3D11ShaderResourceView*, 10>& sources)
	{
		auto output = MakeTexture(Image{}, true);
		context->UpdateSubresource(constants.Get(), 0, nullptr, params.data(), 0, 0);
		auto* cb = constants.Get();
		auto* state = sampler.Get();
		auto* uav = output.uav.Get();
		context->CSSetShader(shader, nullptr, 0);
		context->CSSetShaderResources(0, 10, sources.data());
		context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
		context->CSSetConstantBuffers(0, 1, &cb);
		context->CSSetSamplers(0, 1, &state);
		context->Dispatch(1, 1, 1);
		context->ClearState();
		D3D11_TEXTURE2D_DESC desc{};
		output.resource->GetDesc(&desc);
		desc.BindFlags = 0;
		desc.Usage = D3D11_USAGE_STAGING;
		desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
		ComPtr<ID3D11Texture2D> readback;
		Check(device->CreateTexture2D(&desc, nullptr, &readback));
		context->CopyResource(readback.Get(), output.resource.Get());
		D3D11_MAPPED_SUBRESOURCE mapped{};
		Check(context->Map(readback.Get(), 0, D3D11_MAP_READ, 0, &mapped));
		Image image;
		for (unsigned y = 0; y < kSize; ++y)
			std::memcpy(image.data() + y * kSize, static_cast<const char*>(mapped.pData) + y * mapped.RowPitch, sizeof(Pixel) * kSize);
		context->Unmap(readback.Get(), 0);
		return image;
	}

	void Expect(const char* name, float actual, float expected)
	{
		if (!std::isfinite(actual) || std::abs(actual - expected) > 0.0001f)
			throw std::runtime_error(std::string(name) + ": got " + std::to_string(actual) + ", expected " + std::to_string(expected));
		++checks;
	}

	void Test(bool reverse, const wchar_t* decodePath)
	{
		ResetParams();
		const float nearDepth = reverse ? 0.2f : 0.8f;
		const float farDepth = reverse ? 0.01f : 0.99f;
		Image motion{}, history{}, depth{}, categories{};
		motion.fill({ -0.125f, 0, 0, 0 });
		history.fill({ -0.25f, 0, nearDepth, 0 });
		depth.fill({ nearDepth, 0, 0, 0 });
		auto compose = Compile(L"features/Neural Rendering/Shaders/NeuralRendering/ComposeMotionCS.hlsl", reverse);
		auto composeCase = [&](const char* name, float expected) {
			auto mv = MakeTexture(motion), prev = MakeTexture(history), z = MakeTexture(depth), cat = MakeTexture(categories);
			Expect(name, Run(compose.Get(), { mv.srv.Get(), prev.srv.Get(), z.srv.Get(), cat.srv.Get() })[4 * kSize + 4][0], expected);
		};
		composeCase("acceleration uses both frames", -0.375f);
		history[4 * kSize + 4][0] = 0.5f;
		composeCase("previous motion sampled along trajectory", -0.375f);
		SetFloat(76, 0.75f);
		composeCase("previous raster jitter", 0.375f);
		SetFloat(76, 0.0f);
		history.fill({ 0.125f, 0, nearDepth, 0 });
		composeCase("direction reversal", 0.0f);
		history.fill({ 0, 0, farDepth, 0 });
		composeCase("disocclusion invalidates motion chain", 2.0f);
		history.fill({ 0, 0, nearDepth, 1 });
		composeCase("material change invalidates motion chain", 2.0f);
		motion.fill({ 2, 0, 0, 0 });
		composeCase("offscreen invalidates motion chain", 2.0f);
		motion.fill({ std::numeric_limits<float>::quiet_NaN(), 0, 0, 0 });
		composeCase("non-finite motion is rejected", 2.0f);

		motion.fill({ 0, 0, 0, 0 });
		Image model{}, original{}, proxy{};
		model.fill({ 0.75f, 0.75f, 0.75f, 1 });
		original.fill({ 0.5f, 0.5f, 0.5f, 1 });
		proxy = original;
		auto decode = Compile(decodePath, reverse);
		auto decodeCase = [&](const char* name, float expected) {
			auto mv = MakeTexture(motion), prev = MakeTexture(history), z = MakeTexture(depth), cat = MakeTexture(categories);
			auto answer = MakeTexture(model), clean = MakeTexture(original), oldProxy = MakeTexture(proxy);
			Expect(name, Run(decode.Get(), { answer.srv.Get(), clean.srv.Get(), oldProxy.srv.Get(), z.srv.Get(), cat.srv.Get(), nullptr, nullptr, mv.srv.Get(), nullptr, prev.srv.Get() })[4 * kSize + 4][0], expected);
		};
		history.fill({ 0, 0, nearDepth, 0 });
		decodeCase("matching history preserves enhancement", 0.75f);
		history.fill({ 0, 0, farDepth, 0 });
		decodeCase("same-color exposed surface stays clean", 0.5f);
		history.fill({ 0, 0, nearDepth, 1 });
		decodeCase("same-depth different material stays clean", 0.5f);
		motion.fill({ 2, 0, 0, 0 });
		decodeCase("offscreen stale edit stays clean", 0.5f);
		params[11] = 0;
		decodeCase("fresh evaluation keeps enhancement", 0.75f);
		params[11] = 1;
		motion.fill({ 0, 0, 0, 0 });
		history.fill({ 0, 0, nearDepth, 0 });
		original.fill({ 0.5f, 0, 0, 1 });
		proxy.fill({ 0, 0.5f * std::pow(0.2126f / 0.7152f, 1.0f / 2.2f), 0, 1 });
		decodeCase("equal-luminance chroma change stays clean", 0.5f);

		auto capture = Compile(L"features/Neural Rendering/Shaders/NeuralRendering/CaptureTemporalGuidesCS.hlsl", reverse);
		motion.fill({ 0.125f, -0.25f, 0, 0 });
		categories.fill({ 2.0f / 65535.0f, 0, 0, 0 });
		auto mv = MakeTexture(motion), z = MakeTexture(depth), cat = MakeTexture(categories);
		const auto captured = Run(capture.Get(), { mv.srv.Get(), z.srv.Get(), cat.srv.Get() })[4 * kSize + 4];
		Expect("capture motion x", captured[0], 0.125f);
		Expect("capture motion y", captured[1], -0.25f);
		Expect("capture depth", captured[2], nearDepth);
		Expect("capture unpacked category", captured[3], 2.0f);
	}
};

int wmain(int argc, wchar_t** argv)
{
	try {
		Harness harness;
		const auto* decodePath = argc > 1 ? argv[1] : L"features/Neural Rendering/Shaders/NeuralRendering/DecodeColorCS.hlsl";
		harness.Test(false, decodePath);
		harness.Test(true, decodePath);
		std::cout << "PASS: " << harness.checks << " D3D11 WARP temporal regression checks (normal and Reverse Z).\n";
		return 0;
	} catch (const std::exception& error) {
		std::cerr << "FAIL: " << error.what() << '\n';
		return 1;
	}
}
