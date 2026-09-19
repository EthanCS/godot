// lighting BRDF stack, ported for the parity comparison.
// Sources: lighting assets/shaders/inc/{brdf,brdf_lut,layered_brdf}.hlsl.
// USE_GGX_VNDF_SAMPLING 1, USE_GGX_CORRELATED_MASKING 1 as in the reference.
// The FG LUT is supplied per stage as `kiln_fg_lut` (64x64 RG32F-like sampled texture).

const float KILN_M_TAU = 6.28318530717958647692;
const float KILN_M_FRAC_1_PI = 0.31830988618379067154;
const float KILN_BRDF_SAMPLING_MIN_COS = 1e-5;

const vec2 KILN_BRDF_FG_LUT_UV_SCALE = (vec2(64.0) - 1.0) / vec2(64.0);
const vec2 KILN_BRDF_FG_LUT_UV_BIAS = 0.5 / vec2(64.0);

float kiln_calculate_luma(vec3 col) {
	return dot(vec3(0.2126, 0.7152, 0.0722), col);
}

struct KilnBrdfValue {
	vec3 value_over_pdf;
	vec3 value;
	float pdf;
	vec3 transmission_fraction;
};

KilnBrdfValue kiln_brdf_value_invalid() {
	KilnBrdfValue res;
	res.value_over_pdf = vec3(0.0);
	res.value = vec3(0.0);
	res.pdf = 0.0;
	res.transmission_fraction = vec3(0.0);
	return res;
}

struct KilnBrdfSample {
	vec3 value_over_pdf;
	vec3 value;
	float pdf;
	vec3 transmission_fraction;
	vec3 wi;
	float approx_roughness;
};

KilnBrdfSample kiln_brdf_sample_invalid() {
	KilnBrdfSample res;
	res.value_over_pdf = vec3(0.0);
	res.value = vec3(0.0);
	res.pdf = 0.0;
	res.transmission_fraction = vec3(0.0);
	res.wi = vec3(0.0, 0.0, -1.0);
	res.approx_roughness = 0.0;
	return res;
}

bool kiln_brdf_sample_is_valid(KilnBrdfSample s) {
	return s.wi.z > 1e-6;
}

struct KilnDiffuseBrdf {
	vec3 albedo;
};

KilnBrdfSample kiln_diffuse_sample(KilnDiffuseBrdf b, vec2 urand) {
	float phi = urand.x * KILN_M_TAU;
	float cos_theta = sqrt(max(0.0, 1.0 - urand.y));
	float sin_theta = sqrt(max(0.0, 1.0 - cos_theta * cos_theta));

	KilnBrdfSample res;
	res.wi = vec3(cos(phi) * sin_theta, sin(phi) * sin_theta, cos_theta);
	res.pdf = KILN_M_FRAC_1_PI;
	res.value_over_pdf = b.albedo;
	res.value = res.value_over_pdf * res.pdf;
	res.transmission_fraction = vec3(0.0);
	res.approx_roughness = 1.0;
	return res;
}

KilnBrdfValue kiln_diffuse_evaluate(KilnDiffuseBrdf b, vec3 wi) {
	KilnBrdfValue res;
	res.pdf = wi.z > 0.0 ? KILN_M_FRAC_1_PI : 0.0;
	res.value_over_pdf = wi.z > 0.0 ? b.albedo : vec3(0.0);
	res.value = res.value_over_pdf * res.pdf;
	res.transmission_fraction = vec3(0.0);
	return res;
}

vec3 kiln_eval_fresnel_schlick(vec3 f0, vec3 f90, float cos_theta) {
	return mix(f0, f90, pow(max(0.0, 1.0 - cos_theta), 5.0));
}

float kiln_g_smith_ggx_correlated(float ndotv, float ndotl, float a2) {
	float lambda_v = ndotl * sqrt((-ndotv * a2 + ndotv) * ndotv + a2);
	float lambda_l = ndotv * sqrt((-ndotl * a2 + ndotl) * ndotl + a2);
	return 2.0 * ndotl * ndotv / (lambda_v + lambda_l);
}

float kiln_g_smith_ggx1(float ndotv, float a2) {
	float tan2_v = (1.0 - ndotv * ndotv) / (ndotv * ndotv);
	return 2.0 / (1.0 + sqrt(1.0 + a2 * tan2_v));
}

