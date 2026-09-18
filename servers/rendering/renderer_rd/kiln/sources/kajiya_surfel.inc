// kajiya restir-meets-surfel surfel pool, clipmap grid and lookup, ported for the
// parity comparison. Sources: kajiya assets/shaders/surfel_gi/{surfel_constants,
// surfel_grid_hash,surfel_binning_shared,lookup}.hlsl. The half-migrated hash table
// of the reference is reduced to its effective modulo behavior.
//
// Buffer binding contract shared by every KAJIYA_* stage (set 0), kept clear of
// the bvh.inc and kiln surfel bindings:
//  40 surfel_meta_buf       8 x u32    [1] live count, [2] alloc pointer
//  41 surfel_pool_buf       u32 x MAX  LIFO free-index stack
//  42 cell_index_offset_buf u32 x MAX+1 per-cell inclusive prefix sum
//  43 surfel_index_buf      u32 x MAX*24 sorted surfel ids per cell
//  44 surfel_spatial_buf    vec4 x MAX xyz = position, w bits = packed normal 11:10:11
//  45 surfel_irradiance_buf vec4 x MAX rgb = irradiance, a = accumulated samples
//  46 surfel_aux_buf        vec4 x MAX*2 [2i] blended radiance + count, [2i] MSME state
//  47 surfel_life_buf       u32 x MAX  age; valid < 100
//  48 surfel_reposition_proposal_buf vec4 x MAX next trace origin proposal

#define KAJIYA_MAX_SURFEL_CELLS 262144u
#define KAJIYA_MAX_SURFELS 262144u
#define KAJIYA_MAX_SURFELS_PER_CELL 128u
#define KAJIYA_MAX_SURFELS_PER_CELL_FOR_KEEP_ALIVE 32u
#define KAJIYA_SURFELS_PER_CELL_SLOTS 24u
#define KAJIYA_SURFEL_CS 32u

const float KAJIYA_SURFEL_GRID_CELL_DIAMETER = 0.2;
const float KAJIYA_SURFEL_BASE_RADIUS = 0.24;
const float KAJIYA_SURFEL_RADIUS_OVERSCALE = 1.25; // [sic] reference typo preserved
const float KAJIYA_SURFEL_NORMAL_DIRECTION_SQUISH = 2.0;

const uint KAJIYA_SURFEL_META_SURFEL_COUNT = 4u;
const uint KAJIYA_SURFEL_META_ALLOC_COUNT = 8u;
const uint KAJIYA_SURFEL_LIFE_RECYCLE = 0x8000000u;
const uint KAJIYA_SURFEL_LIFE_RECYCLED = KAJIYA_SURFEL_LIFE_RECYCLE + 1u;

bool kajiya_is_surfel_life_valid(uint life) {
	return life < 100u;
}

bool kajiya_surfel_life_needs_aging(uint life) {
	return life != KAJIYA_SURFEL_LIFE_RECYCLED;
}

// Access qualifiers reflect each stage's use into RD dependencies, following the
// kiln sources convention. The meta buffer is read-write everywhere: the count
// is read by most stages while allocation and aging issue atomics on it.
#define KAJIYA_META_ACCESS
#ifdef STAGE_KAJIYA_SURFEL_CLEAR_POOL
#define KAJIYA_POOL_ACCESS writeonly
#elif defined(STAGE_KAJIYA_SURFEL_ALLOCATE) || defined(STAGE_KAJIYA_SURFEL_AGE)
#define KAJIYA_POOL_ACCESS
#else
#define KAJIYA_POOL_ACCESS readonly
#endif
#if defined(STAGE_KAJIYA_SURFEL_CLEAR_CELLS) || defined(STAGE_KAJIYA_SURFEL_COUNT_CELLS) || defined(STAGE_KAJIYA_SURFEL_SLOT_CELLS)
#define KAJIYA_CELL_ACCESS
#else
#define KAJIYA_CELL_ACCESS readonly
#endif
#ifdef STAGE_KAJIYA_SURFEL_SLOT_CELLS
#define KAJIYA_INDEX_ACCESS
#else
#define KAJIYA_INDEX_ACCESS readonly
#endif
#if defined(STAGE_KAJIYA_SURFEL_ALLOCATE)
#define KAJIYA_SPATIAL_ACCESS
#else
#define KAJIYA_SPATIAL_ACCESS readonly
#endif
#if defined(STAGE_KAJIYA_SURFEL_ALLOCATE) || defined(STAGE_KAJIYA_SURFEL_TRACE)
#define KAJIYA_IRRADIANCE_ACCESS
#else
#define KAJIYA_IRRADIANCE_ACCESS readonly
#endif
#if defined(STAGE_KAJIYA_SURFEL_TRACE) || defined(STAGE_KAJIYA_SURFEL_ALLOCATE)
#define KAJIYA_AUX_ACCESS
#else
#define KAJIYA_AUX_ACCESS readonly
#endif
#define KAJIYA_LIFE_ACCESS
#if defined(STAGE_KAJIYA_SURFEL_FIND_MISSING) || defined(STAGE_KAJIYA_SURFEL_ALLOCATE)
#define KAJIYA_PROPOSAL_ACCESS
#else
#define KAJIYA_PROPOSAL_ACCESS readonly
#endif

