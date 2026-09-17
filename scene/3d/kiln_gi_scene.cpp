// Copyright (c) Godot Engine contributors. See LICENSE.txt.
// Capture the original scene mesh arrays for ray tracing, without simplification.
#include "kiln_gi_world.h"
#ifdef RD_ENABLED
#include "core/object/callable_mp.h"
#include "servers/rendering/rendering_server.h"

void KilnGIWorld::watch(const Ref<Resource> &p_resource) {
	if (p_resource.is_null()) {
		return;
	}
	ObjectID id = p_resource->get_instance_id();
	used_resources.insert(id);
	if (watched_resources.has(id)) {
		return;
	}
	watched_resources.insert(id, p_resource);
	p_resource->connect_changed(callable_mp(this, &KilnGIWorld::resource_changed).bind(id));
}

KilnGIWorld::~KilnGIWorld() {
	for (const KeyValue<ObjectID, Ref<Resource>> &entry : watched_resources) {
		entry.value->disconnect_changed(callable_mp(this, &KilnGIWorld::resource_changed).bind(entry.key));
	}
}

void KilnGIWorld::resource_changed(ObjectID p_id) {
	// Signals invalidate cached data, including in-place ImageTexture updates and
	// edits to a shared material. Geometry is only regenerated for a mesh edit.
	mesh_cache.erase(uint64_t(p_id));
	texture_cache.erase(p_id);
	if (texture_pages.has(p_id)) {
		dirty_textures.insert(p_id);
	}
	material_cache.clear();
	shader_defaults.clear();
}

int KilnGIWorld::capture_texture(const Ref<Texture2D> &p_texture) {
	if (p_texture.is_null()) {
		return -1;
	}
	ObjectID id = p_texture->get_instance_id();
	watch(p_texture);
	if (texture_pages.has(id) && !dirty_textures.has(id)) {
		return texture_pages[id];
	}
	Ref<Image> image = p_texture->get_image();
	if (image.is_null() || image->is_empty()) {
		return -1;
	}
	image = image->duplicate();
	if (image->is_compressed() && image->decompress() != OK) {
		return -1;
	}
	image->convert(Image::FORMAT_RGBA8);
	image->resize(512, 512, Image::INTERPOLATE_BILINEAR);
	constexpr int page_bytes = 512 * 512 * 4;
	int page;
	if (texture_pages.has(id)) {
		page = texture_pages[id];
	} else if (!free_texture_pages.is_empty()) {
		page = free_texture_pages[free_texture_pages.size() - 1];
		free_texture_pages.resize(free_texture_pages.size() - 1);
	} else {
		page = snapshot.texture_pixels.size() / page_bytes;
		snapshot.texture_pixels.resize((page + 1) * page_bytes);
	}
	PackedByteArray bytes = image->get_data();
	memcpy(snapshot.texture_pixels.ptrw() + page * page_bytes, bytes.ptr(), page_bytes);
	texture_pages.insert(id, page);
	dirty_textures.erase(id);
	snapshot.texture_version++;
	return page;
}

Color KilnGIWorld::texture_average(const Ref<Texture2D> &p_texture) {
	if (p_texture.is_null()) {
		return Color(1, 1, 1, 1);
	}
	ObjectID id = p_texture->get_instance_id();
	watch(p_texture);
	if (texture_cache.has(id)) {
		return texture_cache[id];
	}
	Color average(0, 0, 0, 0);
	Ref<Image> image = p_texture->get_image();
	if (image.is_valid() && !image->is_empty()) {
		image = image->duplicate();
		if (image->is_compressed() && image->decompress() != OK) {
			WARN_PRINT("Kiln GI cannot decompress a scene material texture: " + p_texture->get_path());
			texture_cache.insert(id, Color(1, 1, 1, 1));
			return Color(1, 1, 1, 1);
		}
		// Convert EACH sample to linear before reducing (averaging sRGB first
		// significantly darkens high-contrast textures). No per-frame readback.
		int width = MIN(64, image->get_width()), height = MIN(64, image->get_height());
		float weight = 0;
		for (int y = 0; y < height; y++) {
			for (int x = 0; x < width; x++) {
				Color c = image->get_pixel(MIN(image->get_width() - 1, int((x + 0.5) * image->get_width() / width)), MIN(image->get_height() - 1, int((y + 0.5) * image->get_height() / height))).srgb_to_linear();
				average += Color(c.r, c.g, c.b, 1) * c.a;
				weight += c.a;
			}
		}
		average = weight > 0 ? average / weight : Color(0, 0, 0, 0);
	} else {
		average = Color(1, 1, 1, 1);
	}
	texture_cache.insert(id, average);
	return average;
}

