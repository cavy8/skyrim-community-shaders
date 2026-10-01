// Hair Strands: the actor's body and everything it wears as a narrow-band signed distance field,
// built every frame from its collision mesh (BodySdf.cpp) for the strand simulation. TressFX 4.1
// builds one the same way from its collision mesh (TressFXSDFCollision.hlsl): skin the mesh, then
// per triangle the distance to every grid cell near it, the nearest kept with an atomic minimum,
// then per cell the signed distance to that nearest triangle. HairStrandsSkin::BodySurface
// (Skinning.hlsli) reads the result.
//
// Where this differs from TressFX:
//  - TressFX's grid covers the collision mesh's whole box. Here it covers only what the hair can
//    reach, in the actor's own axes, and positions are grid cells from the skinning on.
//  - A cell keeps its nearest triangle as (quantised distance << 20 | triangle) in one atomic
//    minimum; Finalize works out the distance, sign, normal and motion from that triangle alone.
//  - The sign comes from the triangle's vertex normals, interpolated at the closest point, not its
//    face normal: the collision mesh is decimated, and its small triangles turn every way. A cell
//    nearly level with the surface there (beside an open edge: a collar, a cape's hem) counts as
//    outside, so hair passing an edge is not pulled round it.
//  - A triangle reaches OutsideBand round it and, for cells well behind it, its own inside band
//    (half the mesh's thickness there): a point deep in a thick piece of armour is still pushed
//    out, and one beside a forearm is not taken for the inside of its far side.
//  - Each cell also stores the surface's move over the frame at its closest point, for the steps
//    between frames and for contact, and a weight: 1 where the field has a value, 0 elsewhere, so
//    linear filtering averages only cells with values.
//
// Entry points: SkinVertices, Splat, Finalize. The cells are cleared to 0xFFFFFFFF in between
// with ClearUnorderedAccessViewUint.

// Mirrors Strands::CollisionVertex (BodySdf.h).
struct CollisionVertex
{
	float3 Position;  // its source's skin space (a rigid source's own space)
	uint Normal;      // octahedral, two snorm16
	uint Bones01;     // palette entries, 16 bits each
	uint Bones23;
	uint Weights;  // four unorm8
	uint Pad;
};

// Mirrors Strands::SkinnedCollisionVertex (BodySdf.h).
struct SkinnedCollisionVertex
{
	float3 Position;  // grid cells, this frame
	float Pad0;
	float3 PreviousPosition;  // grid cells: where it was last frame (in this frame's grid)
	float Pad1;
	float3 Normal;  // grid axes, unit
	float Pad2;
};

// Mirrors Strands::BodySdfCB (BodySdf.h).
cbuffer BodySdfCB : register(b0)
{
	float4 WorldToGrid[3];  // camera-relative position to grid cells (cell centres at +0.5)
	float4 GridToWorld[3];  // grid cells to camera-relative: the actor's axes times the cell size

	uint3 GridSize;  // cells per axis
	uint VertexCount;

	uint TriangleCount;
	uint EntryCount;    // palette entries
	float OutsideBand;  // cells
	float InsideScale;  // cells per step of a triangle's inside band byte

	float CellSize;   // units
	float MaxMotion;  // units: a larger move over one frame is a teleport, not motion
	float InsideCos;  // a cell counts as inside below this cosine to the surface's normal
	float MaxExtent;  // cells: a triangle whose box is larger is broken (and skipped)

	float InsideBandCos;  // a triangle's inside band takes cells below this cosine to its normal
	float3 Pad;
};

StructuredBuffer<CollisionVertex> Vertices : register(t0);
// EntryCount 3x4 skin-to-camera rows this frame, then EntryCount last frame's, also relative to
// this frame's camera. Entries of a source not drawn this frame are scaled to nothing far away.
StructuredBuffer<float4> Palette : register(t1);
// Three vertices and flags: bits 0-7 the triangle's inside band, in steps of InsideScale cells.
StructuredBuffer<uint4> Triangles : register(t2);
StructuredBuffer<SkinnedCollisionVertex> SkinnedIn : register(t3);