layout(set = 0, binding = 40, std430) KAJIYA_META_ACCESS buffer KajiyaMetaBuf { uint kajiya_surfel_meta_buf[]; };
layout(set = 0, binding = 41, std430) KAJIYA_POOL_ACCESS buffer KajiyaPoolBuf { uint kajiya_surfel_pool_buf[]; };
layout(set = 0, binding = 42, std430) KAJIYA_CELL_ACCESS buffer KajiyaCellOffsetBuf { uint kajiya_cell_index_offset_buf[]; };
layout(set = 0, binding = 43, std430) KAJIYA_INDEX_ACCESS buffer KajiyaIndexBuf { uint kajiya_surfel_index_buf[]; };
layout(set = 0, binding = 44, std430) KAJIYA_SPATIAL_ACCESS buffer KajiyaSpatialBuf { vec4 kajiya_surfel_spatial_buf[]; };
layout(set = 0, binding = 45, std430) KAJIYA_IRRADIANCE_ACCESS buffer KajiyaIrradianceBuf { vec4 kajiya_surfel_irradiance_buf[]; };
layout(set = 0, binding = 46, std430) KAJIYA_AUX_ACCESS buffer KajiyaAuxBuf { vec4 kajiya_surfel_aux_buf[]; };
layout(set = 0, binding = 47, std430) KAJIYA_LIFE_ACCESS buffer KajiyaLifeBuf { uint kajiya_surfel_life_buf[]; };
layout(set = 0, binding = 48, std430) KAJIYA_PROPOSAL_ACCESS buffer KajiyaProposalBuf { vec4 kajiya_surfel_reposition_proposal_buf[]; };

vec3 kajiya_surfel_position(uint idx) {
	return kajiya_surfel_spatial_buf[idx].xyz;
}

vec3 kajiya_surfel_normal(uint idx) {
	return kajiya_unpack_normal_11_10_11_no_normalize(floatBitsToUint(kajiya_surfel_spatial_buf[idx].w));
}

// ---- surfel_grid_hash.hlsl ----
vec3 kajiya_get_eye_position() {
	return p.inv_view[3].xyz;
}

vec3 kajiya_get_prev_eye_position() {
	return p.kajiya_prev_eye.xyz;
}

ivec3 kajiya_surfel_pos_to_grid_coord(vec3 pos, vec3 eye_pos) {
	return ivec3(floor((pos - eye_pos) / KAJIYA_SURFEL_GRID_CELL_DIAMETER));
}

vec3 kajiya_surfel_grid_coord_center(uvec4 coord, vec3 eye_pos) {
	return eye_pos + ((vec3(coord.xyz) + 0.5 - float(KAJIYA_SURFEL_CS) * 0.5) * KAJIYA_SURFEL_GRID_CELL_DIAMETER) * float(1u << coord.w);
}

float kajiya_surfel_grid_coord_to_cascade_float(ivec3 coord) {
	const vec3 fcoord = vec3(coord) + 0.5;
	const float max_coord = max(abs(fcoord.x), max(abs(fcoord.y), abs(fcoord.z)));
	return log2(max_coord / (float(KAJIYA_SURFEL_CS) * 0.5));
}

