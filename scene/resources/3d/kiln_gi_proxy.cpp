// Copyright (c) Godot Engine contributors. See LICENSE.txt.
#include "kiln_gi_proxy.h"
#ifdef RD_ENABLED
#include "core/config/project_settings.h"
#include "core/object/callable_mp.h"
#include "core/object/class_db.h"
#include "scene/resources/surface_tool.h"
#include "scene/resources/texture.h"

Dictionary KilnGIProxy::simplify_surface(const Ref<Mesh> &p_mesh, int p_surface, float p_ratio, float p_error) {
	Array arrays = p_mesh->surface_get_arrays(p_surface);
	struct Surface {
		PackedVector3Array positions, normals;
		PackedInt32Array indices;
		Vector3 vertex_color = Vector3(1, 1, 1);
		int source_triangles = 0;
	} proxy;
	proxy.positions = arrays[Mesh::ARRAY_VERTEX];
	proxy.normals = arrays[Mesh::ARRAY_NORMAL];
	proxy.indices = arrays[Mesh::ARRAY_INDEX];
	PackedColorArray colors = arrays[Mesh::ARRAY_COLOR];
	if (!colors.is_empty()) {
		proxy.vertex_color = Vector3();
		for (const Color &c : colors) {
			proxy.vertex_color += Vector3(c.r, c.g, c.b);
		}
		proxy.vertex_color /= colors.size();
	}
	if (proxy.indices.is_empty()) {
		proxy.indices.resize(proxy.positions.size());
		for (int i = 0; i < proxy.indices.size(); i++) {
			proxy.indices.set(i, i);
		}
	}
	proxy.source_triangles = proxy.indices.size() / 3;
	if (SurfaceTool::simplify_func && p_ratio < 1.0f && proxy.source_triangles > 12) {
		// Weld shading/UV seams within this material. GI uses a constant color,
		// so these are not geometric boundaries. Keep actual open boundaries:
		// boxes/hulls would fill the windows, arches and alleys of an import.
		HashMap<Vector3, int> unique;
		PackedInt32Array remap;
		PackedFloat32Array positions;
		remap.resize(proxy.positions.size());
		for (int i = 0; i < proxy.positions.size(); i++) {
			Vector3 v = proxy.positions[i];
			if (!unique.has(v)) {
				unique.insert(v, i);
			}
			remap.set(i, unique[v]);
			positions.push_back(v.x);
			positions.push_back(v.y);
			positions.push_back(v.z);
		}
		PackedInt32Array indices = proxy.indices;
		for (int i = 0; i < indices.size(); i++) {
			ERR_FAIL_INDEX_V(indices[i], remap.size(), Dictionary());
			indices.set(i, remap[indices[i]]);
		}
		PackedInt32Array simplified;
		simplified.resize(indices.size());
		float error = 0;
		size_t count = SurfaceTool::simplify_func(reinterpret_cast<unsigned int *>(simplified.ptrw()), reinterpret_cast<const unsigned int *>(indices.ptr()), indices.size(), positions.ptr(), proxy.positions.size(), sizeof(float) * 3, MAX(12, int(proxy.source_triangles * p_ratio)) * 3, p_error, SurfaceTool::SIMPLIFY_LOCK_BORDER, &error);
		if (count >= 3 && count < size_t(indices.size())) {
			simplified.resize(count);
			proxy.indices = simplified;
		}
	}
	Dictionary result;
	result["fingerprint"] = int64_t(Variant(arrays).hash());
	result["source_triangles"] = proxy.source_triangles;
	result["vertex_color"] = proxy.vertex_color;
	// Compact vertices too; imported proxies do not retain the original mesh arrays.
	HashMap<int, int> compact;
	PackedVector3Array vertices, normals;
	PackedInt32Array indices;
	for (int original : proxy.indices) {
		ERR_FAIL_INDEX_V(original, proxy.positions.size(), Dictionary());
		if (!compact.has(original)) {
			compact.insert(original, vertices.size());
			vertices.push_back(proxy.positions[original]);
			if (proxy.normals.size() == proxy.positions.size()) {
				normals.push_back(proxy.normals[original]);
			}
		}
		indices.push_back(compact[original]);
	}
	result["positions"] = vertices;
	result["normals"] = normals;
	result["indices"] = indices;
	return result;
}

