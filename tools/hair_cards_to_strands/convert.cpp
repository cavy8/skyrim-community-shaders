// Command-line front end for the engine-agnostic hair cards to strands conversion
// (src/Features/HairStrands/CardsToStrands). Builds on any platform with a C++20 compiler:
//
//   g++ -std=c++20 -O2 -I src/Features/HairStrands/CardsToStrands
//       tools/hair_cards_to_strands/convert.cpp
//       src/Features/HairStrands/CardsToStrands/CardsToStrands.cpp -o build/cards_to_strands
//   (one command; check.py builds it the same way)
//
//   cards_to_strands <mesh.ctsm> <out.ctsr> [key=value ...]
//
// Settings keys match CardsToStrands::Settings (density, segmentLength, clumpSize, ...), plus
// seeding=auto|scalp|area and flowAxis=auto|v|-v|u|-u. The file formats are written and read by
// tools/hair_cards_to_strands/ctsio.py; see that file for the layout.

#include "CardsToStrands.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <stdexcept>

using namespace CardsToStrands;

namespace
{
	class Reader
	{
	public:
		explicit Reader(const char* a_path) :
			in(a_path, std::ios::binary)
		{
			if (!in)
				throw std::runtime_error(std::string("cannot open ") + a_path);
		}

		template <class T>
		T Get()
		{
			T value{};
			in.read(reinterpret_cast<char*>(&value), sizeof(T));
			if (!in)
				throw std::runtime_error("truncated mesh file");
			return value;
		}

		template <class T>
		void Fill(std::vector<T>& o_values, size_t a_count)
		{
			o_values.resize(a_count);
			if (a_count)
				in.read(reinterpret_cast<char*>(o_values.data()), sizeof(T) * a_count);
			if (!in)
				throw std::runtime_error("truncated mesh file");
		}

	private:
		std::ifstream in;
	};

	CardMesh ReadMesh(const char* a_path)
	{
		Reader r(a_path);
		if (r.Get<uint32_t>() != 0x4D535443u)  // "CTSM"
			throw std::runtime_error("not a CTSM mesh file");
		if (r.Get<uint32_t>() != 1)
			throw std::runtime_error("unsupported CTSM version");
		CardMesh mesh;
		const auto vertices = r.Get<uint32_t>();
		const auto indices = r.Get<uint32_t>();
		const auto bones = r.Get<uint32_t>();
		const auto hasNormals = r.Get<uint32_t>();
		r.Fill(mesh.positions, vertices);
		if (hasNormals)
			r.Fill(mesh.normals, vertices);
		r.Fill(mesh.uvs, vertices);
		r.Fill(mesh.boneIndices, vertices);
		r.Fill(mesh.boneWeights, vertices);
		r.Fill(mesh.indices, indices);
		for (uint32_t b = 0; b < bones; ++b) {
			const auto length = r.Get<uint32_t>();
			std::vector<char> name;
			r.Fill(name, length);
			mesh.boneNames.emplace_back(name.begin(), name.end());
			mesh.boneBindPositions.push_back(r.Get<Vec3>());
		}
		mesh.coverage.width = r.Get<uint32_t>();
		mesh.coverage.height = r.Get<uint32_t>();
		const size_t texels = static_cast<size_t>(mesh.coverage.width) * mesh.coverage.height;
		r.Fill(mesh.coverage.alpha, texels);
		r.Fill(mesh.coverage.shade, texels);
		mesh.flow.width = r.Get<uint32_t>();
		mesh.flow.height = r.Get<uint32_t>();
		r.Fill(mesh.flow.rg, static_cast<size_t>(mesh.flow.width) * mesh.flow.height * 2);
		return mesh;
	}

	void Apply(Settings& io_settings, const std::string& a_key, const std::string& a_value)
	{
		const auto number = [&] { return std::stof(a_value); };
		if (a_key == "seeding")
			io_settings.seeding = a_value == "area" ? Seeding::Area : (a_value == "scalp" || a_value == "roots" ? Seeding::Scalp : Seeding::Auto);
		else if (a_key == "flowAxis")
			io_settings.flowAxis = a_value == "v" ? FlowAxis::V : a_value == "-v" ? FlowAxis::NegV :
			                                                  a_value == "u"      ? FlowAxis::U :
			                                                  a_value == "-u"     ? FlowAxis::NegU :
			                                                                        FlowAxis::Auto;
		else if (a_key == "density")
			io_settings.density = number();
		else if (a_key == "segmentLength")
			io_settings.segmentLength = number();
		else if (a_key == "lengthScale")
			io_settings.lengthScale = number();
		else if (a_key == "volume")
			io_settings.volume = number();
		else if (a_key == "layerJitter")
			io_settings.layerJitter = number();
		else if (a_key == "clumpStrength")
			io_settings.clumpStrength = number();
		else if (a_key == "clumpSize")
			io_settings.clumpSize = number();
		else if (a_key == "clumpTwist")
			io_settings.clumpTwist = number();
		else if (a_key == "shortLength")
			io_settings.shortLength = number();
		else if (a_key == "coverageThreshold")
			io_settings.coverageThreshold = number();
		else if (a_key == "tipVariation")
			io_settings.tipVariation = number();
		else if (a_key == "seed")
			io_settings.seed = static_cast<uint32_t>(std::stoul(a_value));
		else
			throw std::runtime_error("unknown setting " + a_key);
	}