float kiln_ggx_ndf(float a2, float cos_theta) {
	float denom_sqrt = cos_theta * cos_theta * (a2 - 1.0) + 1.0;
	return a2 / (PI * denom_sqrt * denom_sqrt);
}

float kiln_ggx_ndf_0_1(float a2, float cos_theta) {
	float denom_sqrt = cos_theta * cos_theta * (a2 - 1.0) + 1.0;
	return a2 * a2 / (denom_sqrt * denom_sqrt);
}

float kiln_pdf_ggx(float a2, float cos_theta) {
	return kiln_ggx_ndf(a2, cos_theta) * cos_theta;
}

float kiln_pdf_ggx_vn(float a2, vec3 wo, vec3 h) {
	float g1 = kiln_g_smith_ggx1(wo.z, a2);
	float d = kiln_ggx_ndf(a2, h.z);
	return g1 * d * max(0.0, dot(wo, h)) / wo.z;
}

struct KilnNdfSample {
	vec3 m;
	float pdf;
};

KilnNdfSample kiln_sample_ndf(float roughness, vec2 urand) {
	const float a2 = roughness * roughness;

	const float cos2_theta = (1.0 - urand.x) / (1.0 - urand.x + a2 * urand.x);
	const float cos_theta = sqrt(cos2_theta);
	const float phi = KILN_M_TAU * urand.y;

	const float sin_theta = sqrt(max(0.0, 1.0 - cos2_theta));

	KilnNdfSample res;
	res.m = vec3(cos(phi) * sin_theta, sin(phi) * sin_theta, cos_theta);
	res.pdf = kiln_pdf_ggx(a2, cos_theta);
	return res;
}

// Heitz 2018 VNDF sampling for the identical alpha in both axes.
KilnNdfSample kiln_sample_vndf(float roughness, vec3 wo, vec2 urand) {
	float alpha = roughness;
	float a2 = alpha * alpha;

	// Transform the view vector to the hemisphere configuration.
	vec3 Vh = normalize(vec3(alpha * wo.x, alpha * wo.y, wo.z));

	// Construct orthonormal basis (Vh, T1, T2).
	vec3 T1 = (Vh.z < 0.9999) ? normalize(cross(vec3(0.0, 0.0, 1.0), Vh)) : vec3(1.0, 0.0, 0.0);
	vec3 T2 = cross(Vh, T1);

	// Parameterization of the projected area of the hemisphere.
	float r = sqrt(urand.x);
	float phi = (2.0 * PI) * urand.y;
	float t1 = r * cos(phi);
	float t2 = r * sin(phi);
	float s = 0.5 * (1.0 + Vh.z);
	t2 = (1.0 - s) * sqrt(1.0 - t1 * t1) + s * t2;

	// Reproject onto hemisphere.
	vec3 Nh = t1 * T1 + t2 * T2 + sqrt(max(0.0, 1.0 - t1 * t1 - t2 * t2)) * Vh;

	// Transform the normal back to the ellipsoid configuration.
	vec3 h = normalize(vec3(alpha * Nh.x, alpha * Nh.y, max(0.0, Nh.z)));
	float pdf = kiln_pdf_ggx_vn(a2, wo, h);

	KilnNdfSample res;
	res.m = h;
	res.pdf = pdf;
	return res;
}

struct KilnSpecularBrdf {
	float roughness;
	vec3 albedo;
};

KilnBrdfSample kiln_specular_sample(KilnSpecularBrdf b, vec3 wo, vec2 urand) {
	KilnNdfSample ndf_sample = kiln_sample_vndf(b.roughness, wo, urand);

	const vec3 wi = reflect(-wo, ndf_sample.m);

	if (ndf_sample.m.z <= KILN_BRDF_SAMPLING_MIN_COS || wi.z <= KILN_BRDF_SAMPLING_MIN_COS || wo.z <= KILN_BRDF_SAMPLING_MIN_COS) {
		return kiln_brdf_sample_invalid();
	}

	// Change of variables from half-direction space to regular lighting geometry.
	const float jacobian = 1.0 / (4.0 * dot(wi, ndf_sample.m));

	const vec3 fresnel = kiln_eval_fresnel_schlick(b.albedo, vec3(1.0), dot(ndf_sample.m, wi));
	const float a2 = b.roughness * b.roughness;
	const float cos_theta = ndf_sample.m.z;

	const float g = kiln_g_smith_ggx_correlated(wo.z, wi.z, a2);
	const float g_over_g1_wo = g / kiln_g_smith_ggx1(wo.z, a2);

	KilnBrdfSample res;
	res.pdf = ndf_sample.pdf * jacobian / wi.z;
	res.wi = wi;
	res.transmission_fraction = vec3(1.0) - fresnel;
	res.approx_roughness = b.roughness;
	res.value_over_pdf = fresnel * g_over_g1_wo;
	res.value = fresnel * g * kiln_ggx_ndf(a2, cos_theta) / (4.0 * wo.z * wi.z);

	return res;
}