Color KilnGIProxy::average(const Ref<Texture2D> &p_texture) {
	if (p_texture.is_null()) {
		return Color(1, 1, 1, 1);
	}
	ObjectID id = p_texture->get_instance_id();
	observe(p_texture);
	if (averages.has(id)) {
		return averages[id];
	}
	Color average(0, 0, 0, 0);
	Ref<Image> image = p_texture->get_image();
	if (image.is_valid() && !image->is_empty()) {
		image = image->duplicate();
		if (image->is_compressed() && image->decompress() != OK) {
			WARN_PRINT("Kiln proxy cannot decompress transport texture: " + p_texture->get_path());
			averages.insert(id, Color(1, 1, 1, 1));
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
	averages.insert(id, average);
	return average;
}

void KilnGIProxy::observe(const Ref<Resource> &p_resource) {
	if (p_resource.is_null()) {
		return;
	}
	ObjectID id = p_resource->get_instance_id();
	used.insert(id);
	if (!watched.has(id)) {
		watched.insert(id, p_resource);
		p_resource->connect_changed(callable_mp(this, &KilnGIProxy::source_changed).bind(id));
	}
}

void KilnGIProxy::source_changed(ObjectID p_id) {
	averages.erase(p_id);
	refresh_materials();
}

KilnGIProxy::~KilnGIProxy() {
	for (const KeyValue<ObjectID, Ref<Resource>> &item : watched) {
		item.value->disconnect_changed(callable_mp(this, &KilnGIProxy::source_changed).bind(item.key));
	}
}

void KilnGIProxy::set_proxy_mesh(const Ref<ArrayMesh> &p_mesh) {
	proxy_mesh = p_mesh;
	refresh_materials();
}
void KilnGIProxy::set_source_materials(const TypedArray<Material> &p_materials) {
	source_materials = p_materials;
	refresh_materials();
}

void KilnGIProxy::replace_source_material(int p_surface, const Ref<Material> &p_material) {
	Array surfaces = data.get("surfaces", Array());
	if (p_surface < 0 || p_surface >= surfaces.size()) {
		return;
	} // Mesh topology may be undergoing reimport.
	Dictionary surface = surfaces[p_surface];
	int index = surface.get("proxy_surface", -1);
	if (index >= 0 && index < source_materials.size()) {
		source_materials[index] = p_material;
		refresh_materials();
	}
}

void KilnGIProxy::refresh_materials() {
	if (proxy_mesh.is_null() || source_materials.size() != proxy_mesh->get_surface_count()) {
		return;
	}
	used.clear();
	Array surfaces = data.get("surfaces", Array());
	for (int i = 0; i < source_materials.size(); i++) {
		Ref<Material> source = source_materials[i];
		observe(source);
		Ref<BaseMaterial3D> base = source;
		Color albedo(1, 1, 1), emission(0, 0, 0);
		bool supported = source.is_null();
		if (base.is_valid()) {
			supported = base->get_transparency() == BaseMaterial3D::TRANSPARENCY_DISABLED;
			albedo = base->get_albedo().srgb_to_linear() * average(base->get_texture(BaseMaterial3D::TEXTURE_ALBEDO));
			albedo *= 1.0f - CLAMP(base->get_metallic(), 0.0f, 1.0f);
			if (base->get_flag(BaseMaterial3D::FLAG_ALBEDO_FROM_VERTEX_COLOR)) {
				for (const Variant &entry : surfaces) {
					Dictionary surface = entry;
					if (int(surface.get("proxy_surface", -1)) == i) {
						Vector3 vertex = surface.get("vertex_color", Vector3(1, 1, 1));
						albedo *= Color(vertex.x, vertex.y, vertex.z);
						break;
					}
				}
			}
			if (base->get_feature(BaseMaterial3D::FEATURE_EMISSION)) {
				emission = base->get_emission().srgb_to_linear();
				Ref<Texture2D> texture = base->get_texture(BaseMaterial3D::TEXTURE_EMISSION);
				if (texture.is_valid()) {
					Color color = average(texture);
					emission = base->get_emission_operator() == BaseMaterial3D::EMISSION_OP_ADD ? emission + color : emission * color;
				}
				emission *= base->get_emission_energy_multiplier();
				if (GLOBAL_GET_CACHED(bool, "rendering/lights_and_shadows/use_physical_light_units")) {
					emission *= base->get_emission_intensity();
				}
			}
		}
		Ref<StandardMaterial3D> material = proxy_mesh->surface_get_material(i);
		if (material.is_null()) {
			material.instantiate();
			proxy_mesh->surface_set_material(i, material);
		}
		albedo.a = 1.0f;
		emission.a = 1.0f;
		material->set_albedo(albedo.linear_to_srgb());
		material->set_feature(BaseMaterial3D::FEATURE_EMISSION, emission.r > 0 || emission.g > 0 || emission.b > 0);
		material->set_emission(emission.linear_to_srgb());
		material->set_meta("kiln_transport_supported", supported);
	}
	Vector<ObjectID> removed;
	for (const KeyValue<ObjectID, Ref<Resource>> &item : watched) {
		if (!used.has(item.key)) {
			item.value->disconnect_changed(callable_mp(this, &KilnGIProxy::source_changed).bind(item.key));
			removed.push_back(item.key);
		}
	}
	for (ObjectID id : removed) {
		watched.erase(id);
		averages.erase(id);
	}
	material_revision++;
	emit_changed();
}

Ref<KilnGIProxy> KilnGIProxy::generate(const Ref<Mesh> &p_mesh, float p_ratio, float p_error) {
	ERR_FAIL_COND_V(p_mesh.is_null(), Ref<KilnGIProxy>());
	Ref<KilnGIProxy> result;
	result.instantiate();
	Ref<ArrayMesh> geometry;
	geometry.instantiate();
	geometry->set_name(p_mesh->get_name() + " GI proxy");
	Array surfaces;
	TypedArray<Material> materials;
	for (int i = 0; i < p_mesh->get_surface_count(); i++) {
		Dictionary surface;
		if (p_mesh->surface_get_primitive_type(i) == Mesh::PRIMITIVE_TRIANGLES && p_mesh->get_blend_shape_count() == 0 && !(p_mesh->surface_get_format(i) & Mesh::ARRAY_FORMAT_BONES)) {
			surface = simplify_surface(p_mesh, i, p_ratio, p_error);
			PackedVector3Array positions = surface.get("positions", PackedVector3Array());
			if (!positions.is_empty()) {
				Array arrays;
				arrays.resize(Mesh::ARRAY_MAX);
				arrays[Mesh::ARRAY_VERTEX] = positions;
				PackedVector3Array normals = surface["normals"];
				if (!normals.is_empty()) {
					arrays[Mesh::ARRAY_NORMAL] = normals;
				}
				arrays[Mesh::ARRAY_INDEX] = surface["indices"];
				surface["proxy_surface"] = geometry->get_surface_count();
				geometry->add_surface_from_arrays(Mesh::PRIMITIVE_TRIANGLES, arrays);
				materials.push_back(p_mesh->surface_get_material(i));
				surface.erase("positions");
				surface.erase("normals");
				surface.erase("indices");
			}
		}
		surfaces.push_back(surface);
	}
	result->data["format_version"] = FORMAT_VERSION;
	result->data["ratio"] = p_ratio;
	result->data["error"] = p_error;
	result->data["surfaces"] = surfaces;
	result->proxy_mesh = geometry;
	result->set_source_materials(materials);
	return result;
}

void KilnGIProxy::attach(const Ref<Mesh> &p_mesh) {
	if (p_mesh.is_null()) {
		return;
	}
	Ref<KilnGIProxy> cached = p_mesh->has_meta("kiln_gi_proxy") ? p_mesh->get_meta("kiln_gi_proxy") : Variant();
	bool valid = cached.is_valid();
	for (int i = 0; valid && i < p_mesh->get_surface_count(); i++) {
		valid = cached->matches(p_mesh, i, 0.12f, 0.001f);
	}
	if (!valid) {
		p_mesh->set_meta("kiln_gi_proxy", generate(p_mesh));
	}
}

bool KilnGIProxy::matches(const Ref<Mesh> &p_mesh, int p_surface, float p_ratio, float p_error) const {
	Array surfaces = data.get("surfaces", Array());
	if (int(data.get("format_version", 0)) != FORMAT_VERSION || !Math::is_equal_approx(float(data.get("ratio", 0.0)), p_ratio) || !Math::is_equal_approx(float(data.get("error", 0.0)), p_error) || surfaces.size() != p_mesh->get_surface_count() || proxy_mesh.is_null()) {
		return false;
	}
	Dictionary surface = surfaces[p_surface];
	int index = surface.get("proxy_surface", -1);
	return index >= 0 && index < proxy_mesh->get_surface_count() && int64_t(surface.get("fingerprint", -1)) == int64_t(Variant(p_mesh->surface_get_arrays(p_surface)).hash());
}

Dictionary KilnGIProxy::get_surface(int p_surface) const {
	Array surfaces = data.get("surfaces", Array());
	ERR_FAIL_INDEX_V(p_surface, surfaces.size(), Dictionary());
	Dictionary result = Dictionary(surfaces[p_surface]).duplicate();
	int index = result.get("proxy_surface", -1);
	ERR_FAIL_COND_V(proxy_mesh.is_null(), Dictionary());
	ERR_FAIL_INDEX_V(index, proxy_mesh->get_surface_count(), Dictionary());
	Array arrays = proxy_mesh->surface_get_arrays(index);
	result["positions"] = arrays[Mesh::ARRAY_VERTEX];
	result["normals"] = arrays[Mesh::ARRAY_NORMAL];
	result["indices"] = arrays[Mesh::ARRAY_INDEX];
	return result;
}

void KilnGIProxy::_bind_methods() {
	ClassDB::bind_method(D_METHOD("get_proxy_mesh"), &KilnGIProxy::get_proxy_mesh);
	ClassDB::bind_method(D_METHOD("set_proxy_mesh", "mesh"), &KilnGIProxy::set_proxy_mesh);
	ClassDB::bind_method(D_METHOD("get_source_materials"), &KilnGIProxy::get_source_materials);
	ClassDB::bind_method(D_METHOD("set_source_materials", "materials"), &KilnGIProxy::set_source_materials);
	ClassDB::bind_method(D_METHOD("get_data"), &KilnGIProxy::get_data);
	ClassDB::bind_method(D_METHOD("set_data", "data"), &KilnGIProxy::set_data);
	ClassDB::bind_method(D_METHOD("get_material_revision"), &KilnGIProxy::get_material_revision);
	ADD_PROPERTY(PropertyInfo(Variant::DICTIONARY, "data", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_STORAGE), "set_data", "get_data");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "proxy_mesh", PROPERTY_HINT_RESOURCE_TYPE, "ArrayMesh"), "set_proxy_mesh", "get_proxy_mesh");
	ADD_PROPERTY(PropertyInfo(Variant::ARRAY, "source_materials", PROPERTY_HINT_ARRAY_TYPE, "Material", PROPERTY_USAGE_STORAGE), "set_source_materials", "get_source_materials");
}
#endif