RWStructuredBuffer<SkinnedCollisionVertex> SkinnedOut : register(u0);
RWTexture3D<uint> Cells : register(u1);
RWTexture3D<float4> MotionOut : register(u2);   // (the surface's move over the frame x W, W)
RWTexture3D<float4> SurfaceOut : register(u3);  // (signed distance x W, outward normal x W)

#define BODY_SDF_EMPTY 0xFFFFFFFF
#define BODY_SDF_TRIANGLE_BITS 20
#define BODY_SDF_TRIANGLE_MASK 0xFFFFF
#define BODY_SDF_DISTANCE_STEPS 4095.0

namespace HairStrandsBody
{
	float3x4 LoadEntry(uint a_entry, uint a_base)
	{
		const uint row = a_base + a_entry * 3;
		return float3x4(Palette[row], Palette[row + 1], Palette[row + 2]);
	}

	float3x4 WorldToGridMatrix()
	{
		return float3x4(WorldToGrid[0], WorldToGrid[1], WorldToGrid[2]);
	}

	float3x3 GridToWorldAxes()
	{
		return (float3x3)float3x4(GridToWorld[0], GridToWorld[1], GridToWorld[2]);
	}

	// Octahedral unit vector from two snorm16 (Strands::PackCollisionNormal).
	float3 UnpackNormal(uint a_packed)
	{
		const int2 bits = int2((int)(a_packed << 16), (int)a_packed) >> 16;
		const float2 f = max(float2(bits) / 32767.0, -1.0);
		float3 n = float3(f, 1.0 - abs(f.x) - abs(f.y));
		if (n.z < 0.0)
			n.xy = (1.0 - abs(n.yx)) * float2(n.x >= 0.0 ? 1.0 : -1.0, n.y >= 0.0 ? 1.0 : -1.0);
		return normalize(n);
	}

	float3 SafeNormalize(float3 a_v, float3 a_fallback)
	{
		const float lengthSquared = dot(a_v, a_v);
		return lengthSquared > 1e-12 ? a_v * rsqrt(lengthSquared) : a_fallback;
	}

	// The point of triangle abc closest to a_p, and its barycentric weights (Ericson, Real-Time
	// Collision Detection, 5.1.5).
	float3 ClosestOnTriangle(float3 a_p, float3 a_a, float3 a_b, float3 a_c, out float3 o_weights)
	{
		const float3 ab = a_b - a_a;
		const float3 ac = a_c - a_a;
		const float3 ap = a_p - a_a;
		const float d1 = dot(ab, ap);
		const float d2 = dot(ac, ap);
		if (d1 <= 0.0 && d2 <= 0.0) {
			o_weights = float3(1.0, 0.0, 0.0);
			return a_a;
		}
		const float3 bp = a_p - a_b;
		const float d3 = dot(ab, bp);
		const float d4 = dot(ac, bp);
		if (d3 >= 0.0 && d4 <= d3) {
			o_weights = float3(0.0, 1.0, 0.0);
			return a_b;
		}
		const float vc = d1 * d4 - d3 * d2;
		if (vc <= 0.0 && d1 >= 0.0 && d3 <= 0.0) {
			const float v = d1 / max(d1 - d3, 1e-12);
			o_weights = float3(1.0 - v, v, 0.0);
			return a_a + v * ab;
		}
		const float3 cp = a_p - a_c;
		const float d5 = dot(ab, cp);
		const float d6 = dot(ac, cp);
		if (d6 >= 0.0 && d5 <= d6) {
			o_weights = float3(0.0, 0.0, 1.0);
			return a_c;
		}
		const float vb = d5 * d2 - d1 * d6;
		if (vb <= 0.0 && d2 >= 0.0 && d6 <= 0.0) {
			const float w = d2 / max(d2 - d6, 1e-12);
			o_weights = float3(1.0 - w, 0.0, w);
			return a_a + w * ac;
		}
		const float va = d3 * d6 - d5 * d4;
		if (va <= 0.0 && d4 - d3 >= 0.0 && d5 - d6 >= 0.0) {
			const float w = (d4 - d3) / max((d4 - d3) + (d5 - d6), 1e-12);
			o_weights = float3(0.0, 1.0 - w, w);
			return a_b + w * (a_c - a_b);
		}
		const float denominator = 1.0 / max(va + vb + vc, 1e-12);
		const float v = vb * denominator;
		const float w = vc * denominator;
		o_weights = float3(1.0 - v - w, v, w);
		return a_a + ab * v + ac * w;
	}

