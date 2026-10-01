#include "StrandGenerator.h"

namespace Strands
{
	namespace
	{
		CardsToStrands::Vec3 ToVec3(const float3& a_v) { return { a_v.x, a_v.y, a_v.z }; }
		float3 ToFloat3(const CardsToStrands::Vec3& a_v) { return { a_v.x, a_v.y, a_v.z }; }

		CardsToStrands::CardMesh ToCardMesh(const HairMeshData& a_mesh)
		{
			CardsToStrands::CardMesh mesh;
			mesh.positions.reserve(a_mesh.positions.size());
			for (const auto& p : a_mesh.positions)
				mesh.positions.push_back(ToVec3(p));
			mesh.normals.reserve(a_mesh.normals.size());
			for (const auto& n : a_mesh.normals)
				mesh.normals.push_back(ToVec3(n));
			mesh.uvs.reserve(a_mesh.uvs.size());
			for (const auto& uv : a_mesh.uvs)
				mesh.uvs.push_back({ uv.x, uv.y });
			mesh.boneIndices = a_mesh.boneIndices;
			mesh.boneWeights = a_mesh.boneWeights;
			mesh.indices = a_mesh.indices;
			mesh.boneNames = a_mesh.boneNames;
			for (const auto& p : a_mesh.boneBindPositions)
				mesh.boneBindPositions.push_back(ToVec3(p));
			mesh.coverage.width = a_mesh.coverage.width;
			mesh.coverage.height = a_mesh.coverage.height;
			mesh.coverage.alpha = a_mesh.coverage.alpha;
			mesh.coverage.shade = a_mesh.coverage.shade;
			mesh.flow.width = a_mesh.flow.width;
			mesh.flow.height = a_mesh.flow.height;
			mesh.flow.rg = a_mesh.flow.rg;
			return mesh;
		}

		CardsToStrands::Settings ToSettings(const StrandStyle& a_style)
		{
			CardsToStrands::Settings settings;
			settings.seeding = a_style.seeding == SeedMode::Area ? CardsToStrands::Seeding::Area : (a_style.seeding == SeedMode::Roots ? CardsToStrands::Seeding::Scalp : CardsToStrands::Seeding::Auto);
			switch (a_style.flowAxis) {
			case FlowAxis::V:
				settings.flowAxis = CardsToStrands::FlowAxis::V;
				break;
			case FlowAxis::NegV:
				settings.flowAxis = CardsToStrands::FlowAxis::NegV;
				break;
			case FlowAxis::U:
				settings.flowAxis = CardsToStrands::FlowAxis::U;
				break;
			case FlowAxis::NegU:
				settings.flowAxis = CardsToStrands::FlowAxis::NegU;
				break;
			default:
				settings.flowAxis = CardsToStrands::FlowAxis::Auto;
				break;
			}
			settings.density = a_style.density;
			settings.segmentLength = a_style.segmentLength;
			settings.lengthScale = a_style.lengthScale;
			settings.volume = a_style.volume;
			settings.layerJitter = a_style.layerJitter;
			settings.clumpStrength = a_style.clumpStrength;
			settings.clumpSize = a_style.clumpSize;
			settings.clumpTwist = a_style.clumpTwist;
			settings.shortLength = a_style.shortLength;
			settings.coverageThreshold = a_style.coverageThreshold;
			settings.seed = a_style.seed;
			for (const auto& r : a_style.excludeUV)
				settings.excludeUV.push_back({ r.minU, r.minV, r.maxU, r.maxV });
			return settings;
		}

		RestPoint Pack(const CardsToStrands::StrandPoint& a_point)
		{
			RestPoint point{};
			point.position = ToFloat3(a_point.position);
			point.normal = ToFloat3(a_point.normal);
			point.u = a_point.uv.x;
			point.v = a_point.uv.y;
			point.t = a_point.t;

			// Unorm8 weights summing to 255, the rounding error on the strongest bone.
			std::array<uint32_t, 4> packed{};
			uint32_t packedTotal = 0;
			for (size_t i = 0; i < 4; ++i) {
				packed[i] = static_cast<uint32_t>(std::lround(std::clamp(a_point.weights[i], 0.0f, 1.0f) * 255.0f));
				packedTotal += packed[i];
			}
			packed[0] = static_cast<uint32_t>(std::clamp<int>(static_cast<int>(packed[0]) + 255 - static_cast<int>(packedTotal), 0, 255));
			point.bones01 = a_point.bones[0] | (static_cast<uint32_t>(a_point.bones[1]) << 16);
			point.bones23 = a_point.bones[2] | (static_cast<uint32_t>(a_point.bones[3]) << 16);
			point.weights = packed[0] | (packed[1] << 8) | (packed[2] << 16) | (packed[3] << 24);
			return point;
		}
	}

	bool GenerateStrands(const HairMeshData& a_mesh, const StrandStyle& a_style, StrandAssetData& o_asset, std::string& o_error)
	{
		o_asset = {};
		CardsToStrands::Result result;
		if (!CardsToStrands::Convert(ToCardMesh(a_mesh), ToSettings(a_style), result, o_error))
			return false;

		o_asset.pointsPerStrand = result.pointsPerStrand;
		o_asset.points.reserve(result.points.size());
		for (const auto& point : result.points)
			o_asset.points.push_back(Pack(point));
		o_asset.strands.reserve(result.strands.size());
		for (const auto& strand : result.strands)
			o_asset.strands.push_back({ strand.length, strand.random, strand.guide, strand.clumpRandom });
		o_asset.guideCount = result.guideCount;
		o_asset.headBone = result.headBone;
		o_asset.headCentre = ToFloat3(result.headCentre);
		o_asset.headRadius = result.headRadius;
		o_asset.averageLength = result.averageLength;
		o_asset.flowMapShare = result.stats.flowMapShare;
		o_asset.seedingUsed = result.stats.seedingUsed == CardsToStrands::Seeding::Area ? SeedMode::Area : SeedMode::Roots;
		o_asset.convertedTriangles = result.stats.convertedTriangles;
		o_asset.totalTriangles = result.stats.totalTriangles;
		o_asset.conversion = result.stats;
		return true;
	}
}