KilnBrdfValue kiln_specular_evaluate(KilnSpecularBrdf b, vec3 wo, vec3 wi) {
	if (wi.z <= 0.0 || wo.z <= 0.0) {
		return kiln_brdf_value_invalid();
	}

	const float a2 = b.roughness * b.roughness;

	const vec3 m = normalize(wo + wi);

	const float cos_theta = m.z;

	const float pdf_h = kiln_pdf_ggx_vn(a2, wo, m);

	const float jacobian = 1.0 / (4.0 * dot(wi, m));

	const vec3 fresnel = kiln_eval_fresnel_schlick(b.albedo, vec3(1.0), dot(m, wi));

	const float g = kiln_g_smith_ggx_correlated(wo.z, wi.z, a2);
	const float g_over_g1_wo = g / kiln_g_smith_ggx1(wo.z, a2);

	KilnBrdfValue res;
	res.pdf = pdf_h * jacobian / wi.z;
	res.transmission_fraction = vec3(1.0) - fresnel;
	res.value_over_pdf = fresnel * g_over_g1_wo;
	res.value = fresnel * g * kiln_ggx_ndf(a2, cos_theta) / (4.0 * wo.z * wi.z);

	return res;
}

// ---- brdf_lut.hlsl: split-sum energy preservation over the FG LUT ----
struct KilnSpecularBrdfEnergyPreservation {
	vec3 preintegrated_reflection;
	vec3 preintegrated_reflection_mult;
	vec3 preintegrated_transmission_fraction;
};

KilnSpecularBrdfEnergyPreservation kiln_energy_preservation_from_brdf_ndotv(KilnSpecularBrdf brdf, float ndotv, sampler2D fg_lut) {
	const float roughness = brdf.roughness;
	const vec3 specular_albedo = brdf.albedo;

	vec2 uv = vec2(ndotv, roughness) * KILN_BRDF_FG_LUT_UV_SCALE + KILN_BRDF_FG_LUT_UV_BIAS;
	vec2 fg = textureLod(fg_lut, uv, 0.0).xy;
	vec3 single_scatter = specular_albedo * fg.x + fg.y;

	// The active reference path: multi-scatter energy compensation reformulated
	// per Belcour 2018 sec. 5.1, with an ad-hoc F90 shift for subsequent bounces.
	float e_ss = fg.x + fg.y;
	vec3 f_ss = single_scatter / e_ss;
	// Ad-hoc shift towards F90 for subsequent bounces.
	vec3 f_ss_tail = mix(f_ss, vec3(1.0), 0.4);
	vec3 bounce_radiance = (1.0 - e_ss) * f_ss_tail;
	vec3 mult = 1.0 + bounce_radiance / (1.0 - bounce_radiance);

	KilnSpecularBrdfEnergyPreservation res;
	res.preintegrated_reflection = single_scatter * mult;
	res.preintegrated_reflection_mult = mult;
	res.preintegrated_transmission_fraction = vec3(1.0) - res.preintegrated_reflection;
	return res;
}

// ---- layered_brdf.hlsl ----
vec3 kiln_metalness_albedo_boost(float metalness, vec3 diffuse_albedo) {
	const float a0 = 1.749;
	const float a1 = -1.61;
	const float e1 = 0.5555;
	const float e3 = 0.8244;

	const float x = metalness;
	const vec3 y = diffuse_albedo;
	const vec3 y3 = y * y * y;

	return 1.0 + (0.25 - (x - 0.5) * (x - 0.5)) * (a0 + a1 * abs(x - 0.5)) * (e1 * y + e3 * y3);
}

