// Kiln adaptation of the SurfelPlus persistent surface cache and cell lookup.
// Original authors: Zhen Ren, Ruipeng Wang, Jinxiang Wang. Apache-2.0.
// Changes: std430 storage, sparse hashed cells, triangle anchors, bounded allocation.
// Attribution and original sources: thirdparty/surfelplus/.
// The viewport supplies a bounded capacity, rounded to a complete dispatch row.
#define SURFEL_CAPACITY uint(p.voxel_size.w)
const uint CELL_CAPACITY = 262144u;
const float CELL_SIZE = 0.25;
const uint INVALID_SURFEL = 0u; // linked lists store index + 1 so zero-fill clears them
struct Surfel {
	vec4 position_radius;
	vec4 normal_age;
	vec4 irradiance_samples;
	vec4 short_mean_vbbr;
	vec4 variance_inconsistency;
	uvec4 anchor; // source triangle + 1, last seen frame, alive, progressive sample index
	vec4 barycentric; // triangle barycentric u/v, signed plane offset, last sampled frame
	vec4 local_normal; // original raster normal in the source triangle frame
};
#ifdef STAGE_SURFEL_SPATIAL
layout(set = 0, binding = 36, std430) writeonly buffer ResolvedLighting {
	vec4 shared_samples[];
};
#endif
// Reflect actual stage access into RD/Metal resource dependencies. Readers must
// not advertise writes, which unnecessarily serialize independent GI passes.
#if defined(STAGE_SURFEL_UPDATE) || defined(STAGE_SURFEL_GENERATE) || defined(STAGE_SURFEL_INTEGRATE)
#define SURFEL_ACCESS
#else
#define SURFEL_ACCESS readonly
#endif
#if defined(STAGE_SURFEL_GRID) || defined(STAGE_SURFEL_GRID_PREFIX) || defined(STAGE_SURFEL_GRID_SCATTER)
#define CELL_HEAD_ACCESS
#else
#define CELL_HEAD_ACCESS readonly
#endif
#ifdef STAGE_SURFEL_GRID_SCATTER
#define CELL_LINK_ACCESS
#else
#define CELL_LINK_ACCESS readonly
#endif
#if defined(STAGE_SURFEL_GRID_PREFIX) || defined(STAGE_SURFEL_GRID_PREFIX_SUMS)
#define GRID_SUM_ACCESS
#else
#define GRID_SUM_ACCESS readonly
#endif
#ifdef STAGE_SURFEL_UPDATE
#define FREE_SLOT_ACCESS
#else
#define FREE_SLOT_ACCESS readonly
#endif
#if defined(STAGE_SURFEL_UPDATE) || defined(STAGE_SURFEL_GENERATE) || defined(STAGE_SURFEL_TRACE) || defined(STAGE_SURFEL_GRID_PREFIX_SUMS)
#define COUNTER_ACCESS
#else
#define COUNTER_ACCESS readonly
#endif
layout(set = 0, binding = 20, std430) SURFEL_ACCESS buffer Surfels {
	Surfel surfels[];
};
layout(set = 0, binding = 21, std430) CELL_HEAD_ACCESS buffer CellHeads {
	uvec2 cell_heads[]; // count, block-local exclusive offset
};
layout(set = 0, binding = 22, std430) CELL_LINK_ACCESS buffer CellLinks {
	uint cell_links[]; // compact indices, at most 27 references per surfel
};
layout(set = 0, binding = 35, std430) GRID_SUM_ACCESS buffer GridSums {
	uint grid_sums[];
};
layout(set = 0, binding = 23, std430) FREE_SLOT_ACCESS buffer FreeSlots {
	uint free_slots[];
};
layout(set = 0, binding = 24, std430) COUNTER_ACCESS buffer Counters {
	uint free_count;
	uint spawn_count;
	uint alive_count;
	uint traced_count;
	uint spawn_claims[]; // CELL_CAPACITY claims, then persistent previous occupancy
};
#undef SURFEL_ACCESS
#undef CELL_HEAD_ACCESS
#undef CELL_LINK_ACCESS
#undef GRID_SUM_ACCESS
#undef FREE_SLOT_ACCESS
#undef COUNTER_ACCESS
uint surfel_hash(uint x) {
	x ^= x >> 16;
	x *= 0x7feb352du;
	x ^= x >> 15;
	x *= 0x846ca68bu;
	return x ^ (x >> 16);
}
uint surfel_level(float radius) {
	return radius <= CELL_SIZE ? 0u : (radius <= CELL_SIZE * 2.0 ? 1u : 2u);
}
float cell_size(uint level) {
	return CELL_SIZE * float(1u << level);
}
uint cell_hash(ivec3 c, uint level) {
	return surfel_hash(uint(c.x) * 73856093u ^ uint(c.y) * 19349663u ^ uint(c.z) * 83492791u ^ level * 2654435761u) & (CELL_CAPACITY - 1u);
}
float random_float(inout uint seed) {
	seed = surfel_hash(seed + 0x9e3779b9u);
	return float(seed >> 8) * (1.0 / 16777216.0);
}
vec2 random_pair(inout uint seed) {
	float x = random_float(seed);
	return vec2(x, random_float(seed));
}
float surfel_weight(Surfel s, vec3 position, vec3 normal) {
	vec3 delta = position - s.position_radius.xyz;
	float radius = s.position_radius.w;
	float alignment = dot(normal, s.normal_age.xyz);
	// Surface-plane rejection is deliberately much tighter than radial support.
	// Opposite sides of a thin wall must never share an irradiance cache entry.
	if (alignment < 0.85 || abs(dot(delta, s.normal_age.xyz)) > max(0.012, radius * 0.06)) {
		return 0.0;
	}
	float w = max(0.0, 1.0 - length(delta) / radius) * max(0.0, alignment);
	return w * w * (3.0 - 2.0 * w);
}
vec4 surfel_gather(vec3 position, vec3 normal, bool coverage_only, out float coverage) {
	vec3 sum = vec3(0);
	float weight = 0.0;
	coverage = 0.0;
	for (uint level = 0u; level < 3u; level++) {
		uint key = cell_hash(ivec3(floor(position / cell_size(level))), level);
		uvec2 cell = cell_heads[key];
		uint offset = cell.y + grid_sums[key / 64u];
		// Every overlapping surfel is indexed here. No neighbor traversal and no
		// truncation: hash collisions only add candidates, never drop coverage.
		for (uint i = 0u; i < cell.x; i++) {
			uint id = cell_links[offset + i];
			vec4 sphere = surfels[id].position_radius;
			vec3 delta = position - sphere.xyz;
			float distance_squared = dot(delta, delta);
			if (surfel_level(sphere.w) != level || distance_squared >= sphere.w * sphere.w) continue;
			vec3 sn = surfels[id].normal_age.xyz;
			float alignment = dot(normal, sn);
			if (alignment < 0.85 || abs(dot(delta, sn)) > max(0.012, sphere.w * 0.06)) continue;
			float w = (1.0 - sqrt(distance_squared) / sphere.w) * max(0.0, alignment);
			w = w * w * (3.0 - 2.0 * w);
			coverage += w;
			if (!coverage_only) {
				vec4 irradiance = surfels[id].irradiance_samples;
				float confidence = smoothstep(0.0, 64.0, irradiance.w);

				sum += irradiance.rgb * w * confidence;
				weight += w * confidence;
			}
		}
	}
	// A newly allocated surfel covers geometry but has no lighting yet. Only
	// sampled contributors may terminate a secondary path at the cache.
	return vec4(weight > 1e-6 ? sum / weight : vec3(0), coverage_only ? coverage : weight);
}
vec4 surfel_gather(vec3 position, vec3 normal, bool coverage_only) {
	float coverage;
	return surfel_gather(position, normal, coverage_only, coverage);
}