	// The deepest a triangle reaches behind it, in cells.
	float InsideBand(uint a_flags)
	{
		return (float)(a_flags & 0xFF) * InsideScale;
	}

	float MaxBand()
	{
		return max(OutsideBand, 255.0 * InsideScale);
	}
}

// Skins every collision vertex with this frame's and last frame's palette, into this frame's grid.
[numthreads(64, 1, 1)] void SkinVertices(uint3 dispatchID : SV_DispatchThreadID) {
	const uint id = dispatchID.x;
	if (id >= VertexCount)
		return;
	const CollisionVertex vertex = Vertices[id];
	uint4 bones = uint4(vertex.Bones01 & 0xFFFF, vertex.Bones01 >> 16, vertex.Bones23 & 0xFFFF, vertex.Bones23 >> 16);
	bones = min(bones, (EntryCount - 1).xxxx);
	float4 weights = float4(vertex.Weights & 0xFF, (vertex.Weights >> 8) & 0xFF, (vertex.Weights >> 16) & 0xFF, vertex.Weights >> 24);
	weights /= max(dot(weights, 1.0), 1e-6);

	float3x4 current = 0;
	float3x4 previous = 0;
	[unroll] for (uint i = 0; i < 4; ++i)
	{
		current += HairStrandsBody::LoadEntry(bones[i], 0) * weights[i];
		previous += HairStrandsBody::LoadEntry(bones[i], EntryCount * 3) * weights[i];
	}
	const float3x4 toGrid = HairStrandsBody::WorldToGridMatrix();
	const float4 position = float4(vertex.Position, 1.0);

	SkinnedCollisionVertex result;
	result.Position = mul(toGrid, float4(mul(current, position), 1.0));
	result.PreviousPosition = mul(toGrid, float4(mul(previous, position), 1.0));
	// The grid's axes are the actor's, scaled evenly: its rotation part keeps directions.
	result.Normal = HairStrandsBody::SafeNormalize(mul((float3x3)toGrid, mul((float3x3)current, HairStrandsBody::UnpackNormal(vertex.Normal))), float3(0.0, 0.0, 1.0));
	result.Pad0 = 0;
	result.Pad1 = 0;
	result.Pad2 = 0;
	SkinnedOut[id] = result;
};