	template <class T>
	void Put(std::ofstream& a_out, const T& a_value)
	{
		a_out.write(reinterpret_cast<const char*>(&a_value), sizeof(T));
	}

	void WriteResult(const char* a_path, const Result& a_result)
	{
		std::ofstream out(a_path, std::ios::binary);
		if (!out)
			throw std::runtime_error(std::string("cannot write ") + a_path);
		Put(out, 0x52535443u);  // "CTSR"
		Put(out, 1u);
		Put(out, a_result.pointsPerStrand);
		Put(out, a_result.StrandCount());
		Put(out, a_result.guideCount);
		for (const auto& p : a_result.points) {
			Put(out, p.position);
			Put(out, p.normal);
			Put(out, p.uv);
			Put(out, p.t);
			Put(out, p.bones);
			Put(out, p.weights);
		}
		for (const auto& s : a_result.strands) {
			Put(out, s.length);
			Put(out, s.random);
			Put(out, s.guide);
			Put(out, s.clumpRandom);
			Put(out, s.cardGuide);
			Put(out, static_cast<uint32_t>(s.scalpRooted));
		}
		Put(out, a_result.headBone);
		Put(out, a_result.headCentre);
		Put(out, a_result.headRadius);
		Put(out, a_result.scalp.centre);
		Put(out, a_result.scalp.sphereRadius);
		Put(out, static_cast<uint32_t>(a_result.scalp.fitted));
		Put(out, a_result.scalp.radii);
		const auto& st = a_result.stats;
		for (uint32_t v : { st.totalTriangles, st.convertedTriangles, st.cardGuides, st.redundantGuides, st.rootedGuides, st.continuedGuides, st.mergedGuides, st.bridgedGuides, st.droppedGuides, static_cast<uint32_t>(st.seedingUsed) })
			Put(out, v);
		Put(out, st.flowMapShare);
		Put(out, static_cast<uint32_t>(a_result.guides.size()));
		for (const auto& guide : a_result.guides) {
			Put(out, static_cast<uint32_t>(guide.kind));
			Put(out, guide.strands);
			Put(out, static_cast<uint32_t>(guide.path.size()));
			for (const auto& p : guide.path)
				Put(out, p);
		}
	}
}

int main(int argc, char** argv)
{
	if (argc < 3) {
		std::cerr << "usage: cards_to_strands <mesh.ctsm> <out.ctsr> [key=value ...]\n";
		return 2;
	}
	try {
		const CardMesh mesh = ReadMesh(argv[1]);
		Settings settings;
		for (int i = 3; i < argc; ++i) {
			const char* eq = std::strchr(argv[i], '=');
			if (!eq)
				throw std::runtime_error(std::string("expected key=value: ") + argv[i]);
			Apply(settings, std::string(argv[i], static_cast<size_t>(eq - argv[i])), std::string(eq + 1));
		}
		Result result;
		std::string error;
		const auto start = std::chrono::steady_clock::now();
		if (!Convert(mesh, settings, result, error)) {
			std::cerr << "not converted: " << error << "\n";
			return 1;
		}
		const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
		WriteResult(argv[2], result);
		const auto& st = result.stats;
		std::printf("%u strands x %u points, avg length %.1f, %u card guides (%u redundant, %u rooted, %u continued, %u merged, %u bridged, %u dropped), %.0f ms\n",
			result.StrandCount(), result.pointsPerStrand, result.averageLength, st.cardGuides, st.redundantGuides, st.rootedGuides, st.continuedGuides, st.mergedGuides, st.bridgedGuides, st.droppedGuides, ms);
	} catch (const std::exception& e) {
		std::cerr << e.what() << "\n";
		return 1;
	}
	return 0;
}
