// kajiya sky cube helpers: the reference renders a 32px RGBA16F cube per frame
// (rust-gpu sky.rs, atmosphere from the world origin) and every GI stage samples
// it. Ported as a 6-layer 2D array with the standard cube face mapping.
const int KAJIYA_SKY_DIMS = 32;

vec3 kajiya_cube_face_rotation(int face, vec3 v) {
	// Column-major rotations from kajiya rust-shaders-shared/src/util.rs.
	if (face == 0) return v * mat3(vec3(0.0, 0.0, -1.0), vec3(0.0, -1.0, 0.0), vec3(-1.0, 0.0, 0.0));
	if (face == 1) return v * mat3(vec3(0.0, 0.0, 1.0), vec3(0.0, -1.0, 0.0), vec3(1.0, 0.0, 0.0));
	if (face == 2) return v * mat3(vec3(1.0, 0.0, 0.0), vec3(0.0, 0.0, 1.0), vec3(0.0, -1.0, 0.0));
	if (face == 3) return v * mat3(vec3(1.0, 0.0, 0.0), vec3(0.0, 0.0, -1.0), vec3(0.0, 1.0, 0.0));
	if (face == 4) return v * mat3(vec3(1.0, 0.0, 0.0), vec3(0.0, -1.0, 0.0), vec3(0.0, 0.0, -1.0));
	return v * mat3(vec3(-1.0, 0.0, 0.0), vec3(0.0, -1.0, 0.0), vec3(0.0, 0.0, 1.0));
}

// Inverse of the face rotations above: standard OpenGL major-axis cube mapping.
void kajiya_dir_to_cube_face_uv(vec3 dir, out int face, out vec2 uv) {
	vec3 a = abs(dir);
	if (a.x >= a.y && a.x >= a.z) {
		if (dir.x >= 0.0) { face = 0; uv = vec2(-dir.z, -dir.y) / a.x; }
		else { face = 1; uv = vec2(dir.z, -dir.y) / a.x; }
	} else if (a.y >= a.z) {
		if (dir.y >= 0.0) { face = 2; uv = vec2(dir.x, dir.z) / a.y; }
		else { face = 3; uv = vec2(dir.x, -dir.z) / a.y; }
	} else {
		if (dir.z >= 0.0) { face = 4; uv = vec2(dir.x, -dir.y) / a.z; }
		else { face = 5; uv = vec2(-dir.x, -dir.y) / a.z; }
	}
	uv = uv * 0.5 + 0.5;
}

vec3 kajiya_sample_sky(sampler2DArray sky_tex, vec3 dir) {
	int face;
	vec2 uv;
	kajiya_dir_to_cube_face_uv(normalize(dir), face, uv);
	return textureLod(sky_tex, vec3(uv, float(face)), 0.0).rgb;
}
