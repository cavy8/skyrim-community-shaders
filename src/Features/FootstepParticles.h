#pragma once

#include "Buffer.h"

struct FootstepParticles : Feature
{
	virtual inline std::string GetName() override { return "Footstep Particles"; }
	virtual std::string GetDisplayName() override { return T("feature.footstep_particles.name", "Footstep Particles"); }
	virtual inline std::string GetShortName() override { return "FootstepParticles"; }
	virtual std::string_view GetCategory() const override { return FeatureCategories::kLandscapeAndTextures; }

	virtual std::pair<std::string, std::vector<std::string>> GetFeatureSummary() override
	{
		return { T("feature.footstep_particles.description", "Kicks up dust, dirt, snow and water splashes from footsteps, driven by the ground material, Snow Cover and Wetness Effects puddles."),
			{ T("feature.footstep_particles.key_feature_1", "Dust and dirt clods on dirt, gravel, sand and ash"),
				T("feature.footstep_particles.key_feature_2", "Snow powder and clumps on snowy ground, including Snow Cover snow"),
				T("feature.footstep_particles.key_feature_3", "Water splashes in rain puddles from Wetness Effects"),
				T("feature.footstep_particles.key_feature_4", "GPU simulated, tinted by the ground and lit by the scene") } };
	}

	enum class Surface : uint32_t
	{
		Hard,
		Dirt,
		Gravel,
		Sand,
		Grass,
		Mud,
		Snow,
		Ice,
		Ash,
		Water,
		Count
	};

	struct Settings
	{
		bool Enabled = true;
		bool IncludeNPCs = true;
		bool UseWetnessEffects = true;
		bool UseSnowCover = true;
		float MaxDistance = 3500.0f;
		float DustIntensity = 1.0f;
		float SnowIntensity = 1.0f;
		float SplashIntensity = 1.0f;
		float ParticleSize = 1.0f;
		float Lifetime = 1.0f;
		float Opacity = 1.0f;
		uint SurfaceOverride = 0;
	};

	struct alignas(16) Particle
	{
		float3 Position;
		float Age;
		float3 Velocity;
		float Lifetime;
		float3 Color;
		float Size;
		uint Type;
		float GroundZ;
		float Growth;
		float Opacity;
	};
	static_assert(sizeof(Particle) == 64);

	struct alignas(16) FootstepEvent
	{
		float3 Position;
		uint FirstSlot;
		float3 Velocity;
		uint Surface;
		float2 Forward;
		float Strength;
		float Scale;
		uint Seed;
		uint Flags;
		float2 Pad;
	};
	static_assert(sizeof(FootstepEvent) == 64);

	struct alignas(16) FrameData
	{
		float4 FogNearColor;
		float4 FogFarColor;
		float4 FogParams;
		float DeltaTime;
		float Gravity;
		uint EventCount;
		uint MaxParticles;
		float DustIntensity;
		float SnowIntensity;
		float SplashIntensity;
		float SizeScale;
		float LifetimeScale;
		float Opacity;
		float SoftDistance;
		uint FrameFlags;
		float2 Wind;
		float Time;
		float FramePad;
	};
	static_assert(sizeof(FrameData) == 112);

	Settings settings;

	virtual void SetupResources() override;
	virtual void ClearShaderCache() override;
	virtual void PostPostLoad() override;
	virtual void DataLoaded() override;
	virtual void DrawSettings() override;
	virtual void LoadSettings(json& o_json) override;
	virtual void SaveSettings(json& o_json) override;
	virtual void RestoreDefaultSettings() override;

	void QueueFootstep(RE::ActorHandle a_actor, const RE::BSFixedString& a_tag);
	void Render(RE::BSShaderAccumulator* a_accumulator);

private:
	struct PendingFootstep
	{
		RE::ActorHandle actor;
		uint32_t tagFlags;
	};

	struct DebugInfo
	{
		uint32_t eventsThisSecond = 0;
		uint32_t eventsLastSecond = 0;
		float secondTimer = 0.0f;
		uint64_t renderedFrames = 0;
		std::string lastPlayerMaterial;
		Surface lastPlayerSurface = Surface::Hard;
	};

	void BuildEvents(const RE::NiPoint3& a_cameraPosition);
	bool BuildEvent(const PendingFootstep& a_pending, const RE::NiPoint3& a_cameraPosition, FootstepEvent& a_event);
	FrameData BuildFrameData(float a_deltaTime, uint32_t a_eventCount) const;
	void Dispatch(float a_deltaTime, bool a_clear);

	ID3D11ComputeShader* GetSpawnCS();
	ID3D11ComputeShader* GetSimulateCS();
	ID3D11VertexShader* GetParticleVS();
	ID3D11PixelShader* GetParticlePS();
	std::vector<std::pair<const char*, const char*>> GetFeatureDefines() const;

	std::mutex pendingMutex;
	std::vector<PendingFootstep> pendingFootsteps;
	std::vector<FootstepEvent> frameEvents;
	bool testBurstQueued = false;

	eastl::unique_ptr<Buffer> particleBuffer;
	eastl::unique_ptr<Buffer> eventBuffer;
	eastl::unique_ptr<ConstantBuffer> frameConstants;

	winrt::com_ptr<ID3D11SamplerState> linearSampler;
	winrt::com_ptr<ID3D11BlendState> blendState;
	winrt::com_ptr<ID3D11RasterizerState> rasterizerState;
	winrt::com_ptr<ID3D11DepthStencilState> depthStencilState;

	winrt::com_ptr<ID3D11ComputeShader> spawnCS;
	winrt::com_ptr<ID3D11ComputeShader> simulateCS;
	winrt::com_ptr<ID3D11VertexShader> particleVS;
	winrt::com_ptr<ID3D11PixelShader> particlePS;
	bool shaderCompileFailed = false;

	uint32_t nextSlot = 0;
	uint32_t eventSeed = 0x1234567u;
	float simulationTime = 0.0f;
	float lastSpawnTime = -1000.0f;
	RE::NiPoint3 lastCameraPosition;
	bool hasLastCamera = false;
	bool clearRequested = true;
	uint32_t lastRenderedFrame = UINT32_MAX;

	DebugInfo debug;
};