// Per triangle, every cell within its bands: in front out to OutsideBand, behind out to its inside
// band. A cell keeps its nearest triangle (ties: the lowest index).
[numthreads(64, 1, 1)] void Splat(uint3 dispatchID : SV_DispatchThreadID) {
	const uint id = dispatchID.x;
	if (id >= TriangleCount)
		return;
	const uint4 corners = Triangles[id];
	const SkinnedCollisionVertex va = SkinnedIn[corners.x];
	const SkinnedCollisionVertex vb = SkinnedIn[corners.y];
	const SkinnedCollisionVertex vc = SkinnedIn[corners.z];
	const float3 a = va.Position;
	const float3 b = vb.Position;
	const float3 c = vc.Position;
	// A source not drawn this frame sits far outside the grid; a broken skin is not finite.
	if (any(!isfinite(a)) || any(!isfinite(b)) || any(!isfinite(c)))
		return;
	const float3 cross0 = cross(b - a, c - a);
	const float twiceArea = length(cross0);
	if (!(twiceArea > 1e-8))
		return;
	const float3 normal = cross0 / twiceArea;
	const float insideBand = HairStrandsBody::InsideBand(corners.w);

	// The box round the triangle and the prism behind it, out to the outside band.
	float3 low = min(min(a, b), c);
	float3 high = max(max(a, b), c);
	const float3 back = -normal * insideBand;
	low = min(low, low + back) - OutsideBand;
	high = max(high, high + back) + OutsideBand;
	if (any(high - low > MaxExtent) || any(high < 0.0) || any(low > (float3)GridSize))
		return;
	const int3 first = max((int3)ceil(low - 0.5), 0);
	const int3 last = min((int3)floor(high - 0.5), (int3)GridSize - 1);
	if (any(first > last))
		return;

	const float maxBand = HairStrandsBody::MaxBand();
	int x, y, z;
	[loop] for (z = first.z; z <= last.z; ++z)
	{
		[loop] for (y = first.y; y <= last.y; ++y)
		{
			[loop] for (x = first.x; x <= last.x; ++x)
			{
				const float3 centre = float3(x, y, z) + 0.5;
				const float side = dot(centre - a, normal);
				if (side > OutsideBand || side < -insideBand)
					continue;
				float3 weights;
				const float3 offset = centre - HairStrandsBody::ClosestOnTriangle(centre, a, b, c, weights);
				const float distance = length(offset);
				// The inside band only for a cell well behind the triangle (Finalize puts it inside),
				// where the band (half the mesh's thickness) stays inside the mesh: one beside it,
				// past a convex surface, gets the outside band, or a triangle out of the outside
				// band's reach could not win it back.
				const float3 vertexNormal = weights.x * va.Normal + weights.y * vb.Normal + weights.z * vc.Normal;
				const bool behind = dot(offset, vertexNormal) < InsideBandCos * distance * length(vertexNormal);
				if (distance > (behind ? insideBand : OutsideBand))
					continue;
				const uint key = ((uint)(saturate(distance / maxBand) * BODY_SDF_DISTANCE_STEPS) << BODY_SDF_TRIANGLE_BITS) | id;
				InterlockedMin(Cells[uint3(x, y, z)], key);
			}
		}
	}
};

// Per cell: the signed distance to its nearest triangle, the outward normal, and the surface's move
// over the frame at the closest point; nothing (weight 0) where no triangle reached.
[numthreads(4, 4, 4)] void Finalize(uint3 dispatchID : SV_DispatchThreadID) {
	if (any(dispatchID >= GridSize))
		return;
	const uint key = Cells[dispatchID];
	float4 motion = 0;
	float4 surface = 0;
	if (key != BODY_SDF_EMPTY) {
		const uint4 corners = Triangles[min(key & BODY_SDF_TRIANGLE_MASK, TriangleCount - 1)];
		const SkinnedCollisionVertex a = SkinnedIn[corners.x];
		const SkinnedCollisionVertex b = SkinnedIn[corners.y];
		const SkinnedCollisionVertex c = SkinnedIn[corners.z];
		const float3 centre = float3(dispatchID) + 0.5;
		float3 weights;
		const float3 closest = HairStrandsBody::ClosestOnTriangle(centre, a.Position, b.Position, c.Position, weights);
		const float3 offset = centre - closest;
		const float distance = length(offset);
		const float3 faceNormal = HairStrandsBody::SafeNormalize(cross(b.Position - a.Position, c.Position - a.Position), float3(0.0, 0.0, 1.0));
		const float3 vertexNormal = HairStrandsBody::SafeNormalize(weights.x * a.Normal + weights.y * b.Normal + weights.z * c.Normal, faceNormal);
		const bool inside = distance > 1e-4 && dot(offset, vertexNormal) < InsideCos * distance;
		// The gradient of the signed distance: away from the surface outside, towards it inside.
		const float3 gradient = distance > 1e-3 ? offset * ((inside ? -1.0 : 1.0) / distance) : vertexNormal;

		const float3x3 toWorld = HairStrandsBody::GridToWorldAxes();
		const float3 previous = weights.x * a.PreviousPosition + weights.y * b.PreviousPosition + weights.z * c.PreviousPosition;
		float3 move = mul(toWorld, closest - previous);
		const float moveLength = length(move);
		if (!(moveLength <= MaxMotion))
			move = 0;
		motion = float4(move, 1.0);
		surface = float4((inside ? -distance : distance) * CellSize, HairStrandsBody::SafeNormalize(mul(toWorld, gradient), float3(0.0, 0.0, 1.0)));
	}
	MotionOut[dispatchID] = motion;
	SurfaceOut[dispatchID] = surface;
};