uint kajiya_cascade_float_to_cascade(float cascade_float) {
	return uint(clamp(ceil(max(0.0, cascade_float)), 0.0, 7.0));
}

uint kajiya_surfel_grid_coord_to_cascade(ivec3 coord) {
	return kajiya_cascade_float_to_cascade(kajiya_surfel_grid_coord_to_cascade_float(coord));
}

float kajiya_surfel_radius_for_pos(vec3 pos) {
	return KAJIYA_SURFEL_BASE_RADIUS * max(1.0, length(pos - kajiya_get_eye_position()) / (KAJIYA_SURFEL_GRID_CELL_DIAMETER * float(KAJIYA_SURFEL_CS) * 0.5));
}

ivec3 kajiya_surfel_grid_coord_within_cascade(ivec3 coord, uint cascade) {
	return (coord >> int(cascade)) + int(KAJIYA_SURFEL_CS / 2);
}

uvec4 kajiya_surfel_grid_coord_to_c4(ivec3 coord) {
	const uint cascade = kajiya_surfel_grid_coord_to_cascade(coord);
	const uvec3 ucoord_in_cascade = clamp(uvec3(kajiya_surfel_grid_coord_within_cascade(coord, cascade)), uvec3(0.0), uvec3(KAJIYA_SURFEL_CS - 1u));
	return uvec4(ucoord_in_cascade, cascade);
}

uint kajiya_surfel_grid_c4_to_hash(uvec4 c4) {
	// GLSL has no integer dot(); the reference relies on HLSL's uint4 dot.
	return c4.x + c4.y * KAJIYA_SURFEL_CS + c4.z * (KAJIYA_SURFEL_CS * KAJIYA_SURFEL_CS) + c4.w * (KAJIYA_SURFEL_CS * KAJIYA_SURFEL_CS * KAJIYA_SURFEL_CS);
}

uint kajiya_surfel_grid_coord_to_hash(ivec3 coord) {
	return kajiya_surfel_grid_c4_to_hash(kajiya_surfel_grid_coord_to_c4(coord)) % KAJIYA_MAX_SURFEL_CELLS;
}

uint kajiya_cell_range_begin(uint cell_idx) {
	return kajiya_cell_index_offset_buf[cell_idx];
}

uint kajiya_cell_range_end(uint cell_idx) {
	return kajiya_cell_index_offset_buf[cell_idx + 1u];
}

// ---- surfel_binning_shared.hlsl ----
uint kajiya_surfel_cascade_for_pos(vec3 position) {
	const float fc = kajiya_surfel_grid_coord_to_cascade_float(kajiya_surfel_pos_to_grid_coord(position, kajiya_get_eye_position()));
	return kajiya_cascade_float_to_cascade(fc);
}

bool kajiya_surfel_intersects_grid_coord(vec3 position, vec3 normal, uvec4 grid_coord) {
	const float surfel_radius = kajiya_surfel_radius_for_pos(position);

	const vec3 cell_center = kajiya_surfel_grid_coord_center(grid_coord, kajiya_get_eye_position());
	const float grid_cell_radius = (KAJIYA_SURFEL_GRID_CELL_DIAMETER * 0.5) * float(1u << grid_coord.w);

	const vec3 cell_local_surfel_pos = position - cell_center;
	const vec3 cell_local_closest_point_on_grid_cell = clamp(cell_local_surfel_pos, vec3(-grid_cell_radius), vec3(grid_cell_radius));

	const vec3 pos_offset = cell_local_surfel_pos - cell_local_closest_point_on_grid_cell;

	// Approximate box-ellipsoid culling. Sometimes misses corners, but greatly
	// improves culling efficiency (reference preserves this behavior).
	const float mahalanobis_dist = length(pos_offset) * (1.0 + abs(dot(pos_offset, normal)) * KAJIYA_SURFEL_NORMAL_DIRECTION_SQUISH);
	return mahalanobis_dist < surfel_radius;
}

struct KajiyaSurfelGridMinMax {
	uvec4 c4_min[2];
	uvec4 c4_max[2];
	uint cascade_count;
};

