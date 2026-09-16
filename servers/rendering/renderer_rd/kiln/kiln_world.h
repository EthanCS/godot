// Kiln engine integration. Engine licensing: LICENSE.txt.
// Imported algorithm provenance and redistribution limits: kiln/docs/gi-provenance.json.

#pragma once

#include "core/math/aabb.h"
#include "core/math/vector4.h"
#include "core/os/mutex.h"
#include "core/templates/hash_map.h"
#include "core/templates/rid.h"
#include "core/variant/variant.h"

namespace RendererRD {
// Immutable COW snapshots cross the main/render thread boundary. Static geometry,
// rigid transforms/materials, and lighting have independent revision counters.
struct KilnWorld {
	struct Geometry {
		PackedByteArray nodes;
		PackedByteArray triangles;
		Vector<Vector4> emitters;
		AABB bounds;
		uint32_t node_count = 0;
		uint32_t triangle_count = 0;
		float power = 0;
	};
	Geometry world;
	Geometry dynamic;
	uint64_t geometry_version = 0;
	uint64_t material_version = 0;
	uint64_t dynamic_version = 0;
	uint64_t light_version = 0;
	Vector3 sun_direction = Vector3(0, 1, 0);
	Vector3 sun_color = Vector3(1, 1, 1);
	float sun_energy = 1;
	float sky_energy = 1;
	float time_of_day = 0.175;
	Vector3 sky_horizon = Vector3(0.5, 0.58, 0.7), sky_zenith = Vector3(0.18, 0.28, 0.43);
	bool procedural_sky = true;
	bool enabled = true;
	PackedByteArray local_lights, light_grid;
	uint32_t local_light_count = 0;
	String capture_directory;
	uint64_t capture_request = 0, history_version = 0;
	int ao_quality = 3;
	int rays = 1;
	int samples = 256;

	static void report(RID p_environment, const Dictionary &p_statistics);
	static Dictionary statistics(RID p_environment);
	static void publish(RID p_environment, const KilnWorld &p_world);
	static bool read(RID p_environment, KilnWorld &r_world);
	static void remove(RID p_environment);
};
} //namespace RendererRD
