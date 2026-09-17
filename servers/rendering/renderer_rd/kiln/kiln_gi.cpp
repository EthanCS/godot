// Kiln engine integration. Engine licensing: LICENSE.txt.
// SurfelPlus adaptation: thirdparty/surfelplus/NOTICE and kiln/docs/SURFEL-GI.md.

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
	const char *stages[] = { "SURFEL_UPDATE", "SURFEL_GRID", "SURFEL_GENERATE", "SURFEL_TRACE", "SURFEL_INTEGRATE", "SURFEL_EVALUATE", "SURFEL_PUBLISH", "SURFEL_SPECULAR", "SURFEL_SPECULAR_FILTER", "SURFEL_DEBUG", "SEQUENCE_LUT", "XEGTAO_DEPTH", "XEGTAO_MAIN", "XEGTAO_DENOISE", "XEGTAO_TEMPORAL", "BVH_REFIT", "SURFEL_GRID_PREFIX", "SURFEL_GRID_PREFIX_SUMS", "SURFEL_GRID_SCATTER", "SURFEL_DIFFUSE_FILTER", "NRD_PREPARE", "NRD_DIFFUSE", "NRD_RESOLVE" };
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
		for (const char *stage : { "SURFEL_GENERATE", "SURFEL_TRACE", "QUERY_VALIDATE", "SURFEL_SPECULAR", "NRD_DIFFUSE" }) {
			hardware_defines.push_back(String("\n#define KILN_HARDWARE_RAY_QUERY\n#define STAGE_") + stage + "\n");
		}
		hardware_shader.initialize(hardware_defines);
		hardware_version = hardware_shader.version_create();
		hardware_shader.version_set_compute_code(hardware_version, HashMap<String, String>(), "", "", Vector<String>());
		for (int i = 0; i < 5; i++) {
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
	if (nrd) {
		memdelete(nrd);
		nrd = nullptr;
	}
	nrd_active = false;
	free_hardware();
	if (ray_albedo.is_valid()) {
		RD::get_singleton()->free_rid(ray_albedo);
		ray_albedo = RID();
	}
	hardware_failed = false;
	hardware_builds = tlas_builds = 0;
	for (RID rid : owned) {
		if (rid.is_valid()) {
			RD::get_singleton()->free_rid(rid);
		}
	}
	for (const KeyValue<String, RID> &entry : storage) {
		RD::get_singleton()->free_rid(entry.value);
	}
	owned.clear();
	storage.clear();
	capacities.clear();
	textures.clear();
	frames = index = stationary_samples = 0;
	geometry_version = dynamic_version = light_version = material_version = texture_version = 0;
	static_material_version = dynamic_material_version = 0;
	ready = false;
}
void KilnGI::dispatch(Stage stage, Size2i size, std::initializer_list<Binding> bindings, int stride, int z, RID tlas, bool force_translated, Vector2 jitter_delta) {
	RD *rd = RD::get_singleton();
	static const char *stage_names[] = { "Update", "Grid", "Generate", "Trace diffuse", "Integrate", "Evaluate", "Publish diffuse", "Trace specular", "Filter specular", "Debug", "Sequence", "AO depth", "AO main", "AO denoise", "AO temporal", "BVH refit", "Grid prefix", "Grid prefix sums", "Grid scatter", "Filter diffuse", "NRD prepare", "NRD diffuse rays", "NRD resolve", "Query validation" };
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
			stage_mask = surfel_data | bvh_data | (1ull << 28);
			break;
		case SURFEL_INTEGRATE:
			stage_mask = surfel_data | (1ull << 28);
			break;
		case SURFEL_EVALUATE:
			stage_mask = surfel_data | (3ull << 1) | (1ull << 6) | (1ull << 8) | (1ull << 31);
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
		int variant = stage == NRD_DIFFUSE ? 4 : stage == SURFEL_GENERATE ? 0
																		  : (stage == SURFEL_TRACE ? 1 : (stage == SURFEL_SPECULAR ? 3 : 2));
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
	if (stage == BVH_REFIT || stage == AO_DEPTH || stage == AO_DENOISE || stage == AO_TEMPORAL || stage == SURFEL_DIFFUSE_FILTER) {
		int push[4] = { stride, size.x, 0, 0 };
		rd->compute_list_set_push_constant(list, push, (stage == BVH_REFIT) ? sizeof(push) : sizeof(int));
	}
	if (stage == NRD_PREPARE) {
		float jitter[2] = { float(jitter_delta.x), float(jitter_delta.y) };
		rd->compute_list_set_push_constant(list, jitter, sizeof(jitter));
	}
	uint32_t group_width = stage == SURFEL_SPECULAR ? 16 : 8;
	uint32_t group_height = stage == SURFEL_SPECULAR ? 2 : 8;
	rd->compute_list_dispatch(list, (size.x + group_width - 1) / group_width, (size.y + group_height - 1) / group_height, z);
	rd->compute_list_end();
	RENDER_TIMESTAMP("Kiln / between passes");
}
bool KilnGI::process(Ref<RenderSceneBuffersRD> buffers, RenderSceneDataRD *scene, RID environment, RID full_normal, bool signed_normal) {
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
		state->free_data();
		state->environment = environment;
	}
	auto own = [&](RID rid) { state->owned.push_back(rid); return rid; };
	auto allocate = [&](String name, uint32_t bytes) {
		if (!state->storage.has(name) || state->capacities[name] < bytes) {
			if (state->storage.has(name)) {
				rd->free_rid(state->storage[name]);
			}
			state->capacities[name] = MAX(bytes, state->capacities.has(name) ? state->capacities[name] * 2 : 16u);
			state->storage[name] = rd->storage_buffer_create(state->capacities[name]);
		}
		return state->storage[name];
	};
	auto upload = [&](String name, const PackedByteArray &bytes) { RID rid = allocate(name, bytes.size()); rd->buffer_update(rid, 0, bytes.size(), bytes.ptr()); return rid; };
	bool initialize = !state->ready;
	if (initialize) {
		state->size = buffers->get_internal_size();
		// Keep the cache density consistent as the full-resolution receiver count
		// grows. Workgroups address rows of 256 slots; bound memory at 32 MiB.
		uint32_t requested_slots = ((uint64_t(state->size.x) * state->size.y + 2047) / 2048) * 256;
		state->slots = CLAMP(requested_slots, 65536u, 262144u);
		state->parameters = own(rd->uniform_buffer_create(704));
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
		allocate("surfels", state->slots * 128);
		allocate("cell_heads", 262144 * 8);
		allocate("cell_links", state->slots * 27 * 4);
		allocate("grid_sums", 4096 * 4);
		allocate("free_slots", state->slots * 4);
		allocate("counters", 16 + 262144 * 4);
		rd->buffer_clear(state->storage["counters"], 0, 16 + 262144 * 4);
		allocate("ray_results", state->slots * 16);
		rd->buffer_clear(state->storage["surfels"], 0, state->slots * 128);
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
	if (initialize || state->texture_version != world.texture_version) {
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
	if (changed_light) {
		// Keep the short lighting estimator active for a full motion window.
		// Returning to 256-ray accumulation after only eight frames preserves
		// a visible fraction of the previous illumination after lights turn off.
		state->lighting_remaining = 32;
		state->epoch++;
	} else if (state->lighting_remaining) {
		state->lighting_remaining--;
	}
	bool camera_moved = state->frames > 0 && (scene->cam_transform != state->previous_camera || scene->cam_projection != state->previous_projection);
	if (camera_moved) {
		state->motion_remaining = 8;
	} else if (state->motion_remaining) {
		state->motion_remaining--;
	}
	bool moving = camera_moved || state->motion_remaining || state->lighting_remaining;
	if (moving) {
		state->stationary_samples = 0;
	}
	Projection projection = scene->get_cam_projection();
	Projection vp = projection * Projection(scene->cam_transform.affine_inverse());
	float params[176] = {};
	int at = 0;
	for (const Projection &m : { projection, projection.inverse(), Projection(scene->cam_transform), Projection(scene->cam_transform.affine_inverse()), state->previous_vp }) {
		MaterialStorage::store_camera(m, params + at);
		at += 16;
	}
	auto v = [&](float x, float y, float z, float w) { params[at++] = x; params[at++] = y; params[at++] = z; params[at++] = w; };
	auto vec = [&](Vector3 a, float w) { v(a.x, a.y, a.z, w); };
	v(state->size.x, state->size.y, state->frames, state->frames > 0);
	v(1.2, 1, moving ? -1 : state->stationary_samples, world.samples);
	// A stationary camera still samples different receivers under TAA jitter.
	// Reproject those samples without classifying the camera as moving, so
	// stationary confidence can accumulate to the full quality budget.
	bool reproject = camera_moved || scene->taa_jitter != Vector2() || scene->prev_taa_jitter != Vector2();
	v(state->lighting_remaining ? MAX(4, world.rays) : world.rays, reproject, world.ao_quality, 0.22);
	vec(world.world.bounds.position, 0.025);
	vec(world.world.bounds.size, state->slots);
	bool use_nrd = world.enabled && KilnNRD::available() && bool(ProjectSettings::get_singleton()->get_setting("rendering/kiln/nrd", true));
	if (use_nrd && !state->nrd) {
		state->nrd = memnew(KilnNRD);
		if (!state->nrd->initialize(state->size)) {
			memdelete(state->nrd);
			state->nrd = nullptr;
		}
	}
	use_nrd = use_nrd && state->nrd;
	if (use_nrd != state->nrd_active) {
		state->frames = 0;
	}
	bool specular_checkerboard = ProjectSettings::get_singleton()->get_setting("rendering/kiln/specular_checkerboard", true);
	v(world.world.triangle_count, specular_checkerboard && !use_nrd, use_nrd, state->epoch);
	vec(world.sun_direction, world.sun_energy * Math::PI);
	vec(world.sun_color, world.sky_energy);
	vec(world.sky_horizon, 0);
	int debug_mode = ProjectSettings::get_singleton()->get_setting("rendering/kiln/debug_view", 0);
	float debug_radius = CLAMP(float(ProjectSettings::get_singleton()->get_setting("rendering/kiln/surfel_debug_radius", 0.45)), 0.15f, 1.0f);
	float debug_gain = CLAMP(float(ProjectSettings::get_singleton()->get_setting("rendering/kiln/surfel_debug_gain", 4.0)), 0.1f, 32.0f);
	int specular_rays = ProjectSettings::get_singleton()->get_setting("rendering/kiln/surfel_specular", true) ? CLAMP(int(ProjectSettings::get_singleton()->get_setting("rendering/kiln/specular_rays", 2)), 1, 8) : 0;
	v(debug_mode, debug_radius, debug_gain, specular_rays);
	v(0, 0, 0, 1);
	vec(world.sky_zenith, world.procedural_sky ? 2 : 1);
	vec(world.procedural_sky ? Vector3() : Vector3(0.554, 0.464, 0.304), world.procedural_sky ? world.sky_halo * world.sun_energy : 0);
	vec(world.sun_direction, world.time_of_day);
	vec(world.procedural_sky ? world.cloud_color : Vector3(), world.procedural_sky ? world.cloud_coverage : 0);
	v(world.sky_saturation, 1, 0, 0);
	v(0, 0, 0, 1);
	v(world.world.node_count, world.world.triangle_count, world.enabled, state->lighting_remaining > 0);
	v(world.dynamic.node_count, world.dynamic.triangle_count, emitter_count, world.world.power + world.dynamic.power);
	MaterialStorage::store_camera(state->previous_vp.inverse(), params + at);
	at += 16;
	bool has_surface = buffers->has_texture(SNAME("kiln_deferred"), SNAME("surface"));
	RID surface_input = has_surface ? buffers->get_texture(SNAME("kiln_deferred"), SNAME("surface")) : empty_surface;
	v(signed_normal, has_surface, multibounce, changed_dynamic);
	ERR_FAIL_COND_V(at != 176, false);
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
	if (world.enabled) {
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
								B(20, "surfels"), B(21, "cell_heads"), B(22, "cell_links"), B(23, "free_slots"), B(24, "counters"), B(28, "ray_results"), B(35, "grid_sums") },
						0, 1, tlas);
			} else {
				dispatch(stage, work,
						{ U(), S(1, depth), S(2, normal), S(31, surface_input), I(6, T("raw")), I(8, T("confidence", current)),
								B(20, "surfels"), B(21, "cell_heads"), B(22, "cell_links"), B(23, "free_slots"), B(24, "counters"), B(28, "ray_results"), B(35, "grid_sums") });
			}
		};
		auto build_grid = [&]() {
			rd->buffer_clear(state->storage["cell_heads"], 0, 262144 * 8);
			surfel_pass(SURFEL_GRID, false, false);
			surfel_pass(SURFEL_GRID_PREFIX, false, false);
			surfel_pass(SURFEL_GRID_PREFIX_SUMS, false, false);
			surfel_pass(SURFEL_GRID_SCATTER, false, false);
		};
		rd->buffer_clear(state->storage["counters"], 0, 16 + 262144 * 4);
		surfel_pass(SURFEL_UPDATE, true, false);
		build_grid();
		surfel_pass(SURFEL_GENERATE, true, true, query_tlas);
		build_grid();
		surfel_pass(SURFEL_TRACE, true, false, query_tlas);
		surfel_pass(SURFEL_INTEGRATE, false, false);
		surfel_pass(SURFEL_EVALUATE, false, true);
		trace_specular(T("specular_raw"), T("fresnel_raw"), false);
		if (use_nrd) {
			dispatch(NRD_DIFFUSE, state->size,
					{ U(), S(1, depth), S(2, normal), S(31, surface_input), B(4, "nodes"), B(5, "triangles"), B(16, "dynamic_nodes"), B(17, "dynamic_triangles"), B(18, "emitters"), B(25, "local_lights"), B(26, "light_grid"), S(32, state->ray_albedo, true), B(33, "texture_coordinates"), B(34, "dynamic_texture_coordinates"),
							B(20, "surfels"), B(21, "cell_heads"), B(22, "cell_links"), B(23, "free_slots"), B(24, "counters"), B(35, "grid_sums"), I(6, T("nrd_diffuse_raw")) },
					0, 1, query_tlas);
		}
	} else {
		rd->texture_clear(T("raw"), Color(0, 0, 0, 0), 0, 1, 0, 1);
	}
	if (state->ao_quality != world.ao_quality || state->frames == 0) {
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
	RID diffuse_input = T("raw");
	if (world.enabled && !use_nrd) {
		for (int pass = 0; pass < 4; pass++) {
			RID output = T(pass % 2 == 0 ? "diffuse_work" : "diffuse_spatial");
			dispatch(SURFEL_DIFFUSE_FILTER, state->size, { U(), S(1, depth), S(2, normal), S(31, surface_input), S(3, diffuse_input), I(6, output) }, 1 << pass);
			diffuse_input = output;
		}
	}
	dispatch(SURFEL_PUBLISH, state->size, { U(), S(1, depth), S(2, normal), S(31, surface_input), S(3, diffuse_input), I(6, T("diffuse")), I(9, T("display_diffuse", current)), I(10, T("display_specular", current)), S(11, T("display_diffuse", previous)), S(12, T("display_specular", previous)) });

	if (use_nrd) {
		RID motion = has_surface && buffers->has_velocity_buffer(false) ? buffers->get_velocity_buffer(false) : normal;
		dispatch(NRD_PREPARE, state->size, { U(), S(1, depth), S(2, normal), S(3, motion), S(4, T("specular_raw")), S(5, T("fresnel_raw")), I(6, T("nrd_normal")), I(7, T("nrd_depth")), I(8, T("nrd_motion")), I(9, T("nrd_base")), I(10, T("nrd_fresnel")) }, 0, 1, RID(), false, (scene->taa_jitter - scene->prev_taa_jitter) * 0.5);
		RENDER_TIMESTAMP("Kiln / NRD RELAX");
		bool reset = !state->nrd_active || state->frames == 0 || changed_world || changed_material;
		if (!has_surface && changed_dynamic) {
			reset = true;
		}
		use_nrd = state->nrd->denoise(scene, state->previous_projection, state->previous_camera, state->frames, reset, state->lighting_remaining > 0,
				T("nrd_motion"), T("nrd_normal"), T("nrd_depth"), T("nrd_diffuse_raw"), T("nrd_base"), T("nrd_fresnel"), T("diffuse"), T("specular"), T("fresnel"));
		RENDER_TIMESTAMP("Kiln / between passes");
	}
	if (use_nrd) {
		dispatch(NRD_RESOLVE, state->size, { U(), S(1, depth), I(6, T("diffuse")), I(7, T("specular")), I(8, T("fresnel")) });
	}
	if (!use_nrd) {
		dispatch(SURFEL_SPECULAR_FILTER, state->size,
				{ U(), S(1, depth), S(2, normal), S(3, T("specular_raw")), S(4, T("fresnel_raw")), S(5, T("reflection_base", previous)), S(6, T("reflection_fresnel", previous)), S(7, T("reflection_geometry", previous)), S(8, T("display_diffuse", previous)),
						I(9, T("reflection_base", current)), I(10, T("reflection_fresnel", current)), I(11, T("reflection_geometry", current)), I(12, T("specular")), I(13, T("fresnel")) });
	}

	state->nrd_active = use_nrd;

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
			metadata["nrd_diffuse_rays"] = use_nrd ? 2 : 0;
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
			for (const char *name : { "surfels", "ray_results", "counters", "cell_heads", "cell_links", "grid_sums" }) {
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
	statistics["stationary_samples"] = state->stationary_samples;
	statistics["gi_enabled"] = world.enabled;
	statistics["gi_algorithm"] = "Surfel GI (SurfelPlus adaptation)";
	statistics["surfel_capacity"] = state->slots;
	statistics["surfel_multibounce"] = multibounce;
	statistics["specular_rays"] = specular_rays;
	statistics["specular_implementation"] = state->hardware_active && native_specular_version.is_valid() ? "handwritten_msl" : "translated_glsl";
	statistics["specular_checkerboard"] = specular_checkerboard && !use_nrd;
	statistics["nrd_active"] = use_nrd;
	statistics["nrd_version"] = use_nrd ? "4.17.3" : "disabled";
	statistics["nrd_diffuse_rays"] = use_nrd ? 2 : 0;
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
