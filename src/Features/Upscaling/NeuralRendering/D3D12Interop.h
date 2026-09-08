#pragma once

#include <array>
#include <cstdint>

#include <d3d11_4.h>
#include <d3d12.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

namespace NeuralRendering
{
	/**
	 * @brief A texture created on D3D11 and opened as a shared D3D12 resource.
	 *
	 * The D3D11 side owns the allocation (created with a shared NT handle) so the
	 * game's existing D3D11 copies can read and write it; the D3D12 alias is what
	 * gets handed to NGX Feature 18.
	 */
	struct SharedTexture
	{
		Microsoft::WRL::ComPtr<ID3D11Texture2D> resource11;
		Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> uav11;
		Microsoft::WRL::ComPtr<ID3D12Resource> resource12;
		D3D11_TEXTURE2D_DESC desc{};
	};

	/**
	 * @brief D3D11 <-> D3D12 bridge used to run the D3D12-only DLSS NR runtime.
	 *
	 * Owns a private D3D12 device, direct queue and a small ring of command
	 * contexts, plus a fence shared with the game's D3D11 device. Work is ordered
	 * entirely on the GPU: BeginD3D12() signals the shared fence from D3D11 and
	 * makes the D3D12 queue wait on it, EndD3D12() signals it from D3D12 and makes
	 * the D3D11 context wait. The CPU only blocks when every command context is
	 * still in flight.
	 */
	class D3D12Interop
	{
	public:
		D3D12Interop() = default;
		~D3D12Interop();

		D3D12Interop(const D3D12Interop&) = delete;
		D3D12Interop& operator=(const D3D12Interop&) = delete;

		/**
		 * @brief Create the D3D12 device, queue, command contexts and shared fence.
		 * @param adapter The adapter the game's D3D11 device was created on.
		 * @param device The game's D3D11 device (must support ID3D11Device5).
		 * @param context The game's immediate context (must support ID3D11DeviceContext4).
		 * @return True on success; LastError() carries the HRESULT on failure.
		 */
		bool Initialize(IDXGIAdapter* adapter, ID3D11Device* device, ID3D11DeviceContext* context);

		/** @brief Release every interop resource and return to the uninitialized state. */
		void Shutdown();

		/**
		 * @brief Create a D3D11 texture with a shared NT handle and open its D3D12 alias.
		 *
		 * The source description must request D3D11_BIND_UNORDERED_ACCESS; usage,
		 * CPU access and misc flags are overridden to the shareable configuration.
		 *
		 * @param desc Description of the texture to create.
		 * @param texture Receives the created resources; left untouched on failure.
		 * @param name Debug name applied to the D3D11 resource and its UAV.
		 * @return True on success; LastError()/LastOperation() describe a failure.
		 */
		bool CreateSharedTexture(const D3D11_TEXTURE2D_DESC& desc, SharedTexture& texture, const char* name);

		/**
		 * @brief Begin recording D3D12 work, ordered after all pending D3D11 work.
		 * @param commandList Receives the command list to record into (owned by this class).
		 * @return True when recording started.
		 */
		bool BeginD3D12(ID3D12GraphicsCommandList** commandList);

		/**
		 * @brief Close and submit the recorded command list, then make D3D11 wait on it.
		 * @return True when the submission and the cross-API wait were queued.
		 */
		bool EndD3D12();

		/**
		 * @brief Block until every submitted D3D12 command list has completed.
		 * @return True when idle (also true when not initialized).
		 */
		bool WaitForIdle();

		/** @brief Whether Initialize() has succeeded and Shutdown() has not run since. */
		[[nodiscard]] bool IsInitialized() const { return initialized_; }
		/** @brief HRESULT of the most recent failing operation. */
		[[nodiscard]] HRESULT LastError() const { return lastError_; }
		/** @brief Name of the most recent shared-texture operation, for diagnostics. */
		[[nodiscard]] const char* LastOperation() const { return lastOperation_; }
		/** @brief D3D12_RESOURCE_FLAGS of the most recently opened shared resource. */
		[[nodiscard]] std::uint32_t LastResourceFlags() const { return lastResourceFlags_; }
		/** @brief The private D3D12 device NGX is initialized against. */
		[[nodiscard]] ID3D12Device* Device() const { return device12_.Get(); }

	private:
		static constexpr std::size_t kCommandContextCount = 3;

		struct CommandContext
		{
			Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator;
			Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> commandList;
			std::uint64_t fenceValue = 0;
		};

		bool RecordFailure(HRESULT result);
		/// @param timeoutMs CPU wait budget. The per-frame backpressure path keeps the
		///        short default; WaitForIdle() passes a longer budget because a feature
		///        rebuild it is fencing against can legitimately take longer than a frame.
		bool WaitForFence(std::uint64_t value, std::uint32_t timeoutMs = 250);

		Microsoft::WRL::ComPtr<ID3D11Device5> device11_;
		Microsoft::WRL::ComPtr<ID3D11DeviceContext4> context11_;
		Microsoft::WRL::ComPtr<ID3D11Fence> fence11_;
		Microsoft::WRL::ComPtr<ID3D12Device> device12_;
		Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue12_;
		std::array<CommandContext, kCommandContextCount> commandContexts_;
		Microsoft::WRL::ComPtr<ID3D12Fence> fence12_;
		HANDLE fenceEvent_ = nullptr;
		std::uint64_t fenceValue_ = 0;
		HRESULT lastError_ = S_OK;
		const char* lastOperation_ = "none";
		std::uint32_t lastResourceFlags_ = 0;
		std::size_t commandContextCursor_ = 0;
		std::size_t recordingContext_ = kCommandContextCount;
		bool backpressureLogged_ = false;
		bool initialized_ = false;
		bool recording_ = false;
	};
}
