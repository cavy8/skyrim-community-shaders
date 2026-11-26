cbuffer AutoExposureCB : register(b1)
{
	float2 AdaptArea;
	float2 AdaptationRange;
	float AdaptLerp;
	float ExposureCompensation;
	float PurkinjeStartEV;
	float PurkinjeMaxEV;
	float PurkinjeStrength;
	float ExposureTarget;

	float BrightSceneKneeLogLum;
	float BrightSceneCompression;

	float2 GBufferScale;
	float BrightSurfaceCompensation;
	float SkyMeteringWeight;

	float AdaptationStrength;
	float BaselineLerp;
	float BrightSurfaceReference;
	float AutoExposurePadding;
};
