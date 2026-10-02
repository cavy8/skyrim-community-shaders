#include "AssetFile.h"

#include <cstring>
#include <fstream>

namespace Strands
{
	namespace
	{
		constexpr char kMagic[8] = { 'S', 'K', 'Y', 'H', 'A', 'I', 'R', '\0' };
		constexpr uint32_t kVersion = 1;
		constexpr size_t kHeaderBytes = 16;                // magic, version, JSON length
		constexpr uintmax_t kMaxFileBytes = 512ull << 20;  // far past any hair: a guard against reading a wrong file whole
		constexpr float kMinWidth = 0.05f;                 // relative strand width (the designer's Set Thickness)
		constexpr float kMaxWidth = 20.0f;

		std::string Lower(std::string_view a_text)
		{
			std::string result(a_text);
			std::ranges::transform(result, result.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			return result;
		}

		struct Reader
		{
			const std::vector<uint8_t>& bytes;
			size_t at = 0;

			template <class T>
			bool GetArray(std::vector<T>& o_values, uint64_t a_count)
			{
				if (a_count > (bytes.size() - std::min(at, bytes.size())) / sizeof(T))
					return false;
				o_values.resize(static_cast<size_t>(a_count));
				if (a_count)
					std::memcpy(o_values.data(), bytes.data() + at, static_cast<size_t>(a_count) * sizeof(T));
				at += static_cast<size_t>(a_count) * sizeof(T);
				return true;
			}
		};

		CardsToStrands::Vec3 Vec(const json& a_json)
		{
			return { a_json.at(0).get<float>(), a_json.at(1).get<float>(), a_json.at(2).get<float>() };
		}

		// The file's per-strand record: StrandInfo without the width, which follows in an array of its own.
		struct FileStrand
		{
			float length;
			float random;
			uint32_t guide;
			float clumpRandom;
		};
		static_assert(sizeof(FileStrand) == 16);

		// The bytes a part's arrays take, after the JSON.
		uint64_t PartBytes(const json& a_part)
		{
			const auto strands = a_part.at("strands").get<uint64_t>();
			uint64_t joints = 0;
			for (const auto& chain : a_part.at("chains"))
				joints += chain.at("joints").get<uint64_t>();
			return strands * (sizeof(FileStrand) + sizeof(float)) + strands * a_part.at("pointsPerStrand").get<uint64_t>() * sizeof(RestPoint) +
			       a_part.at("cardVertices").get<uint64_t>() * sizeof(CardVertex) + a_part.at("cardIndices").get<uint64_t>() * sizeof(uint32_t) + joints * sizeof(float3);
		}

	}

	AssetLoad LoadAssetFile(const std::filesystem::path& a_path, const std::vector<ShapeId>& a_shapes, const std::vector<std::string>& a_boneNames, StrandAssetData& o_asset, std::string& o_error)
	{
		o_asset = {};
		const std::string name = a_path.filename().string();
		const auto reject = [&](std::string a_error) {
			o_error = std::move(a_error);
			o_asset = {};
			return AssetLoad::Rejected;
		};
		if (a_shapes.empty())
			return reject("no shape to load");

		std::error_code ec;
		const auto size = std::filesystem::file_size(a_path, ec);
		if (ec)
			return reject(std::format("cannot open {}: {}", a_path.string(), ec.message()));
		if (size < kHeaderBytes || size > kMaxFileBytes)
			return reject(std::format("{} is not a .skhair file ({} bytes)", name, size));
		std::vector<uint8_t> bytes(static_cast<size_t>(size));
		{
			std::ifstream file(a_path, std::ios::binary);
			if (!file || !file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size())))
				return reject(std::format("cannot read {}", a_path.string()));
		}
		if (std::memcmp(bytes.data(), kMagic, sizeof(kMagic)) != 0)
			return reject(name + " is not a .skhair file");
		uint32_t version = 0, jsonLength = 0;
		std::memcpy(&version, bytes.data() + 8, sizeof(uint32_t));
		std::memcpy(&jsonLength, bytes.data() + 12, sizeof(uint32_t));
		if (version == 0 || version > kVersion)
			return reject(std::format("{} is format {}; this build reads format {}", name, version, kVersion));
		if (kHeaderBytes + static_cast<uint64_t>(jsonLength) > bytes.size())
			return reject(name + " is truncated");

