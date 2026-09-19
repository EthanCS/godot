// lighting sky cube helpers: the reference renders a 32px RGBA16F cube per frame
// (rust-gpu sky.rs, atmosphere from the world origin) and every GI stage samples
// it. A storage cube view writes the six faces; cube sampling provides seamless
// hardware filtering across face boundaries, as in the reference.
const int KILN_SKY_DIMS = 32;

vec3 kiln_cube_face_rotation(int face, vec3 v) {
	// Column-major rotations from lighting rust-shaders-shared/src/util.rs.
	// The Rust reference multiplies CUBE_MAP_FACE_ROTATIONS * direction.
	// GLSL's mat3 constructors are column-major, so keep the matrices in the
	// same column form and use matrix-times-vector here.  The old row-vector
	// form transposed the top/bottom faces and made the sky rotate with the
	// wrong handedness.
	if (face == 0) return mat3(vec3(0.0, 0.0, -1.0), vec3(0.0, -1.0, 0.0), vec3(-1.0, 0.0, 0.0)) * v;
	if (face == 1) return mat3(vec3(0.0, 0.0, 1.0), vec3(0.0, -1.0, 0.0), vec3(1.0, 0.0, 0.0)) * v;
	if (face == 2) return mat3(vec3(1.0, 0.0, 0.0), vec3(0.0, 0.0, 1.0), vec3(0.0, -1.0, 0.0)) * v;
	if (face == 3) return mat3(vec3(1.0, 0.0, 0.0), vec3(0.0, 0.0, -1.0), vec3(0.0, 1.0, 0.0)) * v;
	if (face == 4) return mat3(vec3(1.0, 0.0, 0.0), vec3(0.0, -1.0, 0.0), vec3(0.0, 0.0, -1.0)) * v;
	return mat3(vec3(-1.0, 0.0, 0.0), vec3(0.0, -1.0, 0.0), vec3(0.0, 0.0, 1.0)) * v;
}

// Inverse of the face rotations above: standard OpenGL major-axis cube mapping.
void kiln_dir_to_cube_face_uv(vec3 dir, out int face, out vec2 uv) {
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

vec3 kiln_sample_sky(samplerCube sky_tex, vec3 dir) {
	return textureLod(sky_tex, dir, 0.0).rgb;
}