KilnGIWorld::Transport KilnGIWorld::transport(const Ref<Material> &p_material) {
	Transport result;
	if (p_material.is_null()) {
		result.supported = true;
		return result;
	}
	ObjectID id = p_material->get_instance_id();
	watch(p_material);
	Ref<BaseMaterial3D> base = p_material;
	Ref<ShaderMaterial> custom = p_material;
	float metal = 0;
	if (base.is_valid()) {
		result.supported = base->get_transparency() == BaseMaterial3D::TRANSPARENCY_DISABLED;
		result.texture_page = capture_texture(base->get_texture(BaseMaterial3D::TEXTURE_ALBEDO));
		Color fallback = texture_average(base->get_texture(BaseMaterial3D::TEXTURE_ALBEDO));
		result.texture_fallback = Vector3(fallback.r, fallback.g, fallback.b);
		Color c = base->get_albedo().srgb_to_linear();
		if (result.texture_page < 0) {
			c *= texture_average(base->get_texture(BaseMaterial3D::TEXTURE_ALBEDO));
		}
		result.albedo = Vector3(c.r, c.g, c.b);
		metal = base->get_metallic();
		result.uv_scale = Vector2(base->get_uv1_scale().x, base->get_uv1_scale().y);
		result.uv_offset = Vector2(base->get_uv1_offset().x, base->get_uv1_offset().y);
		result.texture_repeat = base->get_flag(BaseMaterial3D::FLAG_USE_TEXTURE_REPEAT);
		result.vertex_color = base->get_flag(BaseMaterial3D::FLAG_ALBEDO_FROM_VERTEX_COLOR);
		if (base->get_feature(BaseMaterial3D::FEATURE_EMISSION)) {
			Color e = base->get_emission().srgb_to_linear();
			Color tex = texture_average(base->get_texture(BaseMaterial3D::TEXTURE_EMISSION));
			if (base->get_texture(BaseMaterial3D::TEXTURE_EMISSION).is_valid()) {
				e = base->get_emission_operator() == BaseMaterial3D::EMISSION_OP_ADD ? e + tex : e * tex;
			}
			e *= base->get_emission_energy_multiplier();
			result.emission = Vector3(e.r, e.g, e.b);
		}
	} else if (custom.is_valid() && custom->get_shader().is_valid()) {
		Ref<Shader> shader = custom->get_shader();
		watch(shader);
		if (!shader_defaults.has(shader->get_instance_id())) {
			Dictionary defaults;
			for (const char *name : { "kiln_uniform_transport", "tint_linear", "authored_emission", "metalness", "textured", "albedo_texture", "uv1_scale", "uv1_offset", "uv_scale", "world_mapping", "tile_metres", "vertex_color_albedo", "emission_textured", "emission_texture" }) {
				defaults[name] = RenderingServer::get_singleton()->shader_get_parameter_default(shader->get_rid(), name);
			}
			shader_defaults.insert(shader->get_instance_id(), defaults);
		}
		auto parameter = [&](const StringName &p_name) {
			Variant value = custom->get_shader_parameter(p_name);
			return value.get_type() == Variant::NIL ? shader_defaults[shader->get_instance_id()][p_name] : value;
		};
		Variant color = parameter(SNAME("tint_linear")), emission = parameter(SNAME("authored_emission"));
		result.supported = bool(parameter(SNAME("kiln_uniform_transport"))) && color.get_type() == Variant::VECTOR3 && emission.get_type() == Variant::VECTOR3;
		if (result.supported) {
			result.albedo = color;
			result.emission = emission;
			metal = float(parameter(SNAME("metalness")));
			if (bool(parameter(SNAME("textured")))) {
				result.texture_page = capture_texture(parameter(SNAME("albedo_texture")));
				Color fallback = texture_average(parameter(SNAME("albedo_texture")));
				result.texture_fallback = Vector3(fallback.r, fallback.g, fallback.b);
			}
			Variant scale = parameter(SNAME("uv1_scale")), offset = parameter(SNAME("uv1_offset"));
			if (scale.get_type() == Variant::VECTOR3) {
				Vector3 v = scale;
				result.uv_scale = Vector2(v.x, v.y);
			}
			if (offset.get_type() == Variant::VECTOR3) {
				Vector3 v = offset;
				result.uv_offset = Vector2(v.x, v.y);
			}
			Variant uv_scale = parameter(SNAME("uv_scale"));
			if (uv_scale.get_type() != Variant::NIL && !bool(parameter(SNAME("world_mapping")))) {
				result.uv_scale *= float(uv_scale);
			}
			result.world_mapping = bool(parameter(SNAME("world_mapping")));
			if (result.world_mapping) {
				result.uv_scale /= MAX(float(parameter(SNAME("tile_metres"))), 0.001f);
			}
			result.vertex_color = bool(parameter(SNAME("vertex_color_albedo")));
			if (bool(parameter(SNAME("emission_textured")))) {
				Color e = texture_average(parameter(SNAME("emission_texture")));
				result.emission *= Vector3(e.r, e.g, e.b);
			}
		}
	}
	result.albedo = result.albedo.clamp(Vector3(), Vector3(1, 1, 1)) * (1.0f - CLAMP(metal, 0.0f, 1.0f));
	if (!result.supported && !unsupported_materials.has(id)) {
		unsupported_materials.insert(id);
		WARN_PRINT(vformat("Kiln GI omitted material '%s': blended/cutout materials and arbitrary shaders are not yet supported by ray-hit material evaluation. Opaque BaseMaterial3D and kiln_uniform_transport are supported.", p_material->get_path()));
	}
	const Transport *previous = material_cache.getptr(id);
	if (!previous || previous->albedo != result.albedo || previous->emission != result.emission || previous->supported != result.supported || previous->texture_page != result.texture_page) {
		material_updates++;
		material_cache.insert(id, result);
	}
	return result;
}

KilnGIWorld::MeshData KilnGIWorld::capture_mesh(const Ref<Mesh> &p_mesh, int p_surface) {
	watch(p_mesh);
	MeshData mesh_data;
	Array arrays = p_mesh->surface_get_arrays(p_surface);
	if (arrays.size() != Mesh::ARRAY_MAX) {
		return mesh_data;
	}
	mesh_data.positions = arrays[Mesh::ARRAY_VERTEX];
	mesh_data.normals = arrays[Mesh::ARRAY_NORMAL];
	mesh_data.indices = arrays[Mesh::ARRAY_INDEX];
	mesh_data.colors = arrays[Mesh::ARRAY_COLOR];
	mesh_data.uvs = arrays[Mesh::ARRAY_TEX_UV];
	mesh_data.source_triangles = (mesh_data.indices.is_empty() ? mesh_data.positions.size() : mesh_data.indices.size()) / 3;
	mesh_data.revision = ++mesh_uploads;
	return mesh_data;
}
#endif
