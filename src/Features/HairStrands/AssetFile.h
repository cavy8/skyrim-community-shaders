#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "StrandGenerator.h"

namespace Strands
{
	/** @brief A shape of a hair as the game has it: its node name and mesh counts. */
	struct ShapeId
	{
		std::string name;
		uint32_t vertexCount = 0;
		uint32_t triangleCount = 0;
		bool renamed = false;  // named after its head part: the game gives head-part geometry the part's editor ID
	};

	enum class AssetLoad
	{
		Loaded,      // o_asset holds the hair (no strands and no cards: another shape of its part draws it)
		NotCovered,  // no part of the file holds the shape: it keeps its cards
		Rejected     // the file does not fit the mesh (o_error says why): convert instead
	};

	/**
	 * @brief Loads a hair the Skyrim Hair Designer exported (.skhair) for one shape.
	 *
	 * The file holds the designer's conversion part by part, a part being the shapes it converted
	 * together; its records are RestPoint, StrandInfo and CardVertex, so only bones are mapped:
	 * each bone name to the skin instance's bone of that name (the head bone, with a warning, when
	 * there is none), chain joints after the skin instance's bones, as GenerateStrands numbers
	 * them. Format: the designer's docs/FORMAT.md. Pure CPU; safe to run on a worker thread.
	 *
	 * @param a_path       The file.
	 * @param a_shapes     The shapes converted together here, the drawing shape first (one, for a
	 *                     shape converted on its own). The part holding the first must list every
	 *                     one, with the same vertex and triangle counts. A renamed shape (the file
	 *                     has the nif's name) is found by its counts; among copies with the same
	 *                     counts, by the longest nif shape name its editor ID ends with.
	 * @param a_boneNames  The drawing shape's skin-instance bones, by name.
	 * @param o_asset      Receives the part, in the drawing shape's skin space. If the shape is not
	 *                     its part's first, an asset with no strands and no cards: the part's first
	 *                     shape draws it all.
	 */
	AssetLoad LoadAssetFile(const std::filesystem::path& a_path, const std::vector<ShapeId>& a_shapes, const std::vector<std::string>& a_boneNames, StrandAssetData& o_asset, std::string& o_error);
}