KajiyaSurfelGridMinMax kajiya_get_surfel_grid_box_min_max(vec3 position) {
	const float surfel_radius = kajiya_surfel_radius_for_pos(position);

	const vec3 box_min_pos = position - surfel_radius;
	const vec3 box_max_pos = position + surfel_radius;

	const vec3 eye_pos = kajiya_get_eye_position();
	const float fc = kajiya_surfel_grid_coord_to_cascade_float(kajiya_surfel_pos_to_grid_coord(position, eye_pos));

	const uint c0 = kajiya_cascade_float_to_cascade(fc - 0.2);
	const uint c1 = kajiya_cascade_float_to_cascade(fc + 0.2);

	const ivec3 min_coord = kajiya_surfel_pos_to_grid_coord(box_min_pos, eye_pos);
	const ivec3 max_coord = kajiya_surfel_pos_to_grid_coord(box_max_pos, eye_pos);

	KajiyaSurfelGridMinMax result;

	result.cascade_count = 1u;
	result.c4_min[0] = uvec4(clamp(uvec3(kajiya_surfel_grid_coord_within_cascade(min_coord, c0)), uvec3(0.0), uvec3(KAJIYA_SURFEL_CS - 1u)), c0);
	result.c4_max[0] = uvec4(clamp(uvec3(kajiya_surfel_grid_coord_within_cascade(max_coord, c0)), uvec3(0.0), uvec3(KAJIYA_SURFEL_CS - 1u)), c0);

	if (c1 != c0) {
		result.cascade_count = 2u;
		result.c4_min[1] = uvec4(clamp(uvec3(kajiya_surfel_grid_coord_within_cascade(min_coord, c1)), uvec3(0.0), uvec3(KAJIYA_SURFEL_CS - 1u)), c1);
		result.c4_max[1] = uvec4(clamp(uvec3(kajiya_surfel_grid_coord_within_cascade(max_coord, c1)), uvec3(0.0), uvec3(KAJIYA_SURFEL_CS - 1u)), c1);
	}

	return result;
}

// ---- lookup.hlsl ----
vec3 kajiya_lookup_surfel_gi(vec3 pt_ws, vec3 normal_ws) {
	const uint cell_idx = kajiya_surfel_grid_coord_to_hash(kajiya_surfel_pos_to_grid_coord(pt_ws, kajiya_get_eye_position()));

	uint range_begin = kajiya_cell_range_begin(cell_idx);
	uint range_end = kajiya_cell_range_end(cell_idx);
	// TEMP HACK (reference): never iterate over tons of surfels out of bounds.
	range_end = min(range_end, range_begin + KAJIYA_MAX_SURFELS_PER_CELL);

	vec3 total_color = vec3(0.0);
	float total_weight = 0.0;

	for (uint surfel_idx_loc = range_begin; surfel_idx_loc < range_end; ++surfel_idx_loc) {
		const uint surfel_idx = kajiya_surfel_index_buf[surfel_idx_loc];

#ifndef KAJIYA_SURFEL_LOOKUP_DONT_KEEP_ALIVE
		if (range_end - range_begin <= KAJIYA_MAX_SURFELS_PER_CELL_FOR_KEEP_ALIVE) {
			kajiya_surfel_life_buf[surfel_idx] = 0u;
		}
#endif

		const vec3 surfel_position = kajiya_surfel_position(surfel_idx);
		const vec3 surfel_normal = kajiya_surfel_normal(surfel_idx);
		const vec4 surfel_irradiance_packed = kajiya_surfel_irradiance_buf[surfel_idx];

		const vec3 pos_offset = pt_ws - surfel_position;
		const float directional_weight = max(0.0, dot(surfel_normal, normal_ws));
		const float mahalanobis_dist = length(pos_offset) * (1.0 + abs(dot(pos_offset, surfel_normal)) * KAJIYA_SURFEL_NORMAL_DIRECTION_SQUISH);

		const float surfel_radius = kajiya_surfel_radius_for_pos(surfel_position);
		const float weight = smoothstep(surfel_radius * KAJIYA_SURFEL_RADIUS_OVERSCALE, 0.0, mahalanobis_dist) * directional_weight;

		total_weight += weight;
		total_color += surfel_irradiance_packed.xyz * weight;
	}

	total_color /= max(0.1, total_weight);
	return total_color;
}
