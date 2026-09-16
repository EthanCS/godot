// Copyright (c) Godot Engine contributors. See LICENSE.txt.
// Kiln's diffuse transport proxy is independent of the visible mesh and its LODs.
#include "kiln_gi_world.h"
#ifdef RD_ENABLED
#include "core/object/callable_mp.h"
#include "scene/resources/3d/kiln_gi_proxy.h"
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
	material_cache.clear();
	shader_defaults.clear();
}

void KilnGIWorld::set_proxy_quality(float p_ratio, float p_error) {
	ERR_FAIL_COND_MSG(!Math::is_finite(p_ratio) || !Math::is_finite(p_error) || p_ratio < 0.01f || p_ratio > 1.0f || p_error < 0.0f || p_error > 0.05f, "Proxy ratio must be 0.01..1 and relative geometric error 0..0.05.");
	if (proxy_ratio == p_ratio && proxy_error == p_error) {
		return;
	}
	proxy_ratio = p_ratio;
	proxy_error = p_error;
	rebuild();
}

void KilnGIWorld::set_resolution_divisor(int p_divisor) {
	ERR_FAIL_COND_MSG(p_divisor != 2 && p_divisor != 4, "GI resolution divisor must be 2 (quality) or 4 (balanced).");
	if (snapshot.resolution_divisor != p_divisor) {
		snapshot.resolution_divisor = p_divisor;
		snapshot.history_version++;
	}
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
			WARN_PRINT("Kiln proxy cannot decompress transport texture: " + p_texture->get_path());
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
		Color c = base->get_albedo().srgb_to_linear() * texture_average(base->get_texture(BaseMaterial3D::TEXTURE_ALBEDO));
		result.albedo = Vector3(c.r, c.g, c.b);
		metal = base->get_metallic();
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
			for (const char *name : { "kiln_uniform_transport", "tint_linear", "authored_emission", "metalness" }) {
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
		}
	}
	result.albedo = result.albedo.clamp(Vector3(), Vector3(1, 1, 1)) * (1.0f - CLAMP(metal, 0.0f, 1.0f));
	if (!result.supported && !unsupported_materials.has(id)) {
		unsupported_materials.insert(id);
		WARN_PRINT(vformat("Kiln GI omitted material '%s': blended/cutout geometry and arbitrary shaders need an explicit transport proxy. Solid textured BaseMaterial3D and kiln_uniform_transport are supported.", p_material->get_path()));
	}
	const Transport *previous = material_cache.getptr(id);
	if (!previous || previous->albedo != result.albedo || previous->emission != result.emission || previous->supported != result.supported) {
		material_updates++;
		material_cache.insert(id, result);
	}
	return result;
}

KilnGIWorld::MeshData KilnGIWorld::make_proxy(const Ref<Mesh> &p_mesh, int p_surface) {
	watch(p_mesh);
	Dictionary surface;
	Ref<KilnGIProxy> imported = p_mesh->has_meta("kiln_gi_proxy") ? p_mesh->get_meta("kiln_gi_proxy") : Variant();
	if (imported.is_valid() && imported->matches(p_mesh, p_surface, proxy_ratio, proxy_error)) {
		surface = imported->get_surface(p_surface);
		imported_proxy_hits++;
	} else {
		surface = KilnGIProxy::simplify_surface(p_mesh, p_surface, proxy_ratio, proxy_error);
		runtime_proxy_builds++;
	}
	MeshData proxy;
	proxy.positions = surface.get("positions", PackedVector3Array());
	proxy.normals = surface.get("normals", PackedVector3Array());
	proxy.indices = surface.get("indices", PackedInt32Array());
	proxy.vertex_color = surface.get("vertex_color", Vector3(1, 1, 1));
	proxy.source_triangles = surface.get("source_triangles", 0);
	proxy.revision = ++proxy_builds;
	return proxy;
}
#endif
