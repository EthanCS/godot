// Kiln engine integration. Engine licensing: LICENSE.txt.
// Imported algorithm provenance and redistribution limits: kiln/docs/gi-provenance.json.

#include "kiln_gi.h"

#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/io/json.h"
#include "servers/rendering/renderer_rd/storage_rd/material_storage.h"
#include "servers/rendering/renderer_rd/uniform_set_cache_rd.h"

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
	const char *stages[] = { "PREPARE_RECEIVERS", "TRACE_PRIMARY", "INTEGRATE", "SH_TEMPORAL", "UPDATE_WORLD_CACHE", "SH_FILTER", "SH_DECODE", "BRDF_LUT", "SEQUENCE_LUT", "PUBLISH", "XEGTAO_DEPTH", "XEGTAO_MAIN", "XEGTAO_DENOISE", "XEGTAO_TEMPORAL" };
	for (int i = 0; i < STAGE_COUNT; i++) {
		defines.push_back(String("\n#define STAGE_") + stages[i] + "\n");
	}
	shader.initialize(defines);
	version = shader.version_create();
	shader.version_set_compute_code(version, HashMap<String, String>(), "", "", Vector<String>());
	for (int i = 0; i < STAGE_COUNT; i++) {
		pipelines[i] = RD::get_singleton()->compute_pipeline_create(shader.version_get_shader(version, i));
	}
	RD::SamplerState state;
	sampler = RD::get_singleton()->sampler_create(state);
	state.min_filter = state.mag_filter = RD::SAMPLER_FILTER_LINEAR;
	linear_sampler = RD::get_singleton()->sampler_create(state);
	brdf = texture(Size2i(64, 64), RD::DATA_FORMAT_R16G16_SFLOAT);
	sequence = texture(Size2i(128, 128), RD::DATA_FORMAT_R32G32B32A32_SFLOAT);
	hilbert = texture(Size2i(64, 64), RD::DATA_FORMAT_R16_UINT);
	dispatch(BRDF, Size2i(64, 64), { { 14, RD::UNIFORM_TYPE_IMAGE, brdf } });
	print_line(vformat("[KILN_GI] native software BVH; ray_query=%s raytracing_pipeline=%s (unused)", RD::get_singleton()->has_feature(RD::SUPPORTS_RAY_QUERY), RD::get_singleton()->has_feature(RD::SUPPORTS_RAYTRACING_PIPELINE)));
}
KilnGI::~KilnGI() {
	shader.version_free(version);
	for (RID rid : { sampler, linear_sampler, brdf, sequence, hilbert }) {
		RD::get_singleton()->free_rid(rid);
	}
}
void KilnGI::View::free_data() {
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
	geometry_version = dynamic_version = light_version = 0;
	ready = false;
}
void KilnGI::dispatch(Stage stage, Size2i size, std::initializer_list<Binding> bindings, int stride, int z) {
	RD *rd = RD::get_singleton();
	LocalVector<RD::Uniform> uniforms;
	for (const Binding &b : bindings) {
		RD::Uniform u;
		u.binding = b.binding;
		u.uniform_type = b.type;
		if (b.type == RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE) {
			u.append_id(b.linear ? linear_sampler : sampler);
		}
		u.append_id(b.resource);
		uniforms.push_back(u);
	}
	RID set = UniformSetCacheRD::get_singleton()->get_cache_vec(shader.version_get_shader(version, stage), 0, uniforms);
	RD::ComputeListID list = rd->compute_list_begin();
	rd->compute_list_bind_compute_pipeline(list, pipelines[stage]);
	rd->compute_list_bind_uniform_set(list, set, 0);
	if (stage == FILTER || stage == AO_DEPTH || stage == AO_DENOISE || stage == AO_TEMPORAL) {
		int push[4] = { stride, 0, 0, 0 };
		rd->compute_list_set_push_constant(list, push, stage == FILTER ? sizeof(push) : sizeof(int));
	}
	rd->compute_list_dispatch(list, (size.x + 7) / 8, (size.y + 7) / 8, z);
	rd->compute_list_end();
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
		state->half = (state->size + Size2i(1, 1)) / 2;
		state->parameters = own(rd->uniform_buffer_create(704));
		const char *names[] = { "position", "normal", "confidence", "age", "visibility", "sh", "raw", "visibility_raw", "sh_raw", "sh_filter_first", "sh_filter_work", "sh_filtered", "decoded", "diffuse", "specular", "display_diffuse", "display_specular", "publication_key" };
		for (String name : names) {
			RD::DataFormat format = RD::DATA_FORMAT_R16G16B16A16_SFLOAT;
			if (name == "position" || name == "sh" || name == "sh_raw" || name.begins_with("display_")) {
				format = RD::DATA_FORMAT_R32G32B32A32_SFLOAT;
			}
			if (name == "confidence" || name == "age" || name == "visibility" || name == "visibility_raw") {
				format = RD::DATA_FORMAT_R16_SFLOAT;
			}
			if (name == "publication_key") {
				format = RD::DATA_FORMAT_R32G32_UINT;
			}
			Size2i size = name.begins_with("sh") ? Size2i(state->half.x * 3, state->half.y) : state->half;
			if (name == "diffuse" || name == "specular" || name.begins_with("display_") || name == "publication_key") {
				size = state->size;
			}
			int count = name == "position" || name == "normal" || name == "confidence" || name == "age" || name == "visibility" || name == "sh" || name.begins_with("display_") ? 2 : 1;
			for (int i = 0; i < count; i++) {
				state->textures[name + itos(i)] = own(texture(size, format));
			}
		}
		while (state->slots < MIN(uint32_t(state->size.x * state->size.y), 1048576u)) {
			state->slots *= 2;
		}
		allocate("world_lights", state->slots * 96);
		allocate("world_owners", state->slots * 4);
		rd->buffer_clear(state->storage["world_lights"], 0, state->slots * 96);
		state->dirty_bytes = ((state->half.x + 15) / 16) * ((state->half.y + 15) / 16) * 4;
		allocate("dirty", state->dirty_bytes);
		allocate("receivers", state->half.x * state->half.y * 32);
		allocate("hits", state->half.x * state->half.y * MAX(4, world.rays) * 8);
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
	allocate("hits", state->half.x * state->half.y * MAX(4, world.rays) * 8);
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
	bool changed_world = state->geometry_version != world.geometry_version;
	bool changed_dynamic = state->dynamic_version != world.dynamic_version;
	bool changed_light = changed_world || changed_dynamic || state->light_version != world.light_version;
	if (changed_world) {
		upload("nodes", world.world.nodes);
		upload("triangles", world.world.triangles);
		state->frames = 0;
	}
	if (changed_dynamic) {
		upload("dynamic_nodes", world.dynamic.nodes);
		upload("dynamic_triangles", world.dynamic.triangles);
	}
	int emitter_count = world.world.emitters.size() + world.dynamic.emitters.size();
	if (changed_world || changed_dynamic) {
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
	if (changed_light) {
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
	if (state->epoch > 1048576) {
		state->epoch = 1;
		rd->buffer_clear(state->storage["world_lights"], 0, state->slots * 96);
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
	v(world.world.triangle_count, 1, 0, state->epoch);
	vec(world.sun_direction, world.sun_energy * Math::PI);
	vec(world.sun_color, world.sky_energy);
	vec(world.sky_horizon, 0);
	v(0, state->half.x, state->half.y, 1);
	v(0, 0, 0, 1);
	vec(world.sky_zenith, world.procedural_sky ? 2 : 1);
	vec(world.procedural_sky ? Vector3() : Vector3(0.554, 0.464, 0.304), world.procedural_sky ? 0.035 * world.sun_energy : 0);
	vec(world.sun_direction, world.time_of_day);
	vec(world.procedural_sky ? Vector3(0.78, 0.81, 0.85) : Vector3(), world.procedural_sky ? 0.4 : 0);
	v(1.12, 1, 0, 0);
	v(0, 0, 0, 1);
	v(world.world.node_count, world.world.triangle_count, world.enabled, state->lighting_remaining > 0);
	v(world.dynamic.node_count, world.dynamic.triangle_count, emitter_count, world.world.power + world.dynamic.power);
	MaterialStorage::store_camera(state->previous_vp.inverse(), params + at);
	at += 16;
	v(signed_normal, 0, 0, 0);
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
	RID depth = buffers->get_depth_texture(), normal = full_normal, pos = T("position", current), norm = T("normal", current);
	rd->buffer_clear(state->storage["dirty"], 0, state->dirty_bytes);
	if (!moving) {
		rd->buffer_clear(state->storage["world_owners"], 0, state->slots * 4);
	}
	if (world.enabled) {
		dispatch(PREPARE, state->half, { U(), S(1, depth), S(2, normal), S(11, T("position", previous)), S(12, T("normal", previous)), S(13, T("confidence", previous)), B(21, "receivers"), B(24, "dirty"), B(22, "world_lights"), B(23, "world_owners") });
		dispatch(TRACE, state->half, { U(), S(1, depth), S(2, normal), B(4, "nodes"), B(5, "triangles"), S(11, T("position", previous)), S(12, T("normal", previous)), S(13, T("confidence", previous)), B(16, "dynamic_nodes"), B(17, "dynamic_triangles"), B(18, "emitters"), B(25, "local_lights"), B(26, "light_grid"), S(19, sequence), B(20, "hits"), B(21, "receivers") }, 0, MAX(4, world.rays));
		dispatch(INTEGRATE, state->half, { U(), S(1, depth), S(2, normal), B(4, "nodes"), B(5, "triangles"), I(6, T("raw")), I(7, pos), I(8, norm), I(9, T("sh_raw")), I(10, T("visibility_raw")), S(11, T("position", previous)), S(12, T("normal", previous)), S(13, T("confidence", previous)), S(14, T("sh", previous)), S(15, T("visibility", previous)), B(16, "dynamic_nodes"), B(17, "dynamic_triangles"), B(18, "emitters"), B(25, "local_lights"), B(26, "light_grid"), S(19, sequence), B(20, "hits"), B(21, "receivers"), B(22, "world_lights"), B(23, "world_owners") });
		dispatch(TEMPORAL, state->half, { U(), S(1, T("sh_raw")), S(2, pos), S(3, norm), S(4, T("sh", previous)), S(5, T("position", previous)), S(6, T("normal", previous)), S(7, T("confidence", previous)), I(8, T("sh", current)), I(9, T("confidence", current)), S(10, T("visibility_raw")), S(11, T("visibility", previous)), I(12, T("visibility", current)), S(13, T("raw")), S(14, T("age", previous)), I(15, T("age", current)) });
		if (!moving) {
			dispatch(WORLD_CACHE, state->half, { U(), S(1, pos), S(2, norm), S(3, T("sh", current)), S(4, T("confidence", current)), S(5, T("visibility", current)), S(6, T("raw")), B(22, "world_lights"), B(23, "world_owners") });
		}
		RID filter_input = T("sh", current);
		const char *filter_names[] = { "sh_filter_first", "sh_filter_work", "sh_filtered" };
		for (int pass = 0; pass < 3; pass++) {
			RID output = T(filter_names[pass]);
			dispatch(FILTER, state->half, { U(), S(1, filter_input), S(2, pos), S(3, norm), S(4, T("visibility", current)), S(5, T("confidence", current)), I(6, output), B(7, "dirty") }, 1 << pass);
			filter_input = output;
		}
		dispatch(DECODE, state->half, { U(), S(1, T("sh_filtered")), S(2, norm), S(3, T("visibility", current)), I(4, T("decoded")), B(5, "dirty") });
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
	dispatch(PUBLISH, state->size, { U(), I(1, T("diffuse")), S(2, depth), S(3, full_normal), S(7, T("decoded")), S(8, pos), S(9, norm), S(10, T("confidence", current)), S(11, T("sh_filtered")), I(12, T("specular")), S(13, brdf, true), S(14, T("display_diffuse", previous)), I(15, T("display_diffuse", current)), S(16, T("display_specular", previous)), I(17, T("display_specular", current)), I(18, T("publication_key")), B(19, "dirty") });
	if (!world.enabled) {
		rd->texture_clear(T("diffuse"), Color(0, 0, 0, 0), 0, 1, 0, 1);
		rd->texture_clear(T("specular"), Color(0, 0, 0, 0), 0, 1, 0, 1);
	}
	// Explicit diagnostics only: this synchronous readback is never used for timing.
	if (world.capture_request != state->capture_request && !world.capture_directory.is_empty()) {
		state->capture_request = world.capture_request;
		if (DirAccess::make_dir_recursive_absolute(world.capture_directory) == OK) {
			Dictionary metadata;
			metadata["width"] = state->size.x;
			metadata["height"] = state->size.y;
			metadata["half_width"] = state->half.x;
			metadata["half_height"] = state->half.y;
			metadata["frame"] = state->frames;
			metadata["moving"] = moving;
			metadata["stationary_samples"] = state->stationary_samples;
			metadata["geometry_version"] = world.geometry_version;
			metadata["dynamic_version"] = world.dynamic_version;
			metadata["light_version"] = world.light_version;
			for (const char *name : { "diffuse", "specular", "ao", "confidence", "position", "normal", "sh", "decoded" }) {
				String key(name);
				RID rid = T(key, key == "diffuse" || key == "specular" || key == "ao" || key == "decoded" ? 0 : current);
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
	state->previous_vp = vp;
	state->previous_camera = scene->cam_transform;
	state->previous_projection = scene->cam_projection;
	state->geometry_version = world.geometry_version;
	state->dynamic_version = world.dynamic_version;
	state->light_version = world.light_version;
	state->frames++;
	state->index = previous;
	if (!moving) {
		state->stationary_samples = MIN(world.samples, state->stationary_samples + world.rays);
	}
	return true;
}
