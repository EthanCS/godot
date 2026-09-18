// Kiln engine integration. Engine licensing: LICENSE.txt.
// SurfelPlus adaptation: servers/rendering/renderer_rd/kiln/licenses/SURFELPLUS-NOTICE.txt and kiln/docs/SURFEL-GI.md.

#include "kiln_gi.h"

#include "core/config/project_settings.h"
#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/io/json.h"
#include "core/os/os.h"
#ifdef METAL_ENABLED
#include "drivers/metal/kiln_specular_native.metal.gen.h"
#endif
#include "servers/rendering/renderer_rd/storage_rd/material_storage.h"
#include "servers/rendering/renderer_rd/uniform_set_cache_rd.h"
#include "servers/rendering/rendering_server_globals.h"
#include "servers/rendering/storage/utilities.h"

using namespace RendererRD;
#define kiln_scope SNAME("kiln_gi")
RID KilnGI::texture(Size2i size, RD::DataFormat format) {
	RD::TextureFormat f;
	f.width = size.x;
	f.height = size.y;
	f.format = format;
	f.usage_bits = RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_STORAGE_BIT | RD::TEXTURE_USAGE_CAN_COPY_FROM_BIT | RD::TEXTURE_USAGE_CAN_COPY_TO_BIT;
	RID rid = RD::get_singleton()->texture_create(f, RD::TextureView());
	RD::get_singleton()->texture_clear(rid, Color(0, 0, 0, 0), 0, 1, 0, 1);
	return rid;
}
KilnGI::KilnGI() {
	Vector<String> defines;
	const char *stages[] = { "SURFEL_UPDATE", "SURFEL_GRID", "SURFEL_GENERATE", "SURFEL_TRACE", "SURFEL_INTEGRATE", "SURFEL_EVALUATE", "SURFEL_PUBLISH", "SURFEL_SPECULAR", "SURFEL_SPECULAR_FILTER", "SURFEL_DEBUG", "SEQUENCE_LUT", "XEGTAO_DEPTH", "XEGTAO_MAIN", "XEGTAO_DENOISE", "XEGTAO_TEMPORAL", "BVH_REFIT", "SURFEL_GRID_PREFIX", "SURFEL_GRID_PREFIX_SUMS", "SURFEL_GRID_SCATTER", "SURFEL_DIFFUSE_FILTER", "NRD_PREPARE", "NRD_DIFFUSE", "NRD_RESOLVE", "SURFEL_SPATIAL", "SURFEL_SCHEDULE", "KAJIYA_SKY", "KAJIYA_SURFEL_CLEAR_POOL", "KAJIYA_SURFEL_FIND_MISSING", "KAJIYA_SURFEL_ARGS", "KAJIYA_SURFEL_AGE", "KAJIYA_SURFEL_ALLOCATE", "KAJIYA_SURFEL_CLEAR_CELLS", "KAJIYA_SURFEL_COUNT_CELLS", "KAJIYA_SURFEL_SCAN", "KAJIYA_SURFEL_SCAN_SEGMENTS", "KAJIYA_SURFEL_SCAN_MERGE", "KAJIYA_SURFEL_SLOT_CELLS", "KAJIYA_SURFEL_TRACE", "KAJIYA_SURFEL_RESOLVE" };
	for (int i = 0; i < STAGE_COUNT; i++) {
		defines.push_back(String("\n#define STAGE_") + stages[i] + "\n");
	}
	shader.initialize(defines);
	version = shader.version_create();
	shader.version_set_compute_code(version, HashMap<String, String>(), "", "", Vector<String>());
	for (int i = 0; i < STAGE_COUNT; i++) {
		pipelines[i] = RD::get_singleton()->compute_pipeline_create(shader.version_get_shader(version, i));
	}
	hardware_available = RD::get_singleton()->has_feature(RD::SUPPORTS_RAY_QUERY);
	if (hardware_available) {
		Vector<String> hardware_defines;
		for (const char *stage : { "SURFEL_GENERATE", "SURFEL_TRACE", "QUERY_VALIDATE", "SURFEL_SPECULAR", "NRD_DIFFUSE", "KAJIYA_SURFEL_TRACE" }) {
			hardware_defines.push_back(String("\n#define KILN_HARDWARE_RAY_QUERY\n#define STAGE_") + stage + "\n");
		}
		hardware_shader.initialize(hardware_defines);
		hardware_version = hardware_shader.version_create();
		hardware_shader.version_set_compute_code(hardware_version, HashMap<String, String>(), "", "", Vector<String>());
		for (int i = 0; i < 6; i++) {
			RID code = hardware_shader.version_get_shader(hardware_version, i);
			if (code.is_valid()) {
				hardware_pipelines[i] = RD::get_singleton()->compute_pipeline_create(code);
			}
			hardware_available &= hardware_pipelines[i].is_valid();
		}
	}
#ifdef METAL_ENABLED
	if (hardware_available && OS::get_singleton()->get_current_rendering_driver_name() == "metal" && bool(ProjectSettings::get_singleton()->get_setting("rendering/kiln/metal_native_specular", false))) {
		Vector<String> native_defines;
		// Updating MSL invalidates the ShaderRD cache even when its descriptor
		// metadata is unchanged. Native and translated variants have distinct IDs.
		native_defines.push_back(String("\n#define KILN_NATIVE_MSL_") + KILN_NATIVE_SPECULAR_SHA256 + "\n");
		native_specular_shader.initialize(native_defines);
		native_specular_version = native_specular_shader.version_create();
		native_specular_shader.version_set_compute_code(native_specular_version, HashMap<String, String>(), "", "", Vector<String>());
		print_line("[KILN_GI] specular implementation=handwritten_msl");
	} else
#endif
	{
		print_line("[KILN_GI] specular implementation=translated_glsl");
	}
	specular_pipelines[0][1] = pipelines[SURFEL_SPECULAR];
	specular_pipelines[1][1] = hardware_pipelines[3];
	RD::SamplerState state;
	sampler = RD::get_singleton()->sampler_create(state);
	state.min_filter = state.mag_filter = RD::SAMPLER_FILTER_LINEAR;
	state.repeat_u = state.repeat_v = RD::SAMPLER_REPEAT_MODE_REPEAT;
	linear_sampler = RD::get_singleton()->sampler_create(state);
	sequence = texture(Size2i(128, 128), RD::DATA_FORMAT_R32G32B32A32_SFLOAT);
	hilbert = texture(Size2i(64, 64), RD::DATA_FORMAT_R16_UINT);
	empty_surface = texture(Size2i(1, 1), RD::DATA_FORMAT_R32G32_UINT);
	print_line(vformat("[KILN_GI] hardware ray query=%s; compute software BVH fallback available", hardware_available));
}
KilnGI::~KilnGI() {
	if (native_specular_version.is_valid()) {
		native_specular_shader.version_free(native_specular_version);
	}
	shader.version_free(version);
	if (hardware_version.is_valid()) {
		hardware_shader.version_free(hardware_version);
	}
	for (RID rid : { sampler, linear_sampler, sequence, hilbert, empty_surface }) {
		RD::get_singleton()->free_rid(rid);
	}
}
void KilnGI::View::free_hardware() {
	RD *rd = RD::get_singleton();
	if (hardware_tlas.is_valid()) {
		rd->free_rid(hardware_tlas);
		hardware_tlas = RID();
	}
	for (int i = 0; i < 2; i++) {
		if (hardware_blas[i].is_valid()) {
			rd->free_rid(hardware_blas[i]);
			hardware_blas[i] = RID();
		}
		if (hardware_vertices[i].is_valid()) {
			rd->free_rid(hardware_vertices[i]);
			hardware_vertices[i] = RID();
		}
	}
	hardware_active = false;
}
void KilnGI::View::free_data() {
	// RenderSceneBuffersRD calls this on viewport reconfiguration, including
	// resize. Only screen-space resources depend on that configuration.
	if (nrd) {
		memdelete(nrd);
		nrd = nullptr;
	}
	nrd_active = false;
	for (RID rid : owned) {
		if (rid.is_valid()) {
			RD::get_singleton()->free_rid(rid);
		}
	}
	owned.clear();
	textures.clear();
	index = ao_frames = 0;
	ready = false;
}
void KilnGI::View::free_cache() {
	free_data();
	free_hardware();
	if (ray_albedo.is_valid()) {
		RD::get_singleton()->free_rid(ray_albedo);
		ray_albedo = RID();
	}
	hardware_failed = false;
	hardware_builds = tlas_builds = 0;
	if (parameters.is_valid()) {
		RD::get_singleton()->free_rid(parameters);
		parameters = RID();
	}
	for (const KeyValue<String, RID> &entry : storage) {
		RD::get_singleton()->free_rid(entry.value);
	}
	storage.clear();
	capacities.clear();
	frames = index = stationary_samples = 0;
	motion_remaining = 0;
	lighting_response = 0;
	previous_local_lights.clear();
	geometry_version = dynamic_version = light_version = material_version = texture_version = 0;
	static_material_version = dynamic_material_version = 0;
	ready = false;
}
void KilnGI::dispatch(Stage stage, Size2i size, const std::vector<Binding> &bindings, int stride, int z, RID tlas, bool force_translated, Vector2 jitter_delta) {
	RD *rd = RD::get_singleton();
	static const char *stage_names[] = { "Update", "Grid", "Generate", "Trace diffuse", "Integrate", "Evaluate", "Publish diffuse", "Trace specular", "Filter specular", "Debug", "Sequence", "AO depth", "AO main", "AO denoise", "AO temporal", "BVH refit", "Grid prefix", "Grid prefix sums", "Grid scatter", "Filter diffuse", "NRD prepare", "NRD diffuse rays", "NRD resolve", "Share irradiance", "Schedule rays", "Query validation", "Kajiya sky", "Kajiya surfel pool init", "Kajiya find missing surfels", "Kajiya surfel args", "Kajiya age surfels", "Kajiya allocate surfels", "Kajiya clear cells", "Kajiya count surfels per cell", "Kajiya prefix scan", "Kajiya prefix scan segments", "Kajiya prefix scan merge", "Kajiya slot surfels into cells", "Kajiya trace irradiance", "Kajiya surfel resolve" };
	RENDER_TIMESTAMP(String("Kiln / ") + stage_names[stage]);
	LocalVector<RD::Uniform> uniforms;
	const uint64_t surfel_data = (1ull << 0) | (31ull << 20) | (1ull << 35);
	const uint64_t bvh_data = (1ull << 4) | (1ull << 5) | (7ull << 16) | (3ull << 25) | (7ull << 32);
	uint64_t stage_mask = ~0ull;
	switch (stage) {
		case SURFEL_UPDATE:
			stage_mask = surfel_data | bvh_data | (1ull << 1);
			break;
		case SURFEL_GRID:
		case SURFEL_GRID_PREFIX:
		case SURFEL_GRID_PREFIX_SUMS:
		case SURFEL_GRID_SCATTER:
			stage_mask = surfel_data;
			break;
		case SURFEL_GENERATE:
			stage_mask = surfel_data | bvh_data | (3ull << 1) | (1ull << 31);
			break;
		case SURFEL_TRACE:
			stage_mask = surfel_data | bvh_data | (1ull << 28) | (1ull << 37) | (1ull << 38) | (1ull << 39);
			break;
		case SURFEL_INTEGRATE:
			stage_mask = surfel_data | (1ull << 28) | (1ull << 36);
			break;
		case SURFEL_EVALUATE:
			stage_mask = surfel_data | (3ull << 1) | (1ull << 6) | (1ull << 8) | (1ull << 31);
			break;
		case SURFEL_SCHEDULE:
			stage_mask = surfel_data | (1ull << 39);
			break;
		case SURFEL_SPATIAL:
			stage_mask = surfel_data | (1ull << 28) | (1ull << 36) | (1ull << 38);
			break;
		default:
			break;
	}
	for (const Binding &b : bindings) {
		if (!(stage_mask & (1ull << b.binding))) {
			continue;
		}
		RD::Uniform u;
		u.binding = b.binding;
		u.uniform_type = b.type;
		if (b.type == RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE) {
			u.append_id(b.linear ? linear_sampler : sampler);
		}
		u.append_id(b.resource);
		uniforms.push_back(u);
	}
	RID code, pipeline;
	if (tlas.is_valid()) {
		int variant = 2; // general query stage
		if (stage == NRD_DIFFUSE) {
			variant = 4;
		} else if (stage == KAJIYA_SURFEL_TRACE) {
			variant = 5;
		} else if (stage == SURFEL_GENERATE) {
			variant = 0;
		} else if (stage == SURFEL_TRACE) {
			variant = 1;
		} else if (stage == SURFEL_SPECULAR) {
			variant = 3;
		}
		RD::Uniform u;
		u.binding = 27;
		u.uniform_type = RD::UNIFORM_TYPE_ACCELERATION_STRUCTURE;
		u.append_id(tlas);
		uniforms.push_back(u);
		code = hardware_shader.version_get_shader(hardware_version, variant);
		pipeline = hardware_pipelines[variant];
	} else {
		code = shader.version_get_shader(version, stage);
		pipeline = pipelines[stage];
	}
	if (stage == SURFEL_SPECULAR) {
		uint32_t rays = CLAMP(stride, 1, 8);
		int implementation = tlas.is_valid() ? 1 : 0;
		if (tlas.is_valid() && native_specular_version.is_valid() && !force_translated) {
			code = native_specular_shader.version_get_shader(native_specular_version, 0);
			implementation = 2;
		}
		RID &specialized = specular_pipelines[implementation][rays - 1];
		if (!specialized.is_valid()) {
			RD::PipelineSpecializationConstant count;
			count.type = RD::PIPELINE_SPECIALIZATION_CONSTANT_TYPE_INT;
			count.constant_id = 0;
			count.int_value = rays;
			Vector<RD::PipelineSpecializationConstant> constants;
			constants.push_back(count);
			specialized = rd->compute_pipeline_create(code, constants);
		}
		pipeline = specialized;
	}
	RID set = UniformSetCacheRD::get_singleton()->get_cache_vec(code, 0, uniforms);
	RD::ComputeListID list = rd->compute_list_begin();
	rd->compute_list_bind_compute_pipeline(list, pipeline);
	rd->compute_list_bind_uniform_set(list, set, 0);
	if (stage == BVH_REFIT || stage == AO_DEPTH || stage == AO_DENOISE || stage == AO_TEMPORAL || stage == SURFEL_DIFFUSE_FILTER || stage == SURFEL_PUBLISH || stage == NRD_RESOLVE) {
		int push[4] = { stride, size.x, 0, 0 };
		rd->compute_list_set_push_constant(list, push, (stage == BVH_REFIT) ? sizeof(push) : sizeof(int));
	}
	if (stage == NRD_PREPARE) {
		float jitter[3] = { float(jitter_delta.x), float(jitter_delta.y), float(stride) };
		rd->compute_list_set_push_constant(list, jitter, sizeof(jitter));
	}
	uint32_t group_width = stage == SURFEL_SPECULAR ? 16 : 8;
	uint32_t group_height = stage == SURFEL_SPECULAR ? 2 : 8;
	if (stage >= KAJIYA_SKY) {
		// kajiya stages declare their own local sizes; `size` is given in workgroups.
		group_width = 1;
		group_height = 1;
	}
	rd->compute_list_dispatch(list, (size.x + group_width - 1) / group_width, (size.y + group_height - 1) / group_height, z);
	rd->compute_list_end();
	RENDER_TIMESTAMP("Kiln / between passes");
}
bool KilnGI::process(Ref<RenderSceneBuffersRD> buffers, RenderSceneDataRD *scene, RID environment, RID full_normal, RID dfg, bool signed_normal) {
	KilnWorld world;
	if (!KilnWorld::read(environment, world) || (!world.enabled && world.ao_quality == 0) || world.geometry_version == 0) {
		return false;
	}
	RD *rd = RD::get_singleton();
	if (!buffers->has_custom_data(kiln_scope)) {
		Ref<View> data;
		data.instantiate();
		buffers->set_custom_data(kiln_scope, data);
	}
	Ref<View> state = buffers->get_custom_data(kiln_scope);
	if (state->environment != environment) {
		state->free_cache();
		state->environment = environment;
	}
	auto own = [&](RID rid) { state->owned.push_back(rid); return rid; };
	auto allocate = [&](String name, uint32_t bytes, bool preserve = false) {
		if (!state->storage.has(name) || state->capacities[name] < bytes) {
			RID old = state->storage.has(name) ? state->storage[name] : RID();
			uint32_t old_capacity = state->capacities.has(name) ? state->capacities[name] : 0;
			state->capacities[name] = preserve ? bytes : MAX(bytes, old_capacity ? old_capacity * 2 : 16u);
			RID buffer = rd->storage_buffer_create(state->capacities[name]);
			if (preserve) {
				rd->buffer_clear(buffer, old_capacity, state->capacities[name] - old_capacity);
				if (old.is_valid()) {
					rd->buffer_copy(old, buffer, 0, 0, old_capacity);
				}
			}
			state->storage[name] = buffer;
			if (old.is_valid()) {
				rd->free_rid(old);
			}
		}
		return state->storage[name];
	};
	auto upload = [&](String name, const PackedByteArray &bytes) { RID rid = allocate(name, bytes.size()); rd->buffer_update(rid, 0, bytes.size(), bytes.ptr()); return rid; };
	bool initialize = !state->ready;
	bool initialize_cache = !state->parameters.is_valid();
	if (initialize) {
		state->size = buffers->get_internal_size();
		// Keep the cache density consistent as the full-resolution receiver count
		// grows. Workgroups address rows of 256 slots; bound memory at 32 MiB.
		uint32_t requested_slots = ((uint64_t(state->size.x) * state->size.y + 2047) / 2048) * 256;
		// Grow without replacing existing surfels; shrinking the viewport does
		// not discard transport, sample sequences or the hardware scene.
		state->slots = MAX(state->slots, CLAMP(requested_slots, 65536u, 262144u));
		if (initialize_cache) {
			state->parameters = rd->uniform_buffer_create(736);
		}
		// Persistent world-space surfels. Screen textures contain only the resolve
		// and geometry history; there are no screen probes or legacy SH caches.
		for (const char *name : { "raw", "diffuse_work", "diffuse_spatial", "diffuse", "specular", "fresnel", "specular_raw", "fresnel_raw", "reflection_base", "reflection_fresnel", "reflection_geometry", "confidence", "display_diffuse", "display_specular", "nrd_diffuse_raw", "nrd_base", "nrd_fresnel", "nrd_motion" }) {
			String key(name);
			RD::DataFormat format = key == "confidence" ? RD::DATA_FORMAT_R16_SFLOAT : (key.begins_with("display_") ? RD::DATA_FORMAT_R32G32B32A32_SFLOAT : RD::DATA_FORMAT_R16G16B16A16_SFLOAT);
			int count = key.begins_with("display_") || key.begins_with("reflection_") || key == "confidence" ? 2 : 1;
			for (int i = 0; i < count; i++) {
				state->textures[key + itos(i)] = own(texture(state->size, format));
			}
		}
		state->textures["nrd_normal0"] = own(texture(state->size, RD::DATA_FORMAT_A2B10G10R10_UNORM_PACK32));
		state->textures["nrd_depth0"] = own(texture(state->size, RD::DATA_FORMAT_R32_SFLOAT));
		state->textures["nrd_specular_basis0"] = own(texture(state->size, RD::DATA_FORMAT_R16G16_SFLOAT));
		allocate("surfels", state->slots * 128, true);
		allocate("cell_heads", 262144 * 8);
		allocate("cell_links", state->slots * 27 * 4);
		allocate("grid_sums", 4096 * 4);
		allocate("free_slots", state->slots * 4);
		// Last word survives per-frame counter clearing: previous pool occupancy
		// lets GPU retirement reserve space for newly exposed geometry.
		allocate("counters", 20 + 262144 * 4);
		if (initialize_cache) {
			rd->buffer_clear(state->storage["counters"], 0, 20 + 262144 * 4);
		}
		allocate("ray_results", state->slots * 16);
		allocate("sample_history", state->slots * 16, true);
		allocate("ray_schedule", 16 + state->slots * 8);
		allocate("shared_samples", state->slots * 16);
		allocate("ray_guiding", state->slots * 16 * 8, true);
		Size2i ao_size = state->size;
		for (int i = 0; i < 5; i++) {
			state->textures["ao_depth" + itos(i) + "0"] = own(texture(ao_size, RD::DATA_FORMAT_R32_SFLOAT));
			ao_size = (ao_size + Size2i(1, 1)) / 2;
		}
		for (const char *name : { "ao", "ao_raw", "ao_work", "ao_spatial", "ao_history" }) {
			state->textures[String(name) + "0"] = own(texture(state->size, RD::DATA_FORMAT_R16_SFLOAT));
		}
		state->textures["ao_history1"] = own(texture(state->size, RD::DATA_FORMAT_R16_SFLOAT));
		state->textures["ao_edges0"] = own(texture(state->size, RD::DATA_FORMAT_R8G8B8A8_UNORM));
		state->ready = true;
	}
	if (state->history_version != world.history_version) {
		state->frames = 0;
		state->stationary_samples = 0;
		state->epoch++;
		state->history_version = world.history_version;
	}
	if (state->tracing != world.enabled) {
		state->frames = 0;
		state->tracing = world.enabled;
	}
	bool multibounce = ProjectSettings::get_singleton()->get_setting("rendering/kiln/surfel_multibounce", true);
	// kajiya restir-meets-surfel parity mode: reroutes the GI pass order and
	// shading to the ported reference pipeline (NRD and SurfelPlus stages idle).
	bool kajiya_mode = ProjectSettings::get_singleton()->get_setting("rendering/kiln/kajiya_mode", false);
	if (multibounce != state->multibounce) {
		state->frames = 0;
		state->multibounce = multibounce;
	}
	bool changed_world = state->geometry_version != world.geometry_version;
	bool changed_dynamic = state->dynamic_version != world.dynamic_version;
	bool changed_material = state->material_version != world.material_version;
	bool changed_light = changed_world || changed_dynamic || changed_material || state->light_version != world.light_version;
	if (changed_world) {
		upload("nodes", world.world.nodes);
		upload("triangles", world.world.triangles);
		state->frames = 0;
	}
	if (changed_dynamic) {
		upload("dynamic_nodes", world.dynamic.nodes);
		upload("dynamic_triangles", world.dynamic.triangles);
	}
	if (initialize_cache || state->texture_version != world.texture_version) {
		if (state->ray_albedo.is_valid()) {
			rd->free_rid(state->ray_albedo);
		}
		// Let the texture units perform sRGB decoding before bilinear filtering.
		// The source pages and UV contract are unchanged; this replaces four
		// scattered SSBO loads and twelve pow() evaluations at every ray hit.
		RD::TextureFormat format;
		format.texture_type = RD::TEXTURE_TYPE_2D_ARRAY;
		format.width = format.height = 512;
		format.array_layers = MAX(1, world.texture_pixels.size() / (512 * 512 * 4));
		format.format = RD::DATA_FORMAT_R8G8B8A8_SRGB;
		format.usage_bits = RD::TEXTURE_USAGE_SAMPLING_BIT;
		Vector<Vector<uint8_t>> pages;
		for (uint32_t page = 0; page < format.array_layers; page++) {
			Vector<uint8_t> pixels;
			pixels.resize(512 * 512 * 4);
			if (world.texture_pixels.is_empty()) {
				memset(pixels.ptrw(), 255, pixels.size());
			} else {
				memcpy(pixels.ptrw(), world.texture_pixels.ptr() + page * pixels.size(), pixels.size());
			}
			pages.push_back(pixels);
		}
		state->ray_albedo = rd->texture_create(format, RD::TextureView(), pages);
		state->texture_version = world.texture_version;
	}
	if (changed_world || state->static_material_version != world.static_material_version) {
		upload("texture_coordinates", world.world.texture_coordinates);
	}
	if (changed_dynamic || state->dynamic_material_version != world.dynamic_material_version) {
		upload("dynamic_texture_coordinates", world.dynamic.texture_coordinates);
	}
	// Rebuild bounds on the compute queue before any ray query consumes them.
	for (bool dynamic : { false, true }) {
		if (!(dynamic ? changed_dynamic : changed_world)) {
			continue;
		}
		const auto &geometry = dynamic ? world.dynamic : world.world;
		String prefix = dynamic ? "dynamic_" : "";
		RID order = upload(prefix + "refit", geometry.refit_order);
		for (Vector2i level : geometry.refit_levels) {
			dispatch(BVH_REFIT, Size2i(level.y, 1), { { 0, RD::UNIFORM_TYPE_STORAGE_BUFFER, state->storage[prefix + "nodes"] }, { 1, RD::UNIFORM_TYPE_STORAGE_BUFFER, state->storage[prefix + "triangles"] }, { 2, RD::UNIFORM_TYPE_STORAGE_BUFFER, order } }, level.x);
		}
	}
	bool want_hardware = hardware_available && world.query_backend != 1 && !state->hardware_failed;
	if (!want_hardware && state->hardware_active) {
		state->free_hardware();
	}
	if (want_hardware && (!state->hardware_active || changed_world || changed_dynamic)) {
		bool first_build = !state->hardware_active;
		if (state->hardware_tlas.is_valid()) {
			rd->free_rid(state->hardware_tlas);
			state->hardware_tlas = RID();
		}
		bool success = true;
		for (int i = 0; i < 2 && success; i++) {
			if (!first_build && !(i == 0 ? changed_world : changed_dynamic)) {
				continue;
			}
			if (state->hardware_blas[i].is_valid()) {
				rd->free_rid(state->hardware_blas[i]);
				state->hardware_blas[i] = RID();
			}
			if (state->hardware_vertices[i].is_valid()) {
				rd->free_rid(state->hardware_vertices[i]);
				state->hardware_vertices[i] = RID();
			}
			const KilnWorld::Geometry &source = i == 0 ? world.world : world.dynamic;
			if (source.triangle_count == 0) {
				continue;
			}
			PackedByteArray vertices;
			vertices.resize(source.triangle_count * 9 * sizeof(float));
			float *destination = reinterpret_cast<float *>(vertices.ptrw());
			const float *triangles = reinterpret_cast<const float *>(source.triangles.ptr());
			for (uint32_t triangle = 0; triangle < source.triangle_count; triangle++) {
				const float *t = triangles + triangle * 20;
				for (int vertex = 0; vertex < 3; vertex++) {
					for (int axis = 0; axis < 3; axis++) {
						*destination++ = t[axis] + (vertex == 0 ? 0.0f : t[vertex * 4 + axis]);
					}
				}
			}
			state->hardware_vertices[i] = rd->vertex_buffer_create(vertices.size(), vertices, RD::BUFFER_CREATION_DEVICE_ADDRESS_BIT | RD::BUFFER_CREATION_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT);
			RD::AccelerationStructureGeometry geometry;
			geometry.flags = RD::ACCELERATION_STRUCTURE_GEOMETRY_OPAQUE_BIT;
			geometry.vertex_buffer = state->hardware_vertices[i];
			geometry.vertex_stride = 3 * sizeof(float);
			geometry.vertex_count = source.triangle_count * 3;
			geometry.vertex_format = RD::DATA_FORMAT_R32G32B32_SFLOAT;
			Vector<RD::AccelerationStructureGeometry> geometries;
			geometries.push_back(geometry);
			state->hardware_blas[i] = rd->blas_create(geometries, i == 0 ? RD::ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT : RD::ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT);
			success = state->hardware_blas[i].is_valid() && rd->blas_build(state->hardware_blas[i]) == OK;
			if (success) {
				state->hardware_builds++;
			}
		}
		if (success) {
			Vector<RD::AccelerationStructureInstance> instances;
			for (int i = 0; i < 2; i++) {
				if (!state->hardware_blas[i].is_valid()) {
					continue;
				}
				RD::AccelerationStructureInstance instance;
				instance.blas = state->hardware_blas[i];
				instance.id = i;
				instance.flags = RD::ACCELERATION_STRUCTURE_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT;
				instances.push_back(instance);
			}
			state->hardware_tlas = rd->tlas_create(2, RD::ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT);
			success = state->hardware_tlas.is_valid() && rd->tlas_build(state->hardware_tlas, instances) == OK;
			if (success) {
				state->tlas_builds++;
			}
		}
		state->hardware_active = success;
		if (!success) {
			state->free_hardware();
			state->hardware_failed = true;
			WARN_PRINT("Kiln hardware acceleration structure creation failed; using compute software BVH.");
		}
	}
	RID query_tlas = state->hardware_active ? state->hardware_tlas : RID();
	int emitter_count = world.world.emitters.size() + world.dynamic.emitters.size();
	if (!changed_world && state->static_material_version != world.static_material_version) {
		upload("triangles", world.world.triangles);
	}
	if (!changed_dynamic && state->dynamic_material_version != world.dynamic_material_version) {
		upload("dynamic_triangles", world.dynamic.triangles);
	}
	if (changed_world || changed_dynamic || changed_material) {
		PackedByteArray packed;
		packed.resize(MAX(1, emitter_count) * 16);
		packed.fill(0);
		float *out = reinterpret_cast<float *>(packed.ptrw());
		int i = 0;
		for (Vector4 e : world.world.emitters) {
			for (int k = 0; k < 4; k++) {
				out[i * 4 + k] = e[k];
			}
			i++;
		}
		for (Vector4 e : world.dynamic.emitters) {
			e.x += world.world.triangle_count;
			e.y += world.world.power;
			for (int k = 0; k < 4; k++) {
				out[i * 4 + k] = e[k];
			}
			i++;
		}
		upload("emitters", packed);
	}
	if (state->light_version != world.light_version || initialize) {
		upload("local_lights", world.local_lights);
		upload("light_grid", world.light_grid);
	}
	Vector3 sun_radiance = world.sun_color * world.sun_energy;
	Vector3 sky_radiance = world.sky_horizon * world.sky_energy;
	Vector3 sky_zenith = world.sky_zenith * world.sky_energy;
	auto relative_change = [](Vector3 a, Vector3 b) {
		return (a - b).length() / MAX(MAX(a.length(), b.length()), 0.001f);
	};
	// A light revision is not a magnitude: a continuously advancing TOD must
	// not request the same estimator bandwidth as an instantaneous light switch.
	float lighting_delta = MAX(relative_change(sun_radiance, state->previous_sun_radiance), MAX(relative_change(sky_radiance, state->previous_sky_radiance), relative_change(sky_zenith, state->previous_sky_zenith)));
	if (MAX(sun_radiance.length_squared(), state->previous_sun_radiance.length_squared()) > 0.0001f) {
		lighting_delta = MAX(lighting_delta, (world.sun_direction - state->previous_sun_direction).length());
	}
	if (changed_dynamic || changed_material || world.local_lights != state->previous_local_lights) {
		lighting_delta = 1.0f;
	}
	// Light revisions may arrive every frame during a continuous TOD animation.
	// Convert the measured magnitude into a continuous bandwidth request instead
	// of entering a fixed-duration estimator mode. Abrupt steps still receive a
	// strong but smoothly decaying response; small TOD increments remain quiet.
	if (state->frames == 0 || changed_world) {
		state->lighting_response = 0.0f;
	} else {
		float requested_response = CLAMP(lighting_delta * 4.0f, 0.0f, 1.0f);
		state->lighting_response = MAX(requested_response, state->lighting_response * 0.94f);
		if (state->lighting_response < 0.001f) {
			state->lighting_response = 0.0f;
		}
	}
	if (changed_light) {
		state->epoch++;
	}
	state->previous_sun_direction = world.sun_direction;
	state->previous_sun_radiance = sun_radiance;
	state->previous_sky_radiance = sky_radiance;
	state->previous_sky_zenith = sky_zenith;
	state->previous_local_lights = world.local_lights;
	bool camera_projection_changed = state->frames > 0 && scene->cam_projection != state->previous_projection;
	bool camera_moved = state->frames > 0 && (scene->cam_transform != state->previous_camera || camera_projection_changed);
	float camera_translation = state->frames > 0 ? scene->cam_transform.origin.distance_to(state->previous_camera.origin) : 0.0f;
	float camera_rotation = state->frames > 0 ? scene->cam_transform.basis.get_rotation_quaternion().angle_to(state->previous_camera.basis.get_rotation_quaternion()) : 0.0f;
	// A routine walk exposes only a narrow strip and the normal generation pass
	// keeps up. Reserve the corrective pass for actual disocclusion jumps so the
	// common moving-camera path does not pay for another grid rebuild every frame.
	bool camera_disocclusion = initialize || state->frames < 8 || camera_projection_changed || camera_translation > 0.20f || camera_rotation > Math::deg_to_rad(4.0f);
	if (camera_moved) {
		state->motion_remaining = 8;
	} else if (state->motion_remaining) {
		state->motion_remaining--;
	}
	bool moving = camera_moved || state->motion_remaining || state->lighting_response > 0.01f;
	if (moving) {
		state->stationary_samples = 0;
	}
	Projection projection = scene->get_cam_projection();
	Projection vp = projection * Projection(scene->cam_transform.affine_inverse());
	float params[184] = {};
	int at = 0;
	for (const Projection &m : { projection, projection.inverse(), Projection(scene->cam_transform), Projection(scene->cam_transform.affine_inverse()), state->previous_vp }) {
		MaterialStorage::store_camera(m, params + at);
		at += 16;
	}
	auto v = [&](float x, float y, float z, float w) { params[at++] = x; params[at++] = y; params[at++] = z; params[at++] = w; };
	auto vec = [&](Vector3 a, float w) { v(a.x, a.y, a.z, w); };
	v(state->size.x, state->size.y, state->frames, state->frames > 0 && !initialize);
	v(1.2, state->lighting_response, moving ? -1 : state->stationary_samples, world.samples);
	// A stationary camera still samples different receivers under TAA jitter.
	// Reproject those samples without classifying the camera as moving, so
	// stationary confidence can accumulate to the full quality budget.
	bool reproject = camera_moved || scene->taa_jitter != Vector2() || scene->prev_taa_jitter != Vector2();
	bool reconstruct_diffuse = ProjectSettings::get_singleton()->get_setting("rendering/kiln/surfel_reconstruction", true);
	v(world.rays, reproject, world.ao_quality, reconstruct_diffuse);
	// Preserve the correctness-first budget as a ceiling while avoiding its full
	// cost after the cache has settled. The measured tiers retain full bootstrap
	// and abrupt-relighting bandwidth, then reduce steady and camera-motion work.
	// A configured budget of zero keeps its historical meaning: uncapped rays.
	int configured_surfel_ray_budget = CLAMP(int(ProjectSettings::get_singleton()->get_setting("rendering/kiln/surfel_ray_budget", 2097152)), 0, 8388608);
	bool adaptive_surfel_budget = ProjectSettings::get_singleton()->get_setting("rendering/kiln/surfel_adaptive_budget", true);
	int surfel_ray_budget = configured_surfel_ray_budget;
	String surfel_ray_budget_mode = configured_surfel_ray_budget == 0 ? "uncapped" : "fixed";
	if (adaptive_surfel_budget && configured_surfel_ray_budget > 0) {
		int stationary_budget = CLAMP(int(ProjectSettings::get_singleton()->get_setting("rendering/kiln/surfel_stationary_ray_budget", 393216)), 65536, 8388608);
		int motion_budget = MAX(stationary_budget, CLAMP(int(ProjectSettings::get_singleton()->get_setting("rendering/kiln/surfel_motion_ray_budget", 524288)), 65536, 8388608));
		int bootstrap_budget = MAX(motion_budget, CLAMP(int(ProjectSettings::get_singleton()->get_setting("rendering/kiln/surfel_bootstrap_ray_budget", 1048576)), 65536, 8388608));
		int requested_budget = stationary_budget;
		surfel_ray_budget_mode = "stationary";
		if (camera_moved || state->motion_remaining > 0) {
			requested_budget = MAX(requested_budget, motion_budget);
			surfel_ray_budget_mode = "motion";
		}
		// Continuous TOD changes need the motion tier, while abrupt changes ramp
		// toward the configured correctness ceiling and decay with the response.
		if (state->lighting_response > 0.001f) {
			requested_budget = MAX(requested_budget, motion_budget);
			surfel_ray_budget_mode = "lighting";
		}
		if (state->lighting_response > 0.02f) {
			float relight = Math::smoothstep(0.02f, 0.5f, state->lighting_response);
			requested_budget = MAX(requested_budget, int(Math::lerp(float(motion_budget), float(configured_surfel_ray_budget), relight)));
			surfel_ray_budget_mode = "relighting";
		}
		// New surfels receive their 32-ray admission batch without making the
		// high bootstrap tier a permanent steady-state cost.
		if (state->frames < 16) {
			requested_budget = MAX(requested_budget, bootstrap_budget);
			surfel_ray_budget_mode = "bootstrap";
		}
		surfel_ray_budget = MIN(configured_surfel_ray_budget, requested_budget);
		if (configured_surfel_ray_budget < requested_budget) {
			surfel_ray_budget_mode = "configured_cap";
		}
	}
	vec(world.world.bounds.position, surfel_ray_budget);
	vec(world.world.bounds.size, state->slots);
	int specular_rays = ProjectSettings::get_singleton()->get_setting("rendering/kiln/surfel_specular", true) ? CLAMP(int(ProjectSettings::get_singleton()->get_setting("rendering/kiln/specular_rays", 2)), 1, 8) : 0;
	int nrd_diffuse_iterations = CLAMP(int(ProjectSettings::get_singleton()->get_setting("rendering/kiln/nrd_diffuse_iterations", 4)), 2, 5);
	bool use_nrd = world.enabled && reconstruct_diffuse && KilnNRD::available() && bool(ProjectSettings::get_singleton()->get_setting("rendering/kiln/nrd", false));
	bool combined_specular = buffers->has_texture(SNAME("kiln_deferred"), SNAME("surface")) && specular_rays > 0 && bool(ProjectSettings::get_singleton()->get_setting("rendering/kiln/nrd_combined_specular", true));
	if (use_nrd && state->nrd && (state->nrd_specular != (specular_rays > 0) || state->nrd_combined_specular != combined_specular)) {
		memdelete(state->nrd);
		state->nrd = nullptr;
		state->nrd_active = false;
	}
	if (use_nrd && !state->nrd) {
		state->nrd = memnew(KilnNRD);
		state->nrd_specular = specular_rays > 0;
		state->nrd_combined_specular = combined_specular;
		if (!state->nrd->initialize(state->size, state->nrd_specular, combined_specular)) {
			memdelete(state->nrd);
			state->nrd = nullptr;
		}
	}
	use_nrd = use_nrd && state->nrd;
	bool nrd_validation = use_nrd && bool(ProjectSettings::get_singleton()->get_setting("rendering/kiln/nrd_diffuse_validation", false));
	// NRD owns its own reset; switching a post-filter must not destroy diffuse
	// transport, coverage, sampling sequences or world-space convergence.
	// Keep the shader sampling parity and NRD's frame index identical even
	// on the frame that switches NRD on/off.
	params[82] = state->frames;
	params[83] = state->frames > 0 && !initialize;
	bool nrd_checkerboard = use_nrd && specular_rays > 0 && bool(ProjectSettings::get_singleton()->get_setting("rendering/kiln/nrd_specular_checkerboard", false)) && !bool(ProjectSettings::get_singleton()->get_setting("rendering/kiln/nrd_reference", false));
	bool specular_checkerboard = ProjectSettings::get_singleton()->get_setting("rendering/kiln/specular_checkerboard", true);
	float surfel_target_diameter = CLAMP(float(ProjectSettings::get_singleton()->get_setting("rendering/kiln/surfel_target_diameter_pixels", 20.0)), 12.0f, 40.0f);
	v(surfel_target_diameter, specular_checkerboard && !use_nrd, use_nrd ? (nrd_checkerboard ? 2 : 1) : 0, state->epoch);
	vec(world.sun_direction, world.sun_energy * Math::PI);
	vec(world.sun_color, world.sky_energy);
	bool irradiance_sharing = ProjectSettings::get_singleton()->get_setting("rendering/kiln/surfel_irradiance_sharing", true);
	vec(world.sky_horizon, irradiance_sharing);
	int debug_mode = ProjectSettings::get_singleton()->get_setting("rendering/kiln/debug_view", 0);
	float debug_radius = CLAMP(float(ProjectSettings::get_singleton()->get_setting("rendering/kiln/surfel_debug_radius", 0.45)), 0.15f, 1.0f);
	float debug_gain = CLAMP(float(ProjectSettings::get_singleton()->get_setting("rendering/kiln/surfel_debug_gain", 4.0)), 0.1f, 32.0f);
	v(debug_mode, debug_radius, debug_gain, specular_rays);
	v(0, 0, 0, 1);
	vec(world.sky_zenith, world.procedural_sky ? 2 : 1);
	vec(world.procedural_sky ? Vector3() : Vector3(0.554, 0.464, 0.304), world.procedural_sky ? world.sky_halo * world.sun_energy : 0);
	vec(world.sun_direction, world.time_of_day);
	vec(world.procedural_sky ? world.cloud_color : Vector3(), world.procedural_sky ? world.cloud_coverage : 0);
	v(world.sky_saturation, 1, 0, 0);
	v(0, 0, 0, 1);
	// w is a continuous lighting-response request, not a binary relighting mode.
	v(world.world.node_count, world.world.triangle_count, world.enabled, state->lighting_response);
	v(world.dynamic.node_count, world.dynamic.triangle_count, emitter_count, world.world.power + world.dynamic.power);
	MaterialStorage::store_camera(state->previous_vp.inverse(), params + at);
	at += 16;
	bool has_surface = buffers->has_texture(SNAME("kiln_deferred"), SNAME("surface"));
	RID surface_input = has_surface ? buffers->get_texture(SNAME("kiln_deferred"), SNAME("surface")) : empty_surface;
	v(signed_normal, has_surface, multibounce, changed_dynamic);
	// kajiya parity port: previous eye position (the reference clipmap anchors the
	// grid on the previous frame's eye) and the reference sun size / mode flags.
	vec(state->previous_camera.origin, 1.0f);
	const float kajiya_sun_angular_radius_cos = Math::cos(Math::deg_to_rad(0.53f) * 0.5f);
	v(kajiya_sun_angular_radius_cos, 0.0f, kajiya_mode ? 1.0f : 0.0f, 0);
	ERR_FAIL_COND_V(at != 184, false);
	rd->buffer_update(state->parameters, 0, sizeof(params), params);
	auto U = [&]() { return Binding{ 0, RD::UNIFORM_TYPE_UNIFORM_BUFFER, state->parameters }; };
	auto S = [&](int binding, RID rid, bool linear = false) { return Binding{ binding, RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, rid, linear }; };
	auto B = [&](int binding, String name) { return Binding{ binding, RD::UNIFORM_TYPE_STORAGE_BUFFER, state->storage[name] }; };
	auto I = [&](int binding, RID rid) { return Binding{ binding, RD::UNIFORM_TYPE_IMAGE, rid }; };
	if (initialize) {
		dispatch(SEQUENCE, Size2i(128, 128), { U(), I(1, sequence), I(2, hilbert) });
	}
	int current = state->index, previous = 1 - current;
	auto T = [&](String name, int i = 0) { return state->t(name, i); };
	RID depth = buffers->get_depth_texture(), normal = full_normal;
	if (state->frames == 0 || changed_world) {
		rd->buffer_clear(state->storage["surfels"], 0, state->slots * 128);
	}
	auto trace_specular = [&](RID base, RID fresnel, bool force_translated) {
		dispatch(SURFEL_SPECULAR, state->size,
				{ U(), S(1, depth), S(2, normal), S(31, surface_input), B(4, "nodes"), B(5, "triangles"), B(16, "dynamic_nodes"), B(17, "dynamic_triangles"), B(18, "emitters"), B(25, "local_lights"), B(26, "light_grid"), S(32, state->ray_albedo, true), B(33, "texture_coordinates"), B(34, "dynamic_texture_coordinates"),
						B(20, "surfels"), B(21, "cell_heads"), B(22, "cell_links"), B(23, "free_slots"), B(24, "counters"), B(35, "grid_sums"), I(6, base), I(7, fresnel) },
				specular_rays, 1, query_tlas, force_translated);
	};
	if (kajiya_mode) {
		// kajiya restir-meets-surfel parity pipeline (port in progress). The
		// clipmap surfel pool, binning, tracing and a screen resolve replace the
		// SurfelPlus chain; NRD and XeGTAO stay idle.
		const uint32_t kmax = 262144u;
		allocate("kajiya_meta", 8 * 4);
		allocate("kajiya_pool", kmax * 4, true);
		allocate("kajiya_cell_offset", (kmax + 1) * 4);
		allocate("kajiya_index", kmax * 24 * 4);
		allocate("kajiya_spatial", kmax * 16);
		allocate("kajiya_irradiance", kmax * 16);
		allocate("kajiya_aux", kmax * 32);
		allocate("kajiya_life", kmax * 4);
		allocate("kajiya_proposal", kmax * 16);
		allocate("kajiya_scan_segments", 1024 * 4);
		allocate("kajiya_args", 12 * 4);
		if (initialize) {
			rd->buffer_clear(state->storage["kajiya_meta"], 0, 8 * 4);
			rd->buffer_clear(state->storage["kajiya_life"], 0, kmax * 4);
			dispatch(KAJIYA_SURFEL_CLEAR_POOL, Size2i(kmax / 64, 1),
					{ U(), B(41, "kajiya_pool"), B(40, "kajiya_meta"), B(42, "kajiya_cell_offset"), B(43, "kajiya_index"), B(44, "kajiya_spatial"), B(45, "kajiya_irradiance"), B(46, "kajiya_aux"), B(47, "kajiya_life"), B(48, "kajiya_proposal") });
		}
		if (!state->textures.has("kajiya_sky")) {
			RD::TextureFormat tf;
			tf.format = RD::DATA_FORMAT_R16G16B16A16_SFLOAT;
			tf.texture_type = RD::TEXTURE_TYPE_2D_ARRAY;
			tf.width = 32;
			tf.height = 32;
			tf.array_layers = 6;
			tf.mipmaps = 1;
			tf.usage_bits = RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_STORAGE_BIT | RD::TEXTURE_USAGE_CAN_COPY_TO_BIT;
			state->textures["kajiya_sky"] = own(rd->texture_create(tf, RD::TextureView()));
			RD::TextureFormat ta;
			ta.format = RD::DATA_FORMAT_R32G32_UINT;
			ta.width = (state->size.x + 7) / 8;
			ta.height = (state->size.y + 7) / 8;
			ta.usage_bits = RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_STORAGE_BIT | RD::TEXTURE_USAGE_COLOR_ATTACHMENT_BIT;
			state->textures["kajiya_tile_alloc"] = own(rd->texture_create(ta, RD::TextureView()));
			RD::TextureFormat ti;
			ti.format = RD::DATA_FORMAT_R32G32B32A32_SFLOAT;
			ti.width = (state->size.x + 7) / 8;
			ti.height = (state->size.y + 7) / 8;
			ti.usage_bits = RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_STORAGE_BIT | RD::TEXTURE_USAGE_COLOR_ATTACHMENT_BIT;
			state->textures["kajiya_tile_irradiance"] = own(rd->texture_create(ti, RD::TextureView()));
		}
		RID ksky = state->textures["kajiya_sky"];
		RID kalloc = state->textures["kajiya_tile_alloc"];
		RID ktile_irr = state->textures["kajiya_tile_irradiance"];
		RID kalbedo = has_surface ? buffers->get_texture(SNAME("kiln_deferred"), SNAME("albedo_metallic")) : empty_surface;
		// Every kajiya surfel stage shares the binding contract of the include; bind
		// the full set plus the stage-specific resources on top.
		auto kbase = [&]() {
			return std::vector<Binding>{
				U(),
				B(40, "kajiya_meta"), B(41, "kajiya_pool"), B(42, "kajiya_cell_offset"), B(43, "kajiya_index"),
				B(44, "kajiya_spatial"), B(45, "kajiya_irradiance"), B(46, "kajiya_aux"), B(47, "kajiya_life"), B(48, "kajiya_proposal")
			};
		};
		auto kappend = [&](std::vector<Binding> &list, std::initializer_list<Binding> extra) {
			list.insert(list.end(), extra.begin(), extra.end());
		};
		{
			std::vector<Binding> b = kbase();
			kappend(b, { I(1, ksky) });
			dispatch(KAJIYA_SKY, Size2i(4, 4), b, 0, 6);
		}
		{
			std::vector<Binding> b = kbase();
			kappend(b, { S(10, depth), S(11, surface_input), I(12, kalloc), I(13, ktile_irr) });
			dispatch(KAJIYA_SURFEL_FIND_MISSING, Size2i((state->size.x + 7) / 8, (state->size.y + 7) / 8), b);
		}
		{
			std::vector<Binding> b = kbase();
			kappend(b, { B(10, "kajiya_args") });
			dispatch(KAJIYA_SURFEL_ARGS, Size2i(1, 1), b);
		}
		dispatch(KAJIYA_SURFEL_AGE, Size2i(kmax / 64, 1), kbase());
		{
			std::vector<Binding> b = kbase();
			kappend(b, { S(10, depth), S(11, surface_input), I(12, kalloc), I(13, ktile_irr) });
			dispatch(KAJIYA_SURFEL_ALLOCATE, Size2i((state->size.x + 7) / 8, (state->size.y + 7) / 8), b);
		}
		dispatch(KAJIYA_SURFEL_CLEAR_CELLS, Size2i(65536, 1), kbase());
		dispatch(KAJIYA_SURFEL_COUNT_CELLS, Size2i(kmax / 64, 1), kbase());
		{
			std::vector<Binding> b = kbase();
			kappend(b, { B(10, "kajiya_cell_offset"), B(11, "kajiya_scan_segments") });
			dispatch(KAJIYA_SURFEL_SCAN, Size2i(1024, 1), b);
			dispatch(KAJIYA_SURFEL_SCAN_SEGMENTS, Size2i(1, 1), b);
			dispatch(KAJIYA_SURFEL_SCAN_MERGE, Size2i(1024, 1), b);
		}
		dispatch(KAJIYA_SURFEL_SLOT_CELLS, Size2i(kmax / 64, 1), kbase());
		{
			std::vector<Binding> b = kbase();
			kappend(b, { B(4, "nodes"), B(5, "triangles"), B(16, "dynamic_nodes"), B(17, "dynamic_triangles"), B(18, "emitters"), B(25, "local_lights"), B(26, "light_grid"), S(32, state->ray_albedo, true), B(33, "texture_coordinates"), B(34, "dynamic_texture_coordinates"), S(19, ksky), S(20, dfg) });
			dispatch(KAJIYA_SURFEL_TRACE, Size2i(kmax / 64, 1), b, 0, 1, query_tlas);
		}
		{
			std::vector<Binding> b = kbase();
			kappend(b, { S(10, depth), S(11, surface_input), S(12, kalbedo), I(13, T("diffuse")) });
			dispatch(KAJIYA_SURFEL_RESOLVE, Size2i((state->size.x + 7) / 8, (state->size.y + 7) / 8), b);
		}
	} else if (world.enabled) {
		const Size2i surfel_dispatch(256, state->slots / 256);
		auto surfel_pass = [&](Stage stage, bool geometry, bool screen, RID tlas = RID()) {
			Size2i work = screen ? state->size : surfel_dispatch;
			if (stage == SURFEL_GRID_PREFIX) {
				work = Size2i(256, 1024);
			}
			if (stage == SURFEL_GRID_PREFIX_SUMS) {
				work = Size2i(8, 8);
			}
			// Fixed descriptors are shared by the stages; ShaderRD retains the
			// declared binding contract even for helpers eliminated by glslang.
			if (geometry) {
				dispatch(stage, work,
						{ U(), S(1, depth), S(2, normal), S(31, surface_input), B(4, "nodes"), B(5, "triangles"), B(16, "dynamic_nodes"), B(17, "dynamic_triangles"), B(18, "emitters"), B(25, "local_lights"), B(26, "light_grid"), S(32, state->ray_albedo, true), B(33, "texture_coordinates"), B(34, "dynamic_texture_coordinates"),
								B(20, "surfels"), B(21, "cell_heads"), B(22, "cell_links"), B(23, "free_slots"), B(24, "counters"), B(28, "ray_results"), B(35, "grid_sums"), B(37, "ray_guiding"), B(38, "sample_history"), B(39, "ray_schedule") },
						0, 1, tlas);
			} else {
				dispatch(stage, work,
						{ U(), S(1, depth), S(2, normal), S(31, surface_input), I(6, T("raw")), I(8, T("confidence", current)),
								B(20, "surfels"), B(21, "cell_heads"), B(22, "cell_links"), B(23, "free_slots"), B(24, "counters"), B(28, "ray_results"), B(35, "grid_sums"), B(36, "shared_samples"), B(38, "sample_history"), B(39, "ray_schedule") });
			}
		};
		auto build_grid = [&]() {
			rd->buffer_clear(state->storage["cell_heads"], 0, 262144 * 8);
			surfel_pass(SURFEL_GRID, false, false);
			surfel_pass(SURFEL_GRID_PREFIX, false, false);
			surfel_pass(SURFEL_GRID_PREFIX_SUMS, false, false);
			surfel_pass(SURFEL_GRID_SCATTER, false, false);
		};
		bool generation_needs_current_grid = initialize || state->frames == 0 || changed_world || changed_dynamic;
		if (generation_needs_current_grid) {
			// There is no safe previous grid on initialization or after geometry
			// changes. Build the updated live cache before looking for holes.
			rd->buffer_clear(state->storage["counters"], 0, 16 + 262144 * 4);
			surfel_pass(SURFEL_UPDATE, true, false);
			build_grid();
			surfel_pass(SURFEL_GENERATE, true, true, query_tlas);
			build_grid();
		} else {
			// Match Webgiya's frame order: find holes against the complete previous
			// grid, allocate from the unused tail of its free-slot list, then update
			// and rebuild once so newborns are traced and resolved this frame. The
			// prior spawn_count skips slots allocated from that list last frame.
			rd->buffer_clear(state->storage["counters"], 16, 262144 * 4);
			surfel_pass(SURFEL_GENERATE, true, true, query_tlas);
			rd->buffer_clear(state->storage["counters"], 0, 16 + 262144 * 4);
			surfel_pass(SURFEL_UPDATE, true, false);
			build_grid();
		}
		// A single 8x8 election can cover only one disconnected receiver and its
		// hash claim can collide with another proposal. On a large camera
		// disocclusion, make one corrective pass against the rebuilt grid.
		// Clearing only the transient claims lets genuine remaining holes allocate;
		// spawn/free/alive counters and the persistent occupancy stay intact.
		if (camera_disocclusion) {
			rd->buffer_clear(state->storage["counters"], 16, 262144 * 4);
			surfel_pass(SURFEL_GENERATE, true, true, query_tlas);
			build_grid();
		}
		rd->buffer_clear(state->storage["ray_schedule"], 0, 16);
		surfel_pass(SURFEL_SCHEDULE, false, false);
		surfel_pass(SURFEL_TRACE, true, false, query_tlas);
		surfel_pass(SURFEL_SPATIAL, false, false);
		surfel_pass(SURFEL_INTEGRATE, false, false);
		// Enabling a denoiser must not replace the GI algorithm or launch another
		// diffuse path tracer. Both modes resolve the same world-space cache.
		surfel_pass(SURFEL_EVALUATE, false, true);
		if (specular_rays > 0) trace_specular(T("specular_raw"), T("fresnel_raw"), false);
	} else {
		rd->texture_clear(T("raw"), Color(0, 0, 0, 0), 0, 1, 0, 1);
	}
	if (kajiya_mode) {
		// The kajiya resolve wrote diffuse; keep AO neutral and the specular
		// targets empty while the specular chain is not ported.
		rd->texture_clear(T("ao"), Color(1, 1, 1, 1), 0, 1, 0, 1);
		rd->texture_clear(T("specular"), Color(), 0, 1, 0, 1);
		rd->texture_clear(T("fresnel"), Color(), 0, 1, 0, 1);
	} else {
	if (state->ao_quality != world.ao_quality || state->frames == 0 || initialize) {
		state->ao_frames = 0;
	}
	state->ao_quality = world.ao_quality;
	if (world.ao_quality == 0) {
		rd->texture_clear(T("ao"), Color(1, 1, 1, 1), 0, 1, 0, 1);
	} else {
		Size2i ao_size = state->size;
		for (int mip = 0; mip < 5; mip++) {
			dispatch(AO_DEPTH, ao_size, { U(), S(1, mip == 0 ? depth : T("ao_depth" + itos(mip - 1))), I(2, T("ao_depth" + itos(mip))) }, mip);
			ao_size = (ao_size + Size2i(1, 1)) / 2;
		}
		dispatch(AO_MAIN, state->size, { U(), S(1, T("ao_depth0")), S(2, T("ao_depth1")), S(3, T("ao_depth2")), S(4, T("ao_depth3")), S(5, T("ao_depth4")), S(6, full_normal), I(7, T("ao_raw")), I(8, T("ao_edges")), S(9, hilbert) });
		dispatch(AO_DENOISE, state->size, { S(1, T("ao_raw")), S(2, T("ao_edges")), I(3, T("ao_work")) }, 0);
		dispatch(AO_DENOISE, state->size, { S(1, T("ao_work")), S(2, T("ao_edges")), I(3, T("ao_spatial")) }, 1);
		dispatch(AO_TEMPORAL, state->size, { U(), S(1, T("ao_spatial")), S(2, depth), S(3, full_normal), S(4, T("ao_history", previous)), S(5, T("display_diffuse", previous)), S(6, T("display_specular", previous)), I(7, T("ao_history", current)), I(8, T("ao")) }, state->ao_frames);
		state->ao_frames = MIN(state->ao_frames + 1, 32);
	}
	auto publish_diffuse = [&](bool temporal) {
		RID diffuse_input = T("raw");
		if (world.enabled && temporal) {
			// World-space sharing removes disk variance before projection. Two small
			// reconstruction passes replace the four wide full-screen blur passes.
			for (int pass = 0; pass < 2; pass++) {
				RID output = T(pass % 2 == 0 ? "diffuse_work" : "diffuse_spatial");
				dispatch(SURFEL_DIFFUSE_FILTER, state->size, { U(), S(1, depth), S(2, normal), S(31, surface_input), S(3, diffuse_input), I(6, output) }, 1 << pass);
				diffuse_input = output;
			}
		}
		dispatch(SURFEL_PUBLISH, state->size, { U(), S(1, depth), S(2, normal), S(31, surface_input), S(3, diffuse_input), I(6, T("diffuse")), I(9, T("display_diffuse", current)), I(10, T("display_specular", current)), S(11, T("display_diffuse", previous)), S(12, T("display_specular", previous)) }, temporal ? 1 : 0);
	};
	publish_diffuse(reconstruct_diffuse && (!use_nrd || nrd_validation));
	if (nrd_validation) {
		if (!state->textures.has("diffuse_reference0")) state->textures["diffuse_reference0"] = own(texture(state->size, RD::DATA_FORMAT_R16G16B16A16_SFLOAT));
		rd->texture_copy(T("diffuse"), T("diffuse_reference"), Vector3(), Vector3(), Vector3(state->size.x, state->size.y, 1), 0, 0, 0, 0);
	}

	if (use_nrd) {
		RID motion = has_surface && buffers->has_velocity_buffer(false) ? buffers->get_velocity_buffer(false) : normal;
		RID albedo = has_surface ? buffers->get_texture(SNAME("kiln_deferred"), SNAME("albedo_metallic")) : normal;
		RID material = has_surface ? buffers->get_texture(SNAME("kiln_deferred"), SNAME("material")) : normal;
		dispatch(NRD_PREPARE, state->size, { U(), S(1, depth), S(2, normal), S(3, motion), S(4, T("specular_raw")), S(5, T("fresnel_raw")), I(6, T("nrd_normal")), I(7, T("nrd_depth")), I(8, T("nrd_motion")), I(9, T("nrd_base")), I(10, T("nrd_fresnel")), S(11, T("raw")), I(12, T("nrd_diffuse_raw")), S(13, albedo), S(14, material), S(15, dfg, true), I(16, T("nrd_specular_basis")) }, combined_specular ? 1 : 0, 1, RID(), false, (scene->taa_jitter - scene->prev_taa_jitter) * 0.5);
		RENDER_TIMESTAMP("Kiln / NRD RELAX");
		bool reset = !state->nrd_active || state->frames == 0 || changed_world || changed_material || state->nrd_checkerboard != nrd_checkerboard;
		if (!has_surface && changed_dynamic) {
			reset = true;
		}
		use_nrd = state->nrd->denoise(scene, state->previous_projection, state->previous_camera, state->frames, reset, state->lighting_response > 0.25f, nrd_checkerboard, nrd_diffuse_iterations,
				T("nrd_motion"), T("nrd_normal"), T("nrd_depth"), T("nrd_diffuse_raw"), T("nrd_base"), T("nrd_fresnel"), T("diffuse"), T("specular"), T("fresnel"));
		if (!use_nrd) publish_diffuse(reconstruct_diffuse);
		RENDER_TIMESTAMP("Kiln / between passes");
	}
	if (use_nrd) {
		dispatch(NRD_RESOLVE, state->size, { U(), S(1, depth), I(6, T("diffuse")), I(7, T("specular")), I(8, T("fresnel")), S(9, T("nrd_specular_basis")) }, combined_specular ? 1 : 0);
	}
	if (!use_nrd && world.enabled && specular_rays > 0) {
		dispatch(SURFEL_SPECULAR_FILTER, state->size,
				{ U(), S(1, depth), S(2, normal), S(3, T("specular_raw")), S(4, T("fresnel_raw")), S(5, T("reflection_base", previous)), S(6, T("reflection_fresnel", previous)), S(7, T("reflection_geometry", previous)), S(8, T("display_diffuse", previous)),
						I(9, T("reflection_base", current)), I(10, T("reflection_fresnel", current)), I(11, T("reflection_geometry", current)), I(12, T("specular")), I(13, T("fresnel")) });
	}

	state->nrd_active = use_nrd;
	state->nrd_checkerboard = nrd_checkerboard;
	if (specular_rays == 0 || !world.enabled) {
		for (const char *name : { "specular", "fresnel", "specular_raw", "fresnel_raw" }) {
			rd->texture_clear(T(name), Color(), 0, 1, 0, 1);
		}
	}
	} // end !kajiya_mode legacy AO / publish / NRD / specular filtering

	if (!world.enabled) {
		rd->texture_clear(T("diffuse"), Color(0, 0, 0, 0), 0, 1, 0, 1);
		rd->texture_clear(T("specular"), Color(0, 0, 0, 0), 0, 1, 0, 1);
	}
	bool surfel_debug = debug_mode >= 15 && debug_mode <= 27 && has_surface;
	if (surfel_debug) {
		if (!state->textures.has("debug0")) {
			state->textures["debug0"] = own(texture(state->size, RD::DATA_FORMAT_R16G16B16A16_SFLOAT));
			state->textures["debug_metrics0"] = own(texture(state->size, RD::DATA_FORMAT_R32G32B32A32_SFLOAT));
		}
		dispatch(SURFEL_DEBUG, state->size,
				{ U(), S(1, depth), S(2, normal), S(3, T("raw")), S(31, surface_input),
						B(20, "surfels"), B(21, "cell_heads"), B(22, "cell_links"), B(23, "free_slots"), B(24, "counters"), B(28, "ray_results"), B(35, "grid_sums"),
						I(29, T("debug")), I(30, T("debug_metrics")) });
	}
	// Explicit diagnostics only: this synchronous readback is never used for timing.
	if (world.capture_request != state->capture_request && !world.capture_directory.is_empty()) {
		state->capture_request = world.capture_request;
		if (DirAccess::make_dir_recursive_absolute(world.capture_directory) == OK) {
			Dictionary metadata;
			metadata["nrd_active"] = use_nrd;
			metadata["gi_algorithm"] = "Surfel GI (SurfelPlus adaptation)";
			metadata["surfel_multibounce"] = multibounce;
			metadata["surfel_reconstruction"] = reconstruct_diffuse;
			metadata["surfel_target_diameter_pixels"] = surfel_target_diameter;
			metadata["surfel_ray_budget"] = surfel_ray_budget;
			metadata["surfel_ray_budget_configured"] = configured_surfel_ray_budget;
			metadata["surfel_ray_budget_adaptive"] = adaptive_surfel_budget;
			metadata["surfel_ray_budget_mode"] = surfel_ray_budget_mode;
			metadata["irradiance_sharing"] = irradiance_sharing;
			metadata["surfel_specular"] = specular_rays > 0;
			metadata["specular_rays"] = specular_rays;
			metadata["specular_implementation"] = state->hardware_active && native_specular_version.is_valid() ? "handwritten_msl" : "translated_glsl";
			if (world.enabled && query_tlas.is_valid() && native_specular_version.is_valid() && bool(ProjectSettings::get_singleton()->get_setting("rendering/kiln/metal_specular_validation", false))) {
				// Replay ONLY this read-only pass against the exact same frame's
				// inputs. Diagnostics allocate/read back these textures on demand;
				// normal rendering and timing never execute the reference dispatch.
				for (const char *name : { "specular_reference", "fresnel_reference" }) {
					if (!state->textures.has(String(name) + "0")) {
						state->textures[String(name) + "0"] = own(texture(state->size, RD::DATA_FORMAT_R16G16B16A16_SFLOAT));
					}
				}
				trace_specular(T("specular_reference"), T("fresnel_reference"), true);
				for (const char *name : { "specular_reference", "fresnel_reference" }) {
					Ref<FileAccess> dump = FileAccess::open(world.capture_directory.path_join(String(name) + ".bin"), FileAccess::WRITE);
					if (dump.is_valid()) {
						dump->store_buffer(rd->texture_get_data(T(name), 0));
					}
				}
				metadata["specular_reference"] = "same_frame_translated_glsl";
			}
			metadata["specular_checkerboard"] = specular_checkerboard && !use_nrd;
			metadata["nrd_version"] = use_nrd ? "4.17.3" : "disabled";
			metadata["nrd_checkerboard"] = nrd_checkerboard;
			metadata["nrd_diffuse_rays"] = 0;
			metadata["nrd_diffuse_iterations"] = nrd_diffuse_iterations;
			metadata["nrd_combined_specular"] = use_nrd && combined_specular;
			metadata["nrd_same_input_reference"] = nrd_validation;
			metadata["nrd_signals"] = use_nrd ? (specular_rays > 0 ? (combined_specular ? "cache_diffuse_and_demodulated_specular" : "cache_diffuse_and_specular_schlick_integrals") : "cache_diffuse") : "disabled";
			metadata["texture_pages"] = world.texture_pixels.size() / (512 * 512 * 4);
			metadata["texture_version"] = world.texture_version;
			metadata["renderer_path"] = has_surface ? "G-buffer deferred" : "Forward+";
			metadata["surfel_capacity"] = state->slots;
			metadata["surfel_debug_mode"] = surfel_debug ? debug_mode : 0;
			metadata["surfel_debug_radius_scale"] = debug_radius;
			metadata["surfel_debug_gain"] = debug_gain;
			if (surfel_debug) {
				for (const char *name : { "debug", "debug_metrics", "raw" }) {
					Ref<FileAccess> dump = FileAccess::open(world.capture_directory.path_join(String(name) + ".bin"), FileAccess::WRITE);
					if (dump.is_valid()) {
						dump->store_buffer(rd->texture_get_data(T(name), 0));
					}
				}
			}
			PackedByteArray counters = rd->buffer_get_data(state->storage["counters"]);
			const uint32_t *counts = reinterpret_cast<const uint32_t *>(counters.ptr());
			metadata["surfel_alive"] = counts[2] + MIN(counts[0], counts[1]);
			metadata["surfel_spawned"] = MIN(counts[0], counts[1]);
			metadata["surfel_rays"] = counts[3];
			for (const char *name : { "surfels", "ray_results", "sample_history", "shared_samples", "ray_schedule", "counters", "cell_heads", "cell_links", "grid_sums" }) {
				Ref<FileAccess> dump = FileAccess::open(world.capture_directory.path_join(String(name) + ".bin"), FileAccess::WRITE);
				if (dump.is_valid()) {
					dump->store_buffer(rd->buffer_get_data(state->storage[name]));
				}
			}
			if (buffers->has_texture(SNAME("kiln_deferred"), SNAME("surface"))) {
				Ref<FileAccess> dump = FileAccess::open(world.capture_directory.path_join("surface.bin"), FileAccess::WRITE);
				if (dump.is_valid()) {
					dump->store_buffer(rd->texture_get_data(buffers->get_texture(SNAME("kiln_deferred"), SNAME("surface")), 0));
				}
				for (const char *name : { "albedo_metallic", "normal_roughness", "material", "emission" }) {
					Ref<FileAccess> attribute = FileAccess::open(world.capture_directory.path_join(String(name) + ".bin"), FileAccess::WRITE);
					if (attribute.is_valid()) {
						attribute->store_buffer(rd->texture_get_data(buffers->get_texture(SNAME("kiln_deferred"), StringName(name)), 0));
					}
				}
				metadata["surface_format"] = "RG32_UINT: draw instance/BRDF flag, octahedral geometric normal";
			}
			float maximum_bounds_error = 0;
			for (bool dynamic : { false, true }) {
				const auto &geometry = dynamic ? world.dynamic : world.world;
				PackedByteArray gpu = rd->buffer_get_data(state->storage[dynamic ? "dynamic_nodes" : "nodes"], 0, geometry.nodes.size());
				const float *actual = reinterpret_cast<const float *>(gpu.ptr());
				const float *expected = reinterpret_cast<const float *>(geometry.nodes.ptr());
				for (uint32_t n = 0; n < geometry.node_count * 8; n++) {
					maximum_bounds_error = MAX(maximum_bounds_error, Math::abs(actual[n] - expected[n]));
				}
			}
			metadata["backend"] = state->hardware_active ? "hardware_ray_query" : "compute_software_bvh";
			metadata["hardware_blas_builds"] = state->hardware_builds;
			metadata["hardware_tlas_builds"] = state->tlas_builds;
			if (state->hardware_active) {
				RID validation = allocate("query_validation", 2048 * 16);
				dispatch(QUERY_VALIDATE, Size2i(2048, 1), { U(), B(4, "nodes"), B(5, "triangles"), B(16, "dynamic_nodes"), B(17, "dynamic_triangles"), B(18, "emitters"), B(25, "local_lights"), B(26, "light_grid"), S(32, state->ray_albedo, true), B(33, "texture_coordinates"), B(34, "dynamic_texture_coordinates"), { 28, RD::UNIFORM_TYPE_STORAGE_BUFFER, validation } }, 0, 1, query_tlas);
				PackedByteArray values = rd->buffer_get_data(validation);
				const float *r = reinterpret_cast<const float *>(values.ptr());
				int mismatches = 0, hits = 0;
				float error = 0;
				for (int ray = 0; ray < 2048; ray++) {
					mismatches += int(r[ray * 4]) + int(r[ray * 4 + 2]);
					error = MAX(error, r[ray * 4 + 1]);
					hits += int(r[ray * 4 + 3]);
				}
				metadata["hardware_query_rays"] = 2048;
				metadata["hardware_query_hit_rays"] = hits;
				metadata["hardware_query_mismatches"] = mismatches;
				metadata["hardware_query_maximum_error"] = error;
			}
			metadata["compute_bvh_maximum_error"] = maximum_bounds_error;
			metadata["width"] = state->size.x;
			metadata["height"] = state->size.y;
			metadata["frame"] = state->frames;
			metadata["moving"] = moving;
			metadata["lighting_response"] = state->lighting_response;
			// Retained for capture-schema compatibility. Relighting no longer uses
			// a fixed frame countdown; lighting_response records the live bandwidth.
			metadata["relighting_frames"] = 0;
			metadata["screen_history_valid"] = state->frames > 0 && !initialize;
			metadata["stationary_samples"] = state->stationary_samples;
			metadata["geometry_version"] = world.geometry_version;
			metadata["dynamic_version"] = world.dynamic_version;
			metadata["light_version"] = world.light_version;
			for (const char *name : { "raw", "diffuse", "specular", "fresnel", "specular_raw", "fresnel_raw", "ao", "confidence" }) {
				String key(name);
				RID rid = T(key, key == "confidence" ? current : 0);
				Ref<FileAccess> file = FileAccess::open(world.capture_directory.path_join(key + ".bin"), FileAccess::WRITE);
				if (file.is_valid()) {
					file->store_buffer(rd->texture_get_data(rid, 0));
				}
			}
			if (use_nrd) {
				for (const char *name : { "nrd_diffuse_raw", "nrd_normal", "nrd_depth", "nrd_motion", "nrd_base", "nrd_fresnel", "nrd_specular_basis", "diffuse_reference" }) {
					if (String(name) == "diffuse_reference" && !nrd_validation) continue;
					Ref<FileAccess> signal = FileAccess::open(world.capture_directory.path_join(String(name) + ".bin"), FileAccess::WRITE);
					if (signal.is_valid()) signal->store_buffer(rd->texture_get_data(T(name), 0));
				}
			}
			Ref<FileAccess> file = FileAccess::open(world.capture_directory.path_join("metadata.json"), FileAccess::WRITE);
			if (file.is_valid()) {
				file->store_string(JSON::stringify(metadata, "\t"));
			}
		}
	}
	Dictionary statistics;
	statistics["rendered_frames"] = state->frames + 1;
	statistics["width"] = state->size.x;
	statistics["height"] = state->size.y;
	statistics["moving"] = moving;
	statistics["lighting_response"] = state->lighting_response;
	statistics["stationary_samples"] = state->stationary_samples;
	statistics["gi_enabled"] = world.enabled;
	statistics["gi_algorithm"] = "Surfel GI (SurfelPlus adaptation)";
	statistics["surfel_capacity"] = state->slots;
	statistics["surfel_multibounce"] = multibounce;
	statistics["surfel_reconstruction"] = reconstruct_diffuse;
	statistics["surfel_target_diameter_pixels"] = surfel_target_diameter;
	statistics["surfel_ray_budget"] = surfel_ray_budget;
	statistics["surfel_ray_budget_configured"] = configured_surfel_ray_budget;
	statistics["surfel_ray_budget_adaptive"] = adaptive_surfel_budget;
	statistics["surfel_ray_budget_mode"] = surfel_ray_budget_mode;
	statistics["irradiance_sharing"] = irradiance_sharing;
	statistics["specular_rays"] = specular_rays;
	statistics["specular_implementation"] = state->hardware_active && native_specular_version.is_valid() ? "handwritten_msl" : "translated_glsl";
	statistics["specular_checkerboard"] = specular_checkerboard && !use_nrd;
	statistics["nrd_active"] = use_nrd;
	statistics["nrd_version"] = use_nrd ? "4.17.3" : "disabled";
	statistics["nrd_checkerboard"] = nrd_checkerboard;
	statistics["nrd_diffuse_rays"] = 0;
	statistics["nrd_diffuse_iterations"] = nrd_diffuse_iterations;
	statistics["nrd_combined_specular"] = use_nrd && combined_specular;
	statistics["nrd_same_input_reference"] = nrd_validation;
	statistics["nrd_signals"] = use_nrd ? (specular_rays > 0 ? (combined_specular ? "cache_diffuse_and_demodulated_specular" : "cache_diffuse_and_specular_schlick_integrals") : "cache_diffuse") : "disabled";
	statistics["surfel_grid"] = "compact_overlap_lists";
	statistics["backend"] = state->hardware_active ? "hardware_ray_query" : "compute_software_bvh";
	statistics["hardware_ray_query_available"] = hardware_available;
	statistics["hardware_blas_builds"] = state->hardware_builds;
	statistics["hardware_tlas_builds"] = state->tlas_builds;
	statistics["hardware_fallback"] = world.query_backend != 1 && !state->hardware_active;
	KilnWorld::report(environment, statistics);
	state->previous_vp = vp;
	state->previous_camera = scene->cam_transform;
	state->previous_projection = scene->cam_projection;
	state->geometry_version = world.geometry_version;
	state->dynamic_version = world.dynamic_version;
	state->light_version = world.light_version;
	state->material_version = world.material_version;
	state->static_material_version = world.static_material_version;
	state->dynamic_material_version = world.dynamic_material_version;
	state->frames++;
	state->index = previous;
	if (!moving) {
		state->stationary_samples = MIN(world.samples, state->stationary_samples + world.rays);
	}
	return true;
}