void kiln_apply_metalness_to_brdfs(inout KilnSpecularBrdf specular_brdf, inout KilnDiffuseBrdf diffuse_brdf, float metalness) {
	const vec3 albedo = diffuse_brdf.albedo;

	specular_brdf.albedo = mix(specular_brdf.albedo, albedo, metalness);
	diffuse_brdf.albedo = max(0.0, 1.0 - metalness) * albedo;

	const vec3 albedo_boost = kiln_metalness_albedo_boost(metalness, albedo);
	specular_brdf.albedo = min(specular_brdf.albedo * albedo_boost, vec3(1.0));
	diffuse_brdf.albedo = min(diffuse_brdf.albedo * albedo_boost, vec3(1.0));
}

struct KilnLayeredBrdf {
	KilnSpecularBrdf specular_brdf;
	KilnDiffuseBrdf diffuse_brdf;
	KilnSpecularBrdfEnergyPreservation energy_preservation;
};

KilnLayeredBrdf kiln_layered_from_gbuffer_ndotv(vec3 albedo, float roughness, float metalness, float ndotv, sampler2D fg_lut) {
	KilnSpecularBrdf specular_brdf;
	specular_brdf.albedo = vec3(0.04);
	specular_brdf.roughness = roughness;

	KilnDiffuseBrdf diffuse_brdf;
	diffuse_brdf.albedo = albedo;

	kiln_apply_metalness_to_brdfs(specular_brdf, diffuse_brdf, metalness);

	KilnLayeredBrdf res;
	res.energy_preservation = kiln_energy_preservation_from_brdf_ndotv(specular_brdf, ndotv, fg_lut);
	res.specular_brdf = specular_brdf;
	res.diffuse_brdf = diffuse_brdf;
	return res;
}

vec3 kiln_layered_evaluate(KilnLayeredBrdf b, vec3 wo, vec3 wi) {
	if (wo.z <= 0.0 || wi.z <= 0.0) {
		return vec3(0.0);
	}

	const KilnBrdfValue diff = kiln_diffuse_evaluate(b.diffuse_brdf, wi);
	const KilnBrdfValue spec = kiln_specular_evaluate(b.specular_brdf, wo, wi);

	return spec.value * b.energy_preservation.preintegrated_reflection_mult + diff.value * spec.transmission_fraction;
}

vec3 kiln_layered_evaluate_directional_light(KilnLayeredBrdf b, vec3 wo, vec3 wi) {
	if (wo.z <= 0.0 || wi.z <= 0.0) {
		return vec3(0.0);
	}

	const KilnBrdfValue diff = kiln_diffuse_evaluate(b.diffuse_brdf, wi);
	const KilnBrdfValue spec = kiln_specular_evaluate(b.specular_brdf, wo, wi);

	const vec3 preintegrated_reflection_mult_directional = mix(vec3(1.0), b.energy_preservation.preintegrated_reflection_mult, sqrt(abs(wi.z)));

	return spec.value * preintegrated_reflection_mult_directional + diff.value * spec.transmission_fraction;
}

KilnBrdfSample kiln_layered_sample(KilnLayeredBrdf b, vec3 wo, vec3 urand) {
	KilnBrdfSample brdf_sample;

	// Single ray: toss a coin between reflection and transmission.
	const float spec_wt = kiln_calculate_luma(b.energy_preservation.preintegrated_reflection);
	const float diffuse_wt = kiln_calculate_luma(b.energy_preservation.preintegrated_transmission_fraction * b.diffuse_brdf.albedo);
	const float transmission_p = diffuse_wt / (spec_wt + diffuse_wt);

	const float lobe_xi = urand.z;
	if (lobe_xi < transmission_p) {
		// Transmission wins: sample the bottom layer (diffuse).
		brdf_sample = kiln_diffuse_sample(b.diffuse_brdf, urand.xy);

		const float lobe_pdf = transmission_p;
		brdf_sample.value_over_pdf /= lobe_pdf;

		// Account for the masking that the top level exerts on the bottom.
		brdf_sample.value_over_pdf *= b.energy_preservation.preintegrated_transmission_fraction;
	} else {
		// Reflection wins.
		brdf_sample = kiln_specular_sample(b.specular_brdf, wo, urand.xy);

		const float lobe_pdf = (1.0 - transmission_p);
		brdf_sample.value_over_pdf /= lobe_pdf;

		// Apply approximate multi-scatter energy preservation.
		brdf_sample.value_over_pdf *= b.energy_preservation.preintegrated_reflection_mult;
	}

	return brdf_sample;
}
