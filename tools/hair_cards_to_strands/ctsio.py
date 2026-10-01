"""Reads and writes the files tools/hair_cards_to_strands/convert.cpp exchanges.

CTSM (a hair-card mesh), little-endian:
    u32 magic "CTSM", u32 version 1
    u32 vertexCount, u32 indexCount, u32 boneCount, u32 hasNormals
    f32[3] positions, f32[3] normals (if hasNormals), f32[2] uvs     (per vertex)
    u16[4] bone indices, f32[4] bone weights                         (per vertex)
    u32 indices (triangle list)
    per bone: u32 nameLength, name bytes, f32[3] bind position
    u32 width, u32 height, u8 alpha[w*h], u8 shade[w*h]             (coverage; 0x0 for none)
    u32 width, u32 height, u8 rg[w*h*2]                             (flow map; 0x0 for none)

CTSR (the converted strands):
    u32 magic "CTSR", u32 version 1, u32 pointsPerStrand, u32 strandCount, u32 guideCount
    per point: f32[3] position, f32[3] normal, f32[2] uv, f32 t, u16[4] bones, f32[4] weights
    per strand: f32 length, f32 random, u32 guide, f32 clumpRandom, u32 cardGuide, u32 scalpRooted
    i32 headBone, f32[3] headCentre, f32 headRadius
    f32[3] scalpCentre, f32 scalpSphereRadius, u32 scalpFitted, f32[24*12] scalpRadii
    u32 totalTriangles, convertedTriangles, cardGuides, redundantGuides, rootedGuides,
        continuedGuides, mergedGuides, bridgedGuides, droppedGuides, seedingUsed; f32 flowMapShare
    u32 cardGuideCount; per card guide: u32 kind (CardsToStrands::GuideKind), u32 strands,
        u32 pointCount, f32[3] path[pointCount]                     (optional: may be absent)
"""

GUIDE_KINDS = ("free", "rooted", "merged", "bridged", "continued", "dropped")

import struct

import numpy as np


def write_mesh(path, positions, uvs, indices, normals=None, bone_indices=None, bone_weights=None,
               bones=(("NPC Head [Head]", (0.0, 0.0, 0.0)),), alpha=None, shade=None, flow=None):
    positions = np.asarray(positions, np.float32).reshape(-1, 3)
    n = len(positions)
    uvs = np.asarray(uvs, np.float32).reshape(n, 2)
    indices = np.asarray(indices, np.uint32).reshape(-1)
    if bone_indices is None:
        bone_indices = np.zeros((n, 4), np.uint16)
    if bone_weights is None:
        bone_weights = np.zeros((n, 4), np.float32)
        bone_weights[:, 0] = 1.0
    with open(path, "wb") as f:
        f.write(struct.pack("<6I", 0x4D535443, 1, n, len(indices), len(bones), normals is not None))
        f.write(positions.tobytes())
        if normals is not None:
            f.write(np.asarray(normals, np.float32).reshape(n, 3).tobytes())
        f.write(uvs.tobytes())
        f.write(np.asarray(bone_indices, np.uint16).reshape(n, 4).tobytes())
        f.write(np.asarray(bone_weights, np.float32).reshape(n, 4).tobytes())
        f.write(indices.tobytes())
        for name, pos in bones:
            data = name.encode()
            f.write(struct.pack("<I", len(data)) + data + struct.pack("<3f", *pos))
        if alpha is None:
            f.write(struct.pack("<2I", 0, 0))
        else:
            h, w = alpha.shape
            f.write(struct.pack("<2I", w, h))
            f.write(np.asarray(alpha, np.uint8).tobytes())
            f.write(np.asarray(shade if shade is not None else alpha, np.uint8).tobytes())
        if flow is None:
            f.write(struct.pack("<2I", 0, 0))
        else:
            h, w, _ = flow.shape
            f.write(struct.pack("<2I", w, h))
            f.write(np.asarray(flow, np.uint8).tobytes())


def read_result(path):
    with open(path, "rb") as f:
        data = f.read()
    magic, version, points, strands, guides = struct.unpack_from("<5I", data, 0)
    assert magic == 0x52535443 and version == 1
    off = 20
    point_dtype = np.dtype([("position", "<f4", 3), ("normal", "<f4", 3), ("uv", "<f4", 2), ("t", "<f4"),
                            ("bones", "<u2", 4), ("weights", "<f4", 4)])
    strand_dtype = np.dtype([("length", "<f4"), ("random", "<f4"), ("guide", "<u4"), ("clumpRandom", "<f4"),
                             ("cardGuide", "<u4"), ("scalpRooted", "<u4")])
    pts = np.frombuffer(data, point_dtype, points * strands, off)
    off += pts.nbytes
    info = np.frombuffer(data, strand_dtype, strands, off)
    off += info.nbytes
    head_bone, = struct.unpack_from("<i", data, off)
    head_centre = np.array(struct.unpack_from("<3f", data, off + 4))
    head_radius, = struct.unpack_from("<f", data, off + 16)
    off += 20
    scalp_centre = np.array(struct.unpack_from("<3f", data, off))
    scalp_radius, scalp_fitted = struct.unpack_from("<fI", data, off + 12)
    off += 20
    radii = np.frombuffer(data, "<f4", 24 * 12, off).reshape(12, 24)
    off += radii.nbytes
    stats = struct.unpack_from("<10If", data, off)
    off += 44
    card_guides = []
    if off + 4 <= len(data):
        count, = struct.unpack_from("<I", data, off)
        off += 4
        for _ in range(count):
            kind, grown, n = struct.unpack_from("<3I", data, off)
            off += 12
            path = np.frombuffer(data, "<f4", n * 3, off).reshape(n, 3)
            off += path.nbytes
            card_guides.append({"kind": GUIDE_KINDS[kind], "strands": grown, "path": path})
    names = ("totalTriangles", "convertedTriangles", "cardGuides", "redundantGuides", "rootedGuides",
             "continuedGuides", "mergedGuides", "bridgedGuides", "droppedGuides", "seedingUsed", "flowMapShare")
    return {
        "pointsPerStrand": points,
        "positions": pts["position"].reshape(strands, points, 3),
        "uv": pts["uv"].reshape(strands, points, 2),
        "strands": info,
        "guideCount": guides,
        "headCentre": head_centre,
        "headRadius": head_radius,
        "scalpCentre": scalp_centre,
        "scalpRadius": scalp_radius,
        "scalpFitted": bool(scalp_fitted),
        "scalpRadii": radii,
        "stats": dict(zip(names, stats)),
        "cardGuides": card_guides,
    }