		try {
			const auto j = json::parse(bytes.begin() + kHeaderBytes, bytes.begin() + kHeaderBytes + jsonLength);
			const auto fileBones = j.at("bones").get<std::vector<std::string>>();
			const auto& parts = j.at("parts");

			// Where the file holds a shape: by name, or for a renamed shape (the file has the nif's
			// name) by its counts. Copies of one shape share counts; an editor ID often ends with the
			// nif's shape name (NPC2Wig: "NPC2Wig_KSsky161_F_Sky161Inv"), so the longest such ending
			// picks among them. Still more than one: not found.
			struct Found
			{
				size_t part = SIZE_MAX;
				const json* shape = nullptr;
			};
			const auto find = [&](const ShapeId& a_shape) {
				const std::string lower = Lower(a_shape.name);
				Found best;
				size_t bestSuffix = 0;
				uint32_t tied = 0;
				for (size_t p = 0; p < parts.size(); ++p) {
					for (const auto& s : parts[p].at("shapes")) {
						const std::string fileName = Lower(s.at("name").get<std::string>());
						if (fileName == lower)
							return Found{ p, &s };
						if (!a_shape.renamed || s.value("vertices", 0u) != a_shape.vertexCount || s.value("triangles", 0u) != a_shape.triangleCount)
							continue;
						const size_t suffix = !fileName.empty() && lower.ends_with(fileName) ? fileName.size() : 0;
						if (!best.shape || suffix > bestSuffix) {
							best = { p, &s };
							bestSuffix = suffix;
							tied = 1;
						} else if (suffix == bestSuffix)
							++tied;
					}
				}
				return tied == 1 ? best : Found{};
			};

			// The part holding the first shape, and where its arrays start.
			const Found first = find(a_shapes.front());
			if (!first.shape)
				return AssetLoad::NotCovered;
			const json* part = &parts.at(first.part);
			uint64_t offset = kHeaderBytes + jsonLength;
			for (size_t p = 0; p < first.part; ++p)
				offset += PartBytes(parts[p]);

			// Every shape converted together here must be in the part as exported.
			const auto& partShapes = part->at("shapes");
			for (const auto& shape : a_shapes) {
				const Found found = find(shape);
				if (found.part != first.part)
					return reject(std::format("{} converts {} without {}, which is on the same texture here", name, a_shapes.front().name, shape.name));
				const auto vertices = found.shape->value("vertices", 0u), triangles = found.shape->value("triangles", 0u);
				if (vertices != shape.vertexCount || triangles != shape.triangleCount)
					return reject(std::format("{} was exported from {} with {} vertices and {} triangles; it has {} and {} now", name, shape.name, vertices, triangles, shape.vertexCount, shape.triangleCount));
			}
			// The part's arrays are in its first shape's skin space: only that shape can draw them.
			if (first.shape != &partShapes.at(0)) {
				if (a_shapes.size() > 1)
					return reject(std::format("{} converts these shapes from {}, here from {}", name, partShapes.at(0).at("name").get<std::string>(), a_shapes.front().name));
				o_asset.sourceFile = a_path.string();
				return AssetLoad::Loaded;  // drawn by the part's first shape
			}

			const auto pointsPerStrand = part->at("pointsPerStrand").get<uint32_t>();
			const auto strandCount = part->at("strands").get<uint64_t>();
			const auto guideCount = part->at("guides").get<uint32_t>();
			if (strandCount > 0 && (pointsPerStrand < CardsToStrands::Limits::kMinPointsPerStrand || pointsPerStrand > CardsToStrands::Limits::kMaxPointsPerStrand))
				return reject(std::format("{}: {} points per strand; the strand shaders take {} to {}", name, pointsPerStrand, CardsToStrands::Limits::kMinPointsPerStrand, CardsToStrands::Limits::kMaxPointsPerStrand));
			if (guideCount > strandCount || (strandCount > 0 && guideCount == 0))
				return reject(std::format("{}: {} guides for {} strands", name, guideCount, strandCount));

			Reader data{ bytes, static_cast<size_t>(offset) };
			std::vector<FileStrand> strands;
			std::vector<float> widths;
			std::vector<RestPoint> points;
			std::vector<CardVertex> cardVertices;
			std::vector<uint32_t> cardIndices;
			bool ok = data.GetArray(strands, strandCount) && data.GetArray(widths, strandCount) && data.GetArray(points, strandCount * pointsPerStrand) &&
			          data.GetArray(cardVertices, part->at("cardVertices").get<uint64_t>()) && data.GetArray(cardIndices, part->at("cardIndices").get<uint64_t>());
			std::vector<CardsToStrands::ChainCurve> chains;
			uint32_t joints = 0;
			for (const auto& c : part->at("chains")) {
				CardsToStrands::ChainCurve chain;
				std::vector<float3> chainJoints;
				ok = ok && data.GetArray(chainJoints, c.at("joints").get<uint64_t>());
				for (const auto& joint : chainJoints)
					chain.joints.push_back({ joint.x, joint.y, joint.z });
				chain.pinnedJoints = c.at("pinnedJoints").get<uint32_t>();
				chain.radius = c.at("radius").get<float>();
				chain.parentBone = c.at("parentBone").get<int32_t>();  // a file bone until mapped below
				if (chain.joints.size() < 2 || chain.pinnedJoints > chain.joints.size() || chain.parentBone >= static_cast<int32_t>(fileBones.size()))
					return reject(std::format("{}: a chain of {} joints, {} pinned, hangs from bone {}", name, chain.joints.size(), chain.pinnedJoints, chain.parentBone));
				joints += static_cast<uint32_t>(chain.joints.size());
				chains.push_back(std::move(chain));
			}
			if (!ok)
				return reject(name + " is truncated");

			// File bones to skin-instance bones, by name; chain joints after the skin instance's bones.
			const auto findBone = [&](std::string_view a_name) {
				const std::string lower = Lower(a_name);
				for (size_t b = 0; b < a_boneNames.size(); ++b) {
					if (Lower(a_boneNames[b]) == lower)
						return static_cast<int32_t>(b);
				}
				return -1;
			};
			const auto head = part->at("head");
			const auto fileHead = head.at("bone").get<int32_t>();
			int32_t headBone = fileHead >= 0 && static_cast<size_t>(fileHead) < fileBones.size() ? findBone(fileBones[fileHead]) : -1;
			if (headBone < 0)
				headBone = findBone("NPC Head [Head]");
			std::vector<uint16_t> boneMap(fileBones.size(), 0);
			std::string missing;
			for (size_t b = 0; b < fileBones.size(); ++b) {
				const int32_t bone = findBone(fileBones[b]);
				if (bone < 0)
					missing += (missing.empty() ? "" : ", ") + fileBones[b];
				boneMap[b] = static_cast<uint16_t>(bone >= 0 ? bone : std::max(headBone, 0));
			}
			if (!missing.empty())
				logger::warn("[HairStrands] {}: the mesh is not skinned to {}; their hair follows the head", name, missing);
			const auto chainBoneBase = static_cast<uint32_t>(a_boneNames.size());
			const auto fileBoneCount = static_cast<uint32_t>(fileBones.size());
			if (chainBoneBase + joints > 0xFFFF)
				return reject(std::format("{}: {} bones and {} chain joints do not fit the bone palette", name, chainBoneBase, joints));
			bool bonesOk = true;
			const auto remap = [&](uint32_t& io_bones01, uint32_t& io_bones23, uint32_t a_weights) {
				uint32_t bones[4] = { io_bones01 & 0xFFFF, io_bones01 >> 16, io_bones23 & 0xFFFF, io_bones23 >> 16 };
				for (int k = 0; k < 4; ++k) {
					const bool weighted = ((a_weights >> (8 * k)) & 0xFF) != 0;
					if (bones[k] < fileBoneCount)
						bones[k] = boneMap[bones[k]];
					else if (bones[k] - fileBoneCount < joints)
						bones[k] = chainBoneBase + (bones[k] - fileBoneCount);
					else {
						bonesOk = bonesOk && !weighted;
						bones[k] = 0;
					}
				}
				io_bones01 = bones[0] | (bones[1] << 16);
				io_bones23 = bones[2] | (bones[3] << 16);
			};
			for (auto& point : points)
				remap(point.bones01, point.bones23, point.weights);
			for (auto& vertex : cardVertices)
				remap(vertex.bones01, vertex.bones23, vertex.weights);
			if (!bonesOk)
				return reject(name + ": a bone number past the part's bones and chain joints");
			for (uint32_t index : cardIndices) {
				if (index >= cardVertices.size())
					return reject(std::format("{}: card index {} past its {} vertices", name, index, cardVertices.size()));
			}
			if (cardIndices.size() % 3 != 0)
				return reject(name + ": the card indices are not a triangle list");

			o_asset.pointsPerStrand = strandCount > 0 ? pointsPerStrand : 0;
			o_asset.points = std::move(points);
			o_asset.strands.reserve(strands.size());
			for (size_t s = 0; s < strands.size(); ++s) {
				const auto& strand = strands[s];
				if (strand.guide >= guideCount || (s < guideCount && strand.guide != s))
					return reject(std::format("{}: strand {} follows strand {}, not one of the {} guides", name, s, strand.guide, guideCount));
				const float width = std::isfinite(widths[s]) && widths[s] > 0.0f ? std::clamp(widths[s], kMinWidth, kMaxWidth) : 1.0f;
				o_asset.strands.push_back({ strand.length, strand.random, strand.guide, strand.clumpRandom, width });
			}
			o_asset.guideCount = guideCount;
			o_asset.headBone = fileHead >= 0 ? headBone : -1;
			const auto centre = Vec(head.at("centre"));
			o_asset.headCentre = { centre.x, centre.y, centre.z };
			o_asset.headRadius = head.at("radius").get<float>();
			o_asset.averageLength = part->value("averageLength", 0.0f);
			o_asset.seedingUsed = part->value("seeding", std::string{}) == "area" ? SeedMode::Area : SeedMode::Roots;
			for (const auto& shape : a_shapes)
				o_asset.totalTriangles += shape.triangleCount;
			o_asset.cardVertices = std::move(cardVertices);
			o_asset.cardIndices = std::move(cardIndices);
			uint32_t firstBone = chainBoneBase;
			for (auto& chain : chains) {
				chain.parentBone = chain.parentBone >= 0 ? boneMap[chain.parentBone] : std::max(headBone, 0);
				chain.firstBone = firstBone;
				firstBone += static_cast<uint32_t>(chain.joints.size());
			}
			o_asset.chains = std::move(chains);
			o_asset.chainBoneBase = chainBoneBase;
			o_asset.chainBoneCount = joints;
			o_asset.sourceFile = a_path.string();
			return AssetLoad::Loaded;
		} catch (const json::exception& e) {
			return reject(std::format("{}: {}", name, e.what()));
		}
	}
}
