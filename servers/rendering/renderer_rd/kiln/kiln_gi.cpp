// Kiln engine integration. Engine licensing: LICENSE.txt.
// Pipeline: kiln/docs/PIPELINE.md. Algorithm notices: kiln/provenance and this directory's licenses/.

#include "kiln_gi.h"

#include "blue_noise.gen.h"
#include "rtr_noise.gen.h"

#include "core/config/project_settings.h"
#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/io/json.h"
#include "core/os/os.h"
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
	f.usage_bits = RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_STORAGE_BIT | RD::TEXTURE_USAGE_CAN_COPY_FROM_BIT | RD::TEXTURE_USAGE_CAN_COPY_TO_BIT | RD::TEXTURE_USAGE_CAN_UPDATE_BIT;
	RID rid = RD::get_singleton()->texture_create(f, RD::TextureView());
	RD::get_singleton()->texture_clear(rid, Color(0, 0, 0, 0), 0, 1, 0, 1);
	return rid;
}
KilnGI::KilnGI() {
	Vector<String> defines;
	const char *stages[] = { "BVH_REFIT", "KILN_SKY", "KILN_SURFEL_CLEAR_POOL", "KILN_SURFEL_FIND_MISSING", "KILN_SURFEL_ARGS", "KILN_SURFEL_AGE", "KILN_SURFEL_ALLOCATE", "KILN_SURFEL_CLEAR_CELLS", "KILN_SURFEL_COUNT_CELLS", "KILN_SURFEL_SCAN", "KILN_SURFEL_SCAN_SEGMENTS", "KILN_SURFEL_SCAN_MERGE", "KILN_SURFEL_SLOT_CELLS", "KILN_SURFEL_TRACE", "KILN_RESTIR_TRACE", "KILN_RESTIR_TEMPORAL", "KILN_RESTIR_SPATIAL", "KILN_RESTIR_RESOLVE", "KILN_BRDF_LUT", "KILN_LIGHT", "KILN_RTDGI_REPROJECT", "KILN_RTDGI_TEMPORAL_FILTER", "KILN_RTDGI_SPATIAL_FILTER", "KILN_RTDGI_VALIDITY", "KILN_RTR_TRACE", "KILN_RTR_TEMPORAL", "KILN_RTR_RESOLVE", "KILN_RTR_FILTER", "KILN_RTR_CLEANUP", "KILN_SSGI", "KILN_SSGI_SPATIAL", "KILN_SSGI_UPSAMPLE", "KILN_SSGI_TEMPORAL", "KILN_SHADOW_TRACE", "KILN_SHADOW_BITPACK", "KILN_SHADOW_TEMPORAL", "KILN_SHADOW_SPATIAL", "KILN_TAA_REPROJECT", "KILN_TAA_INPUT", "KILN_TAA_HISTORY", "KILN_TAA_PROB", "KILN_TAA_PROB_FILTER", "KILN_TAA_PROB_FILTER2", "KILN_TAA", "KILN_DISPLAY_LUT", "KILN_POST", "KILN_POST_BLUR0", "KILN_POST_BLUR", "KILN_POST_REVERSE", "KILN_RTDGI_HISTORY_REPROJECT", "KILN_WRC_TRACE", "KILN_VELOCITY_REDUCE_X", "KILN_VELOCITY_REDUCE_Y", "KILN_VELOCITY_DILATE", "KILN_MOTION_BLUR", "KILN_NRD_PREPARE", "KILN_NRD_REPROJECT" };
	static_assert(sizeof(stages) / sizeof(stages[0]) == STAGE_COUNT);
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
		for (const char *stage : { "KILN_SURFEL_TRACE", "KILN_RESTIR_TRACE", "KILN_LIGHT", "KILN_RTR_TRACE", "KILN_SHADOW_TRACE", "KILN_WRC_TRACE" }) {
			hardware_defines.push_back(String("\n#define KILN_HARDWARE_RAY_QUERY\n#define STAGE_") + stage + "\n");
		}
		hardware_shader.initialize(hardware_defines);
		hardware_version = hardware_shader.version_create();
		hardware_shader.version_set_compute_code(hardware_version, HashMap<String, String>(), "", "", Vector<String>());
		for (int i = 0; i < HARDWARE_STAGE_COUNT; i++) {
			RID code = hardware_shader.version_get_shader(hardware_version, i);
			if (code.is_valid()) {
				hardware_pipelines[i] = RD::get_singleton()->compute_pipeline_create(code);
			}
			hardware_available &= hardware_pipelines[i].is_valid();
		}
	}
	RD::SamplerState state;
	sampler = RD::get_singleton()->sampler_create(state);
	state.min_filter = state.mag_filter = RD::SAMPLER_FILTER_LINEAR;
	state.repeat_u = state.repeat_v = RD::SAMPLER_REPEAT_MODE_REPEAT;
	linear_sampler = RD::get_singleton()->sampler_create(state);
	state.repeat_u = state.repeat_v = RD::SAMPLER_REPEAT_MODE_CLAMP_TO_EDGE;
	linear_clamp_sampler = RD::get_singleton()->sampler_create(state);
	Vector<uint8_t> rtr_data;
	rtr_data.resize(sizeof(kiln_rtr_noise_bytes) * sizeof(uint32_t));
	uint32_t *rtr_words = reinterpret_cast<uint32_t *>(rtr_data.ptrw());
	for (uint32_t i = 0; i < sizeof(kiln_rtr_noise_bytes); i++) {
		rtr_words[i] = kiln_rtr_noise_bytes[i];
	}
	rtr_noise = RD::get_singleton()->storage_buffer_create(rtr_data.size(), rtr_data);
	Vector<uint8_t> noise_png;
	noise_png.resize(sizeof(kiln_blue_noise_png));
	memcpy(noise_png.ptrw(), kiln_blue_noise_png, sizeof(kiln_blue_noise_png));
	Ref<Image> noise_image;
	noise_image.instantiate();
	ERR_FAIL_COND(noise_image->load_png_from_buffer(noise_png) != OK);
	noise_image->convert(Image::FORMAT_RGBA8);
	blue_noise = texture(Size2i(256, 256), RD::DATA_FORMAT_R8G8B8A8_UNORM);
	RD::get_singleton()->texture_update(blue_noise, 0, noise_image->get_data());
	print_line(vformat("[KILN_GI] hardware ray query=%s; compute software BVH fallback available", hardware_available));
}
KilnGI::~KilnGI() {
	shader.version_free(version);
	if (hardware_version.is_valid()) {
		hardware_shader.version_free(hardware_version);
	}
	for (RID rid : { sampler, linear_sampler, linear_clamp_sampler, blue_noise, rtr_noise }) {
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
	// RenderSceneBuffersRD calls this on viewport reconfiguration, including
	// resize. Only screen-space resources depend on that configuration.
	for (RID rid : owned) {
		if (rid.is_valid()) {
			RD::get_singleton()->free_rid(rid);
		}
	}
	owned.clear();
	textures.clear();
	frames = index = 0;
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
	frames = index = 0;
	geometry_version = dynamic_version = material_version = texture_version = 0;
	static_material_version = dynamic_material_version = 0;
	ready = false;
}
void KilnGI::dispatch(Stage stage, Size2i size, const std::vector<Binding> &bindings, int stride, int z, RID tlas) {
	RD *rd = RD::get_singleton();
	static const char *stage_names[] = { "BVH refit", "sky", "surfel pool init", "find missing surfels", "surfel args", "age surfels", "allocate surfels", "clear cells", "count surfels per cell", "prefix scan", "prefix scan segments", "prefix scan merge", "slot surfels into cells", "trace irradiance", "ReSTIR trace", "ReSTIR temporal", "ReSTIR spatial", "ReSTIR resolve", "BRDF FG LUT", "Deferred sun and GI", "RTDGI reproject", "RTDGI temporal filter", "RTDGI spatial filter", "RTDGI path validity", "RTR trace", "RTR ReSTIR temporal", "RTR resolve", "RTR temporal filter", "RTR cleanup", "SSGI AO", "SSGI spatial", "SSGI upsample", "SSGI temporal", "Sun shadow rays", "Shadow bitpack", "Shadow temporal", "Shadow spatial", "taa reproject", "taa input", "taa history", "taa prob", "taa prob filter", "taa prob filter2", "taa", "display lut", "post", "post blur0", "post blur", "post reverse", "RTDGI history reprojection", "World radiance cache", "Velocity reduce X", "Velocity reduce Y", "Velocity dilate", "Motion blur", "NRD prepare", "NRD diffuse feedback" };
	static_assert(sizeof(stage_names) / sizeof(stage_names[0]) == STAGE_COUNT);
	RENDER_TIMESTAMP(String("Kiln / ") + stage_names[stage]);
	LocalVector<RD::Uniform> uniforms;
	for (const Binding &b : bindings) {
		RD::Uniform u;
		u.binding = b.binding;
		u.uniform_type = b.type;
		if (b.type == RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE) {
			u.append_id(b.linear ? (b.clamp ? linear_clamp_sampler : linear_sampler) : sampler);
		}
		u.append_id(b.resource);
		uniforms.push_back(u);
	}
	RID code, pipeline;
	if (tlas.is_valid()) {
		int variant = -1;
		switch (stage) {
			case KILN_SURFEL_TRACE:
				variant = 0;
				break;
			case KILN_RESTIR_TRACE:
				variant = 1;
				break;
			case KILN_LIGHT:
				variant = 2;
				break;
			case KILN_RTR_TRACE:
				variant = 3;
				break;
			case KILN_SHADOW_TRACE:
				variant = 4;
				break;
			case KILN_WRC_TRACE:
				variant = 5;
				break;
			default:
				ERR_FAIL_MSG("Stage does not support hardware ray queries.");
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
	RID set = UniformSetCacheRD::get_singleton()->get_cache_vec(code, 0, uniforms);
	RD::ComputeListID list = rd->compute_list_begin();
	rd->compute_list_bind_compute_pipeline(list, pipeline);
	rd->compute_list_bind_uniform_set(list, set, 0);
	if (stage == BVH_REFIT || stage == KILN_RESTIR_SPATIAL || stage == KILN_SHADOW_SPATIAL) {
		int push[4] = { stride, size.x, 0, 0 };
		rd->compute_list_set_push_constant(list, push, (stage == BVH_REFIT) ? sizeof(push) : sizeof(int));
	}
	// BVH refit uses 8x8 groups; all other callers supply workgroup counts.
	uint32_t group_width = stage == BVH_REFIT ? 8 : 1;
	uint32_t group_height = stage == BVH_REFIT ? 8 : 1;
	rd->compute_list_dispatch(list, (size.x + group_width - 1) / group_width, (size.y + group_height - 1) / group_height, z);
	rd->compute_list_end();
	RENDER_TIMESTAMP("Kiln / between passes");
}
bool KilnGI::process(Ref<RenderSceneBuffersRD> buffers, RenderSceneDataRD *scene, RID environment, RID full_normal) {
	KilnWorld world;
	if (!KilnWorld::read(environment, world) || !world.enabled || world.geometry_version == 0 || !buffers->has_texture(SNAME("kiln_deferred"), SNAME("surface"))) {
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
		if (initialize_cache) {
			state->parameters = rd->uniform_buffer_create(176 * sizeof(float));
		}
		state->textures["diffuse0"] = own(texture(state->size, RD::DATA_FORMAT_R16G16B16A16_SFLOAT));
		if (KilnNRD::available()) {
			state->nrd = memnew(KilnNRD);
			if (!state->nrd->initialize(state->size)) {
				memdelete(state->nrd);
				state->nrd = nullptr;
				WARN_PRINT("Kiln NRD initialization failed; using legacy GI filters.");
			} else {
				for (const char *name : { "nrd_motion", "nrd_diffuse", "nrd_specular" }) {
					state->textures[String(name) + "0"] = own(texture(state->size, RD::DATA_FORMAT_R16G16B16A16_SFLOAT));
				}
				state->textures["nrd_normal0"] = own(texture(state->size, RD::DATA_FORMAT_A2B10G10R10_UNORM_PACK32));
				state->textures["nrd_depth0"] = own(texture(state->size, RD::DATA_FORMAT_R32_SFLOAT));
			}
		} else {
			WARN_PRINT_ONCE("Kiln NRD requires the pinned NRD SDK and Vulkan; using legacy GI filters.");
		}
		state->ready = true;
	}
	if (state->history_version != world.history_version) {
		state->frames = 0;
		state->history_version = world.history_version;
	}
	if (state->tracing != world.enabled) {
		state->frames = 0;
		state->tracing = world.enabled;
	}
	bool changed_world = state->geometry_version != world.geometry_version;
	bool changed_dynamic = state->dynamic_version != world.dynamic_version;
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
			const PackedByteArray &vertices = source.vertex_positions;
			if (vertices.size() != source.triangle_count * 9 * sizeof(float)) {
				success = false;
				break;
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
	if (!changed_world && state->static_material_version != world.static_material_version) {
		upload("triangles", world.world.triangles);
	}
	if (!changed_dynamic && state->dynamic_material_version != world.dynamic_material_version) {
		upload("dynamic_triangles", world.dynamic.triangles);
	}
	Projection projection = scene->get_cam_projection();
	if (state->frames == 0) {
		state->previous_camera = scene->cam_transform;
		state->previous_projection = scene->cam_projection;
	}
	float params[176] = {};
	int at = 0;
	for (const Projection &m : { projection, projection.inverse(), Projection(scene->cam_transform), Projection(scene->cam_transform.affine_inverse()) }) {
		MaterialStorage::store_camera(m, params + at);
		at += 16;
	}
	auto v = [&](float x, float y, float z, float w) { params[at++] = x; params[at++] = y; params[at++] = z; params[at++] = w; };
	auto vec = [&](Vector3 a, float w) { v(a.x, a.y, a.z, w); };
	v(state->size.x, state->size.y, state->frames, state->frames > 0 && !initialize);
	vec(world.sun_direction, world.sun_energy * Math::PI);
	vec(world.sun_color, world.sky_energy);
	v(world.world.node_count, world.world.triangle_count, world.enabled, 0);
	v(world.dynamic.node_count, world.dynamic.triangle_count, 0, 0);
	RID surface_input = buffers->get_texture(SNAME("kiln_deferred"), SNAME("surface"));
	// Anchor the clipmap on the previous frame's camera position.
	vec(state->previous_camera.origin, 1.0f);
	const float kiln_sun_angular_radius_cos = Math::cos(Math::deg_to_rad(0.53f) * 0.5f);
	v(kiln_sun_angular_radius_cos, 0.0f, 1.0f, 0);
	v(-scene->taa_jitter.x * state->size.x * 0.5f, scene->taa_jitter.y * state->size.y * 0.5f, MAX(1e-4f, scene->time_step), 0.0f);
	Projection clip_correction;
	clip_correction.set_depth_correction(scene->flip_y);
	Projection unjittered_projection = clip_correction * scene->cam_projection;
	Projection previous_projection = clip_correction * state->previous_projection;
	for (const Projection &m : { unjittered_projection, unjittered_projection.inverse(), previous_projection * Projection(state->previous_camera.affine_inverse()), previous_projection.inverse(), Projection(state->previous_camera) }) {
		MaterialStorage::store_camera(m, params + at);
		at += 16;
	}
	ERR_FAIL_COND_V(at != 176, false);
	rd->buffer_update(state->parameters, 0, sizeof(params), params);
	auto U = [&]() { return Binding{ 0, RD::UNIFORM_TYPE_UNIFORM_BUFFER, state->parameters }; };
	auto S = [&](int binding, RID rid, bool linear = false) { return Binding{ binding, RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, rid, linear }; };
	auto C = [&](int binding, RID rid) { return Binding{ binding, RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, rid, true, true }; };
	auto B = [&](int binding, String name) { return Binding{ binding, RD::UNIFORM_TYPE_STORAGE_BUFFER, state->storage[name] }; };
	auto I = [&](int binding, RID rid) { return Binding{ binding, RD::UNIFORM_TYPE_IMAGE, rid }; };
	int current = state->index, previous = 1 - current;
	auto T = [&](String name, int i = 0) { return state->t(name, i); };
	// These textures are only needed by the SDK-unavailable/error fallback.
	// NRD output itself persists until the next frame's feedback reprojection.
	auto ensure_legacy_filters = [&]() {
		if (state->textures.has("rtdgi_filtered0")) {
			return;
		}
		for (int i = 0; i < 2; i++) {
			state->textures["rtdgi_history" + itos(i)] = own(texture(state->size, RD::DATA_FORMAT_R16G16B16A16_SFLOAT));
			state->textures["rtdgi_moments" + itos(i)] = own(texture(state->size, RD::DATA_FORMAT_R16G16_SFLOAT));
			state->textures["rtr_history" + itos(i)] = own(texture(state->size, RD::DATA_FORMAT_R16G16B16A16_SFLOAT));
		}
		state->textures["rtdgi_filtered0"] = own(texture(state->size, RD::DATA_FORMAT_R16G16B16A16_SFLOAT));
	};
	RID depth = buffers->get_depth_texture(), normal = full_normal;
	{
		// Persistent clipmap surfels, diffuse ReSTIR and indirect reflections.
		const uint32_t kmax = 262144u;
		allocate("kiln_meta", 8 * 4);
		allocate("kiln_pool", kmax * 4, true);
		allocate("kiln_cell_offset", (kmax + 1) * 4);
		allocate("kiln_index", kmax * 24 * 4);
		allocate("kiln_spatial", kmax * 16);
		allocate("kiln_irradiance", kmax * 16);
		allocate("kiln_aux", kmax * 32);
		allocate("kiln_life", kmax * 4);
		allocate("kiln_proposal", kmax * 16);
		allocate("kiln_scan_segments", 1024 * 4);
		allocate("kiln_args", 12 * 4);
		if (initialize) {
			rd->buffer_clear(state->storage["kiln_meta"], 0, 8 * 4);
			rd->buffer_clear(state->storage["kiln_life"], 0, kmax * 4);
			rd->buffer_clear(state->storage["kiln_cell_offset"], 0, (kmax + 1) * 4);
			dispatch(KILN_SURFEL_CLEAR_POOL, Size2i(kmax / 64, 1),
					{ U(), B(41, "kiln_pool"), B(40, "kiln_meta"), B(42, "kiln_cell_offset"), B(43, "kiln_index"), B(44, "kiln_spatial"), B(45, "kiln_irradiance"), B(46, "kiln_aux"), B(47, "kiln_life"), B(48, "kiln_proposal") });
		}
		if (!state->textures.has("kiln_sky")) {
			RD::TextureFormat tf;
			tf.format = RD::DATA_FORMAT_R16G16B16A16_SFLOAT;
			tf.texture_type = RD::TEXTURE_TYPE_CUBE;
			tf.width = 32;
			tf.height = 32;
			tf.array_layers = 6;
			tf.mipmaps = 1;
			tf.usage_bits = RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_STORAGE_BIT | RD::TEXTURE_USAGE_CAN_COPY_TO_BIT;
			state->textures["kiln_sky"] = own(rd->texture_create(tf, RD::TextureView()));
			RD::TextureFormat ta;
			ta.format = RD::DATA_FORMAT_R32G32_UINT;
			ta.width = (state->size.x + 7) / 8;
			ta.height = (state->size.y + 7) / 8;
			ta.usage_bits = RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_STORAGE_BIT | RD::TEXTURE_USAGE_COLOR_ATTACHMENT_BIT;
			state->textures["kiln_tile_alloc"] = own(rd->texture_create(ta, RD::TextureView()));
			RD::TextureFormat ti;
			ti.format = RD::DATA_FORMAT_R32G32B32A32_SFLOAT;
			ti.width = (state->size.x + 7) / 8;
			ti.height = (state->size.y + 7) / 8;
			ti.usage_bits = RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_STORAGE_BIT | RD::TEXTURE_USAGE_COLOR_ATTACHMENT_BIT;
			state->textures["kiln_tile_irradiance"] = own(rd->texture_create(ti, RD::TextureView()));
			Size2i half_size = (state->size + Size2i(1, 1)) / 2;
			auto make_restir = [&](const char *name, RD::DataFormat format, int count = 2) {
				for (int i = 0; i < count; i++) {
					state->textures[String(name) + itos(i)] = own(texture(half_size, format));
				}
			};
			state->textures["wrc_atlas0"] = own(texture(Size2i(512, 512), RD::DATA_FORMAT_R32G32B32A32_SFLOAT));
			state->textures["shadow_raw0"] = own(texture(state->size, RD::DATA_FORMAT_R8_UNORM));
			Size2i shadow_tiles((state->size.x + 7) / 8, (state->size.y + 3) / 4);
			state->textures["shadow_bitpack0"] = own(texture(shadow_tiles, RD::DATA_FORMAT_R32_UINT));
			state->textures["shadow_metadata0"] = own(texture(shadow_tiles, RD::DATA_FORMAT_R32_UINT));
			for (const char *name : { "shadow_spatial", "shadow_temp" }) {
				state->textures[String(name) + "0"] = own(texture(state->size, RD::DATA_FORMAT_R16G16_SFLOAT));
			}
			for (int i = 0; i < 2; i++) {
				state->textures["shadow_accum" + itos(i)] = own(texture(state->size, RD::DATA_FORMAT_R16G16_SFLOAT));
				state->textures["shadow_moments" + itos(i)] = own(texture(state->size, RD::DATA_FORMAT_R16G16B16A16_SFLOAT));
			}
			make_restir("ssgi_raw", RD::DATA_FORMAT_R16_SFLOAT, 1);
			make_restir("ssgi_spatial", RD::DATA_FORMAT_R16_SFLOAT, 1);
			state->textures["ssgi_upsampled0"] = own(texture(state->size, RD::DATA_FORMAT_R16_SFLOAT));
			state->textures["ssgi_final0"] = own(texture(state->size, RD::DATA_FORMAT_R8_UNORM));
			for (int i = 0; i < 2; i++) {
				state->textures["ssgi_history" + itos(i)] = own(texture(state->size, RD::DATA_FORMAT_R16_SFLOAT));
			}
			make_restir("rtr_candidate0", RD::DATA_FORMAT_R32G32B32A32_SFLOAT, 1);
			make_restir("rtr_candidate1", RD::DATA_FORMAT_R32G32B32A32_SFLOAT, 1);
			make_restir("rtr_candidate2", RD::DATA_FORMAT_R8G8B8A8_SNORM, 1);
			for (const char *name : { "rtr_irradiance", "rtr_origin", "rtr_hit", "rtr_reservoir" }) {
				make_restir(name, RD::DATA_FORMAT_R32G32B32A32_SFLOAT);
			}
			make_restir("rtr_hit_normal", RD::DATA_FORMAT_R16G16B16A16_SFLOAT);
			for (int i = 0; i < 2; i++) {
				state->textures["rtr_length" + itos(i)] = own(texture(state->size, RD::DATA_FORMAT_R16G16_SFLOAT));
			}
			state->textures["rtr_resolved0"] = own(texture(state->size, RD::DATA_FORMAT_R16G16B16A16_SFLOAT));
			state->textures["rtr_final0"] = own(texture(state->size, RD::DATA_FORMAT_R16G16B16A16_SFLOAT));
			make_restir("restir_candidate", RD::DATA_FORMAT_R16G16B16A16_SFLOAT, 1);
			make_restir("restir_candidate_hit", RD::DATA_FORMAT_R16G16B16A16_SFLOAT, 1);
			make_restir("restir_candidate_history", RD::DATA_FORMAT_R16G16B16A16_SFLOAT);
			make_restir("restir_invalidity", RD::DATA_FORMAT_R8_UNORM, 1);
			make_restir("restir_validity", RD::DATA_FORMAT_R16G16_SFLOAT);
			make_restir("restir_origin", RD::DATA_FORMAT_R32G32B32A32_SFLOAT);
			make_restir("restir_hit_normal", RD::DATA_FORMAT_R16G16B16A16_SFLOAT);
			make_restir("restir_spatial", RD::DATA_FORMAT_R32G32B32A32_SFLOAT);
			make_restir("restir_hit", RD::DATA_FORMAT_R32G32B32A32_SFLOAT);
			make_restir("restir_irradiance", RD::DATA_FORMAT_R16G16B16A16_SFLOAT);
			make_restir("restir_reservoir", RD::DATA_FORMAT_R32G32B32A32_SFLOAT);
			for (int i = 0; i < 2; ++i) {
				state->textures["rtdgi_geometry" + itos(i)] = own(texture(state->size, RD::DATA_FORMAT_R32G32B32A32_SFLOAT));
			}
			for (const char *name : { "rtdgi_reprojected", "rtdgi_reprojection", "rtdgi_raw" }) {
				state->textures[String(name) + "0"] = own(texture(state->size, RD::DATA_FORMAT_R16G16B16A16_SFLOAT));
			}
			state->textures["rtdgi_lighting0"] = own(texture(state->size, RD::DATA_FORMAT_R16G16B16A16_SFLOAT));
			state->textures["rtdgi_fg0"] = own(texture(Size2i(64, 64), RD::DATA_FORMAT_R16G16_SFLOAT));
			dispatch(KILN_BRDF_LUT, Size2i(8, 8), { U(), I(1, T("rtdgi_fg")) });
		}
		Size2i half_size = (state->size + Size2i(1, 1)) / 2;
		RID albedo = buffers->get_texture(SNAME("kiln_deferred"), SNAME("albedo_metallic"));
		RID ksky = state->textures["kiln_sky"];
		RID kalloc = state->textures["kiln_tile_alloc"];
		RID ktile_irr = state->textures["kiln_tile_irradiance"];
		dispatch(KILN_RTDGI_REPROJECT, Size2i((state->size.x + 7) / 8, (state->size.y + 7) / 8),
				{ U(), S(1, depth), S(2, normal), S(3, blue_noise), S(7, surface_input), S(11, T("rtdgi_geometry", previous)), S(12, buffers->get_texture(SNAME("kiln_deferred"), SNAME("motion_3d"))), I(13, T("rtdgi_geometry", current)), I(14, T("rtdgi_reprojection")) });
		if (!state->nrd) {
			ensure_legacy_filters();
		}
		dispatch(state->nrd ? KILN_NRD_REPROJECT : KILN_RTDGI_HISTORY_REPROJECT, Size2i((state->size.x + 7) / 8, (state->size.y + 7) / 8), { U(), C(10, state->nrd ? T("diffuse") : T("rtdgi_history", previous)), C(11, T("rtdgi_reprojection")), I(12, T("rtdgi_reprojected")) });
		const Size2i ao_half_groups((half_size.x + 7) / 8, (half_size.y + 7) / 8);
		const Size2i ao_full_groups((state->size.x + 7) / 8, (state->size.y + 7) / 8);
		dispatch(KILN_SSGI, ao_half_groups, { U(), S(1, depth), S(2, normal), S(6, albedo), I(15, T("ssgi_raw")) });
		dispatch(KILN_SSGI_SPATIAL, ao_half_groups, { U(), S(1, depth), S(2, normal), S(6, albedo), S(10, T("ssgi_raw")), I(13, T("ssgi_spatial")) });
		dispatch(KILN_SSGI_UPSAMPLE, ao_full_groups, { U(), S(2, normal), S(10, T("ssgi_spatial")), S(11, depth), I(13, T("ssgi_upsampled")) });
		dispatch(KILN_SSGI_TEMPORAL, ao_full_groups, { U(), S(10, T("ssgi_upsampled")), C(11, T("ssgi_history", previous)), S(12, T("rtdgi_reprojection")), I(13, T("ssgi_final")), I(14, T("ssgi_history", current)) });
		// Every clipmap surfel stage shares the binding contract of the include; bind
		// the full set plus the stage-specific resources on top.
		auto kbase = [&]() {
			return std::vector<Binding>{
				U(),
				B(40, "kiln_meta"), B(41, "kiln_pool"), B(42, "kiln_cell_offset"), B(43, "kiln_index"),
				B(44, "kiln_spatial"), B(45, "kiln_irradiance"), B(46, "kiln_aux"), B(47, "kiln_life"), B(48, "kiln_proposal")
			};
		};
		auto kappend = [&](std::vector<Binding> &list, std::initializer_list<Binding> extra) {
			list.insert(list.end(), extra.begin(), extra.end());
		};
		{
			std::vector<Binding> b = kbase();
			kappend(b, { I(1, ksky) });
			dispatch(KILN_SKY, Size2i(4, 4), b, 0, 6);
		}
		{
			std::vector<Binding> b = kbase();
			kappend(b, { S(10, depth), S(11, surface_input), I(12, kalloc), I(13, ktile_irr) });
			dispatch(KILN_SURFEL_FIND_MISSING, Size2i((state->size.x + 7) / 8, (state->size.y + 7) / 8), b);
		}
		{
			std::vector<Binding> b = kbase();
			kappend(b, { B(10, "kiln_args") });
			dispatch(KILN_SURFEL_ARGS, Size2i(1, 1), b);
		}
		dispatch(KILN_SURFEL_AGE, Size2i(kmax / 64, 1), kbase());
		{
			std::vector<Binding> b = kbase();
			kappend(b, { S(10, depth), S(11, surface_input), I(12, kalloc), I(13, ktile_irr) });
			b.push_back(S(14, normal));
			dispatch(KILN_SURFEL_ALLOCATE, Size2i((state->size.x + 63) / 64, (state->size.y + 63) / 64), b);
		}
		dispatch(KILN_SURFEL_CLEAR_CELLS, Size2i((kmax + 256) / 256, 1), kbase());
		std::vector<Binding> assignment_bindings = kbase();
		kappend(assignment_bindings, { B(10, "kiln_args") });
		dispatch(KILN_SURFEL_COUNT_CELLS, Size2i(kmax / 64, 1), assignment_bindings);
		{
			std::vector<Binding> b = kbase();
			kappend(b, { B(10, "kiln_cell_offset"), B(11, "kiln_scan_segments") });
			dispatch(KILN_SURFEL_SCAN, Size2i(1024, 1), b);
			dispatch(KILN_SURFEL_SCAN_SEGMENTS, Size2i(1, 1), b);
			dispatch(KILN_SURFEL_SCAN_MERGE, Size2i(1024, 1), b);
		}
		dispatch(KILN_SURFEL_SLOT_CELLS, Size2i(kmax / 64, 1), assignment_bindings);
		{
			std::vector<Binding> b = kbase();
			kappend(b, { B(4, "nodes"), B(5, "triangles"), B(16, "dynamic_nodes"), B(17, "dynamic_triangles"), S(32, state->ray_albedo, true), B(33, "texture_coordinates"), B(34, "dynamic_texture_coordinates"), S(19, ksky, true), S(20, T("rtdgi_fg"), true), I(21, T("wrc_atlas")) });
			dispatch(KILN_WRC_TRACE, Size2i(192 * 1024 / 64, 1), b, 0, 1, query_tlas);
		}
		{
			std::vector<Binding> b = kbase();
			kappend(b, { B(4, "nodes"), B(5, "triangles"), B(16, "dynamic_nodes"), B(17, "dynamic_triangles"), S(32, state->ray_albedo, true), B(33, "texture_coordinates"), B(34, "dynamic_texture_coordinates"), S(19, ksky, true), S(20, T("rtdgi_fg"), true) });
			dispatch(KILN_SURFEL_TRACE, Size2i(kmax / 64, 1), b, 0, 1, query_tlas);
		}
		{
			std::vector<Binding> b = kbase();
			Size2i half_size = (state->size + Size2i(1, 1)) / 2;
			kappend(b, { S(1, depth), S(2, normal), S(3, blue_noise), S(19, ksky, true), S(20, T("rtdgi_fg"), true), I(21, T("restir_candidate")), I(22, T("restir_candidate_hit")), C(50, T("wrc_atlas")), S(10, T("rtdgi_reprojected")), S(11, T("rtdgi_reprojection")), S(6, T("restir_irradiance", previous)), S(7, T("restir_origin", previous)), S(8, T("restir_hit", previous)), I(9, T("restir_invalidity")), B(4, "nodes"), B(5, "triangles"), B(16, "dynamic_nodes"), B(17, "dynamic_triangles"), S(32, state->ray_albedo, true), B(33, "texture_coordinates"), B(34, "dynamic_texture_coordinates") });
			dispatch(KILN_RESTIR_TRACE, Size2i((half_size.x + 7) / 8, (half_size.y + 7) / 8), b, 0, 1, query_tlas);
		}
		Size2i diffuse_half_groups((half_size.x + 7) / 8, (half_size.y + 7) / 8);
		Size2i diffuse_full_groups((state->size.x + 7) / 8, (state->size.y + 7) / 8);
		dispatch(KILN_RTDGI_VALIDITY, diffuse_half_groups,
				{ U(), S(1, depth), S(10, T("restir_invalidity")), S(11, T("restir_validity", previous)), S(12, T("rtdgi_reprojection")), I(15, T("restir_validity", current)) });
		dispatch(KILN_RESTIR_TEMPORAL, diffuse_half_groups,
				{ U(), S(2, normal), S(3, blue_noise), S(6, albedo), S(11, depth), S(12, T("restir_candidate")), S(13, T("restir_candidate_hit")),
						S(14, T("restir_irradiance", previous)), S(15, T("restir_origin", previous)), S(16, T("restir_hit", previous)), S(17, T("restir_reservoir", previous)), S(18, T("rtdgi_reprojection")), S(19, T("restir_hit_normal", previous)), S(20, T("restir_candidate_history", previous)), S(21, T("restir_validity", current)),
						I(22, T("restir_irradiance", current)), I(23, T("restir_origin", current)), I(24, T("restir_hit", current)), I(25, T("restir_hit_normal", current)), I(26, T("restir_reservoir", current)), I(27, T("restir_candidate_history", current)) });
		for (int pass = 0; pass < 2; pass++) {
			dispatch(KILN_RESTIR_SPATIAL, diffuse_half_groups,
					{ U(), S(1, depth), S(2, normal), S(6, albedo), S(10, T("restir_irradiance", current)), S(11, T("restir_hit_normal", current)), S(12, T("restir_hit", current)), S(13, pass == 0 ? T("restir_reservoir", current) : T("restir_spatial", 0)), S(17, T("ssgi_final")), I(19, T("restir_spatial", pass)) }, pass);
		}
		dispatch(KILN_RESTIR_RESOLVE, diffuse_full_groups,
				{ U(), S(1, depth), S(2, normal), S(3, blue_noise), S(6, albedo), S(10, T("restir_irradiance", current)), S(12, T("restir_hit", current)), S(13, T("restir_spatial", 1)), S(15, depth), I(21, T("rtdgi_raw")) });
		auto legacy_diffuse = [&]() {
			ensure_legacy_filters();
			dispatch(KILN_RTDGI_TEMPORAL_FILTER, diffuse_full_groups,
					{ U(), S(10, T("rtdgi_raw")), S(11, T("rtdgi_reprojected")), C(12, T("rtdgi_moments", previous)), S(13, T("rtdgi_reprojection")), S(14, T("restir_validity", current)), I(15, T("rtdgi_filtered")), I(16, T("rtdgi_history", current)), I(17, T("rtdgi_moments", current)) });
			dispatch(KILN_RTDGI_SPATIAL_FILTER, diffuse_full_groups,
					{ U(), S(7, surface_input), S(10, T("rtdgi_filtered")), S(11, depth), S(12, T("ssgi_final")), I(14, T("diffuse")) });
		};
		if (!state->nrd) {
			legacy_diffuse();
		}
		Size2i full_groups((state->size.x + 7) / 8, (state->size.y + 7) / 8);
		Size2i half_groups((half_size.x + 7) / 8, (half_size.y + 7) / 8);
		{
			// Current resolved diffuse supplies reflection hits; NRD filters the resulting
			// specular signal together with diffuse once, after RTR resolve.
			std::vector<Binding> b = kbase();
			kappend(b, { S(1, depth), S(2, normal), S(3, blue_noise), S(6, albedo), S(7, state->nrd ? T("rtdgi_raw") : T("diffuse")), C(8, T("rtdgi_fg")), C(19, ksky), { 20, RD::UNIFORM_TYPE_STORAGE_BUFFER, rtr_noise }, I(21, T("rtr_candidate0")), I(22, T("rtr_candidate1")), I(23, T("rtr_candidate2")), B(4, "nodes"), B(5, "triangles"), B(16, "dynamic_nodes"), B(17, "dynamic_triangles"), S(32, state->ray_albedo, true), B(33, "texture_coordinates"), B(34, "dynamic_texture_coordinates") });
			dispatch(KILN_RTR_TRACE, half_groups, b, 0, 1, query_tlas);
		}
		dispatch(KILN_RTR_TEMPORAL, half_groups,
				{ U(), S(2, normal), S(6, albedo), S(12, depth), S(13, T("rtr_candidate0")), S(14, T("rtr_candidate1")), S(15, T("rtr_candidate2")),
						S(16, T("rtr_irradiance", previous)), S(17, T("rtr_origin", previous)), S(18, T("rtr_hit", previous)), S(19, T("rtr_reservoir", previous)), S(20, T("rtdgi_reprojection")), S(21, T("rtr_hit_normal", previous)),
						I(22, T("rtr_irradiance", current)), I(23, T("rtr_origin", current)), I(24, T("rtr_hit", current)), I(25, T("rtr_hit_normal", current)), I(26, T("rtr_reservoir", current)) });
		dispatch(KILN_RTR_RESOLVE, full_groups,
				{ U(), S(1, depth), S(2, normal), S(3, blue_noise), S(6, albedo), C(8, T("rtdgi_fg")), S(11, depth), S(13, T("rtr_candidate1")), S(14, T("rtr_candidate2")), S(16, T("rtdgi_reprojection")), C(19, T("rtr_length", previous)),
						S(20, T("rtr_irradiance", current)), S(21, T("rtr_hit", current)), S(22, T("rtr_reservoir", current)), S(23, T("rtr_origin", current)), I(24, T("rtr_resolved")), I(25, T("rtr_length", current)) });
		state->nrd_active = false;
		if (state->nrd) {
			dispatch(KILN_NRD_PREPARE, full_groups,
					{ U(), S(1, depth), S(2, normal), S(3, buffers->get_texture(SNAME("kiln_deferred"), SNAME("motion_3d"))), S(4, T("rtdgi_raw")), S(5, T("rtr_resolved")), S(6, T("rtr_length", current)),
							I(7, T("nrd_normal")), I(8, T("nrd_depth")), I(9, T("nrd_motion")), I(10, T("nrd_diffuse")), I(11, T("nrd_specular")) });
			RENDER_TIMESTAMP("Kiln / NRD RELAX diffuse + specular");
			state->nrd_active = state->nrd->denoise(scene, state->previous_projection, state->previous_camera, state->previous_jitter, state->frames, state->frames == 0,
					T("nrd_motion"), T("nrd_normal"), T("nrd_depth"), T("nrd_diffuse"), T("nrd_specular"), T("diffuse"), T("rtr_final"));
			RENDER_TIMESTAMP("Kiln / between passes");
			if (!state->nrd_active) {
				WARN_PRINT_ONCE("Kiln NRD dispatch failed; using legacy GI filters.");
				legacy_diffuse();
				memdelete(state->nrd);
				state->nrd = nullptr;
			}
		}
		if (!state->nrd_active) {
			// The retained RTR temporal_filter.hlsl is an unconditional copy.
			dispatch(KILN_RTR_FILTER, full_groups,
					{ U(), S(10, T("rtr_resolved")), C(11, T("rtr_history", previous)), S(12, depth), S(13, T("rtr_length", current)), C(14, T("rtdgi_reprojection")), I(15, T("rtr_history", current)) });
			dispatch(KILN_RTR_CLEANUP, full_groups,
					{ U(), S(7, surface_input), S(10, T("rtr_history", current)), S(11, depth), I(13, T("rtr_final")) });
		}
		dispatch(KILN_SHADOW_TRACE, full_groups,
				{ U(), S(1, depth), S(2, normal), S(3, blue_noise), S(6, surface_input), I(10, T("shadow_raw")),
						B(4, "nodes"), B(5, "triangles"), B(16, "dynamic_nodes"), B(17, "dynamic_triangles"), S(32, state->ray_albedo, true), B(33, "texture_coordinates"), B(34, "dynamic_texture_coordinates") },
				0, 1, query_tlas);
		dispatch(KILN_SHADOW_BITPACK, Size2i((state->size.x + 31) / 32, (state->size.y + 15) / 16), { U(), S(10, T("shadow_raw")), I(11, T("shadow_bitpack")) });
		dispatch(KILN_SHADOW_TEMPORAL, full_groups,
				{ U(), S(10, T("shadow_raw")), S(11, T("shadow_bitpack")), C(12, T("shadow_moments", previous)), C(13, T("shadow_accum", previous)), S(14, T("rtdgi_reprojection")), I(15, T("shadow_moments", current)), I(16, T("shadow_spatial")), I(17, T("shadow_metadata")) });
		for (int pass = 0; pass < 3; pass++) {
			RID input = pass == 0 ? T("shadow_spatial") : (pass == 1 ? T("shadow_accum", current) : T("shadow_temp"));
			RID output = pass == 0 ? T("shadow_accum", current) : (pass == 1 ? T("shadow_temp") : T("shadow_spatial"));
			dispatch(KILN_SHADOW_SPATIAL, full_groups, { U(), S(7, surface_input), S(10, input), S(11, T("shadow_metadata")), S(13, depth), I(14, output) }, 1 << pass);
		}
		dispatch(KILN_LIGHT, Size2i((state->size.x + 7) / 8, (state->size.y + 7) / 8),
				{ U(), S(1, depth), S(2, normal), S(3, blue_noise), S(6, buffers->get_texture(SNAME("kiln_deferred"), SNAME("albedo_metallic"))), S(7, buffers->get_texture(SNAME("kiln_deferred"), SNAME("emission"))), S(8, T("diffuse")), I(9, T("rtdgi_lighting")), S(10, T("rtr_final")), S(11, T("shadow_spatial")), S(19, ksky, true), S(20, T("rtdgi_fg"), true),
						B(4, "nodes"), B(5, "triangles"), B(16, "dynamic_nodes"), B(17, "dynamic_triangles"), S(32, state->ray_albedo, true), B(33, "texture_coordinates"), B(34, "dynamic_texture_coordinates") },
				0, 1, query_tlas);
	}
	// Explicit diagnostics only: this synchronous readback is never used for timing.
	if (world.capture_request != state->capture_request && !world.capture_directory.is_empty()) {
		state->capture_request = world.capture_request;
		if (DirAccess::make_dir_recursive_absolute(world.capture_directory) == OK) {
			{
				Dictionary metadata;
				metadata["width"] = state->size.x;
				metadata["height"] = state->size.y;
				metadata["frame"] = state->frames;
				metadata["nrd_active"] = state->nrd_active;
				metadata["gi_algorithm"] = "Surfel GI + RTDGI ReSTIR";
				metadata["hdr_format"] = "RGBA16F linear scene radiance before display transform";
				const bool hdr_only = ProjectSettings::get_singleton()->get_setting("rendering/kiln/capture_hdr_only", false);
				const bool indirect_only = ProjectSettings::get_singleton()->get_setting("rendering/kiln/capture_indirect_only", false);
				metadata["capture_hdr_only"] = hdr_only;
				Dictionary signals;
				auto capture_signal = [&](const char *name, RID texture_rid) {
					if (indirect_only && String(name) != "diffuse" && String(name) != "rtr_final" && String(name) != "rtdgi_lighting" && String(name) != "surface") {
						return;
					}
					Ref<FileAccess> file = FileAccess::open(world.capture_directory.path_join(String(name) + ".bin"), FileAccess::WRITE);
					if (file.is_null()) {
						return;
					}
					Vector<uint8_t> bytes = rd->texture_get_data(texture_rid, 0);
					file->store_buffer(bytes);
					RD::TextureFormat format = rd->texture_get_format(texture_rid);
					Dictionary signal;
					signal["width"] = format.width;
					signal["height"] = format.height;
					const char *format_name = "unknown";
					switch (format.format) {
						case RD::DATA_FORMAT_R8_UNORM:
							format_name = "R8_UNORM";
							break;
						case RD::DATA_FORMAT_R8G8B8A8_UNORM:
							format_name = "R8G8B8A8_UNORM";
							break;
						case RD::DATA_FORMAT_R16_SFLOAT:
							format_name = "R16_SFLOAT";
							break;
						case RD::DATA_FORMAT_R16G16_SFLOAT:
							format_name = "R16G16_SFLOAT";
							break;
						case RD::DATA_FORMAT_R16G16B16A16_SFLOAT:
							format_name = "R16G16B16A16_SFLOAT";
							break;
						case RD::DATA_FORMAT_R32G32_UINT:
							format_name = "R32G32_UINT";
							break;
						case RD::DATA_FORMAT_R32G32B32A32_SFLOAT:
							format_name = "R32G32B32A32_SFLOAT";
							break;
						default:
							break;
					}
					signal["format"] = format_name;
					signal["format_enum"] = format.format;
					signal["byte_length"] = bytes.size();
					signals[name] = signal;
				};
				for (const char *name : { "rtdgi_lighting", "diffuse", "rtdgi_raw", "rtdgi_filtered", "shadow_raw", "shadow_spatial", "ssgi_final", "ssgi_raw", "rtr_final", "rtr_resolved", "rtr_candidate0", "restir_candidate", "restir_candidate_hit", "restir_invalidity" }) {
					if (hdr_only && String(name) != "rtdgi_lighting" && String(name) != "surface") {
						continue;
					}
					if (state->textures.has(String(name) + "0")) {
						capture_signal(name, T(name));
					}
				}
				for (const char *name : { "restir_irradiance", "restir_hit", "restir_origin", "restir_hit_normal", "restir_reservoir" }) {
					if (hdr_only && String(name) != "rtdgi_lighting" && String(name) != "surface") {
						continue;
					}
					capture_signal(name, T(name, current));
				}
				for (const char *name : { "surface", "normal_roughness", "albedo_metallic", "emission", "motion_3d" }) {
					if (hdr_only && String(name) != "rtdgi_lighting" && String(name) != "surface") {
						continue;
					}
					capture_signal(name, buffers->get_texture(SNAME("kiln_deferred"), StringName(name)));
				}
				metadata["signals"] = signals;
				Ref<FileAccess> dump = FileAccess::open(world.capture_directory.path_join("metadata.json"), FileAccess::WRITE);
				if (dump.is_valid()) {
					dump->store_string(JSON::stringify(metadata, "\t"));
				}
			}
		}
	}
	Dictionary statistics;
	statistics["rendered_frames"] = state->frames + 1;
	statistics["width"] = state->size.x;
	statistics["height"] = state->size.y;
	statistics["gi_enabled"] = world.enabled;
	statistics["nrd_active"] = state->nrd_active;
	statistics["nrd_feedback"] = state->nrd_active ? "previous NRD diffuse, geometry-validated reprojection" : "legacy RTDGI history";
	statistics["legacy_diffuse_filter_active"] = !state->nrd_active;
	statistics["legacy_filter_texture_bytes"] = state->textures.has("rtdgi_filtered0") ? uint64_t(state->size.x) * state->size.y * 48 : uint64_t(0);
	statistics["indirect_denoiser"] = state->nrd_active ? "NRD 4.17.3 RELAX_DIFFUSE_SPECULAR" : "legacy temporal/spatial filters";
	statistics["backend"] = state->hardware_active ? "hardware_ray_query" : "compute_software_bvh";
	statistics["hardware_ray_query_available"] = hardware_available;
	statistics["hardware_blas_builds"] = state->hardware_builds;
	statistics["hardware_tlas_builds"] = state->tlas_builds;
	statistics["hardware_fallback"] = world.query_backend != 1 && !state->hardware_active;
	{
		statistics["gi_algorithm"] = "Surfel GI + RTDGI ReSTIR";
		statistics["surfel_capacity"] = 262144;
		statistics["surfel_grid"] = "clipmap";
		statistics["surfel_ray_budget"] = 0;
		statistics["surfel_ray_budget_adaptive"] = false;
		statistics["surfel_ray_budget_mode"] = "four rays per live surfel";
		statistics["specular_rays"] = 1;
		statistics["specular_resolution"] = "half resolution";
		statistics["specular_implementation"] = state->nrd_active ? "GGX VNDF, ReSTIR temporal reuse, resolve and NRD RELAX" : "GGX VNDF, ReSTIR temporal reuse, resolve and temporal/spatial denoise";
		statistics["specular_checkerboard"] = false;
		statistics["rtdgi_candidate_rays"] = 1;
		statistics["rtdgi_path_validation"] = true;
		statistics["rtdgi_spatial_passes"] = 2;
		statistics["ao_algorithm"] = "SSGI AO denoising guide";
		statistics["shadow_algorithm"] = "Sun disk rays with FidelityFX shadow denoising";
		statistics["display_algorithm"] = "TAA, motion blur, glare and display transform";
	}
	KilnWorld::report(environment, statistics);
	state->previous_camera = scene->cam_transform;
	state->previous_projection = scene->cam_projection;
	state->geometry_version = world.geometry_version;
	state->dynamic_version = world.dynamic_version;
	state->material_version = world.material_version;
	state->static_material_version = world.static_material_version;
	state->dynamic_material_version = world.dynamic_material_version;
	state->previous_jitter = scene->taa_jitter;
	state->frames++;
	state->index = previous;
	return true;
}

RID KilnGI::process_display(Ref<RenderSceneBuffersRD> buffers, RID color) {
	if (!buffers->has_custom_data(kiln_scope)) {
		return RID();
	}
	Ref<View> state = buffers->get_custom_data(kiln_scope);
	if (!state->tracing || state->frames == 0 || !state->textures.has("rtdgi_lighting0")) {
		return RID();
	}
	RD *rd = RD::get_singleton();
	auto T = [&](const String &name, int index = 0) { return state->t(name, index); };
	auto U = [&]() { return Binding{ 0, RD::UNIFORM_TYPE_UNIFORM_BUFFER, state->parameters }; };
	auto S = [&](int binding, RID tex) { return Binding{ binding, RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, tex }; };
	auto C = [&](int binding, RID tex) { return Binding{ binding, RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, tex, true, true }; };
	auto L = [&](int binding, RID tex) { return Binding{ binding, RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, tex, true, false }; };
	auto I = [&](int binding, RID tex) { return Binding{ binding, RD::UNIFORM_TYPE_IMAGE, tex }; };
	auto make = [&](const String &name, Size2i size, RD::DataFormat format) {
		RID tex = texture(size, format);
		state->owned.push_back(tex);
		state->textures[name] = tex;
	};
	Size2i size = state->size;
	Size2i groups((size.x + 7) / 8, (size.y + 7) / 8);
	Size2i half(MAX(1, size.x / 2), MAX(1, size.y / 2));
	int mip_count = MAX(1, int(Math::floor(Math::log(float(MAX(half.x, half.y))) / Math::log(2.0f))));
	if (!state->textures.has("post_final0")) {
		make("motion_reduced_x0", Size2i((size.x + 15) / 16, size.y), RD::DATA_FORMAT_R16G16_SFLOAT);
		for (const char *name : { "motion_reduced_y", "motion_dilated" }) {
			make(String(name) + "0", Size2i((size.x + 15) / 16, (size.y + 15) / 16), RD::DATA_FORMAT_R16G16_SFLOAT);
		}
		make("motion_output0", size, RD::DATA_FORMAT_R16G16B16A16_SFLOAT);
		for (const char *name : { "taa_reprojected", "taa_input", "taa_deviation", "taa_filtered_history", "taa_output" }) {
			make(String(name) + "0", size, RD::DATA_FORMAT_R16G16B16A16_SFLOAT);
		}
		for (const char *name : { "taa_probability", "taa_probability1", "taa_probability2" }) {
			make(String(name) + "0", size, RD::DATA_FORMAT_R16_SFLOAT);
		}
		make("taa_closest_velocity0", size, RD::DATA_FORMAT_R16G16_SFLOAT);
		for (int i = 0; i < 2; i++) {
			make("taa_history" + itos(i), size, RD::DATA_FORMAT_R16G16B16A16_SFLOAT);
			make("taa_variance" + itos(i), size, RD::DATA_FORMAT_R16G16B16A16_SFLOAT);
			make("taa_velocity" + itos(i), size, RD::DATA_FORMAT_R16G16_SFLOAT);
		}
		make("post_final0", size, RD::DATA_FORMAT_B10G11R11_UFLOAT_PACK32);
		make("post_lut0", Size2i(64, 1), RD::DATA_FORMAT_R16G16_SFLOAT);
		dispatch(KILN_DISPLAY_LUT, Size2i(1, 1), { I(10, T("post_lut")) });
		Size2i level_size = half;
		for (int level = 0; level < mip_count; level++) {
			make("post_blur" + itos(level) + "0", level_size, RD::DATA_FORMAT_B10G11R11_UFLOAT_PACK32);
			make("post_reverse" + itos(level) + "0", level_size, RD::DATA_FORMAT_B10G11R11_UFLOAT_PACK32);
			level_size = Size2i(MAX(1, level_size.x / 2), MAX(1, level_size.y / 2));
		}
	}
	const int previous = state->index, current = 1 - previous;
	RID depth = buffers->get_depth_texture();
	RID reprojection = T("rtdgi_reprojection");
	dispatch(KILN_TAA_REPROJECT, groups, { U(), C(10, T("taa_history", previous)), S(11, reprojection), S(12, depth), I(13, T("taa_reprojected")), I(14, T("taa_closest_velocity")) });
	dispatch(KILN_TAA_INPUT, groups, { U(), S(10, color), S(11, depth), I(12, T("taa_input")), I(13, T("taa_deviation")) });
	dispatch(KILN_TAA_HISTORY, groups, { U(), S(10, T("taa_reprojected")), I(11, T("taa_filtered_history")) });
	dispatch(KILN_TAA_PROB, groups, { U(), S(11, T("taa_input")), S(12, T("taa_deviation")), S(14, T("taa_filtered_history")), S(15, reprojection), C(17, T("taa_variance", previous)), C(18, T("taa_velocity", previous)), I(19, T("taa_probability")) });
	dispatch(KILN_TAA_PROB_FILTER, groups, { U(), S(10, T("taa_probability")), I(11, T("taa_probability1")) });
	dispatch(KILN_TAA_PROB_FILTER2, groups, { U(), S(10, T("taa_probability1")), I(11, T("taa_probability2")) });
	dispatch(KILN_TAA, groups, { U(), S(10, color), S(11, T("taa_reprojected")), S(12, reprojection), S(13, T("taa_closest_velocity")), C(14, T("taa_velocity", previous)), C(16, T("taa_variance", previous)), C(17, T("taa_probability2")), I(18, T("taa_history", current)), I(19, T("taa_output")), I(20, T("taa_variance", current)), I(21, T("taa_velocity", current)) });
	Size2i tile_size((size.x + 15) / 16, (size.y + 15) / 16);
	Size2i tile_groups((tile_size.x + 7) / 8, (tile_size.y + 7) / 8);
	dispatch(KILN_VELOCITY_REDUCE_X, Size2i((tile_size.x + 7) / 8, groups.y), { S(10, reprojection), I(11, T("motion_reduced_x")) });
	dispatch(KILN_VELOCITY_REDUCE_Y, tile_groups, { S(10, T("motion_reduced_x")), I(11, T("motion_reduced_y")) });
	dispatch(KILN_VELOCITY_DILATE, tile_groups, { S(10, T("motion_reduced_y")), I(11, T("motion_dilated")) });
	dispatch(KILN_MOTION_BLUR, groups, { U(), C(10, T("taa_output")), C(11, reprojection), S(12, T("motion_dilated")), S(13, depth), I(14, T("motion_output")) });
	Size2i level_size = half;
	for (int level = 0; level < mip_count; level++) {
		RID input = level == 0 ? T("motion_output") : T("post_blur" + itos(level - 1));
		dispatch(level == 0 ? KILN_POST_BLUR0 : KILN_POST_BLUR, Size2i((level_size.x + 63) / 64, level_size.y), { S(10, input), I(11, T("post_blur" + itos(level))) });
		level_size = Size2i(MAX(1, level_size.x / 2), MAX(1, level_size.y / 2));
	}
	// The reference leaves the bottom reverse level black and starts at n-2.
	for (int level = mip_count - 2; level >= 0; level--) {
		level_size = Size2i(MAX(1, half.x >> level), MAX(1, half.y >> level));
		dispatch(KILN_POST_REVERSE, Size2i((level_size.x + 7) / 8, (level_size.y + 7) / 8), { S(10, T("post_blur" + itos(level))), C(11, T("post_reverse" + itos(level + 1))), I(12, T("post_reverse" + itos(level))) });
	}
	dispatch(KILN_POST, groups, { U(), S(3, blue_noise), S(10, T("motion_output")), C(12, T("post_reverse0")), I(13, T("post_final")), L(60, T("post_lut")) });
	return T("post_final");
}
