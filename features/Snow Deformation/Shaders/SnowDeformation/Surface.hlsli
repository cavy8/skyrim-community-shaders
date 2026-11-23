#ifndef __SNOW_DEFORMATION_SURFACE_HLSLI__
#define __SNOW_DEFORMATION_SURFACE_HLSLI__

#define SURFACE_MASK 3u
#define SURFACE_SNOW 0u
#define SURFACE_MUD 1u
#define SURFACE_SAND 2u
#define SURFACE_ASH 3u
#define SURFACE_FIELD_MASK 2047

uint SurfaceOf(float height)
{
	return asuint(height) & SURFACE_MASK;
}

float WithSurface(float height, uint surface)
{
	return asfloat((asuint(height) & ~SURFACE_MASK) | surface);
}

#endif
