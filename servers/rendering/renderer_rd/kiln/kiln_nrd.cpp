// Kiln's native RenderingDevice adapter. Engine licensing: LICENSE.txt.
// NRD is an optional external SDK under its own NVIDIA RTX SDK license.
#include "kiln_nrd.h"

#include "core/io/file_access.h"
#include "core/os/os.h"
#include "servers/rendering/renderer_rd/storage_rd/material_storage.h"
#include "servers/rendering/renderer_rd/uniform_set_cache_rd.h"

#ifdef KILN_NRD_ENABLED
#include <NRD.h>

using namespace RendererRD;
struct KilnNRD::Impl {
	nrd::Instance *instance = nullptr;
	Size2i size;
	Vector<RID> permanent, transient, shaders, pipelines, constants;
	Vector<uint8_t> shared_sets;
	RID samplers[2];
};

bool KilnNRD::available() {
	return OS::get_singleton()->get_current_rendering_driver_name() == "vulkan";
}

static RD::DataFormat nrd_format(nrd::Format f) {
	static const RD::DataFormat formats[] = {
		RD::DATA_FORMAT_R8_UNORM, RD::DATA_FORMAT_R8_SNORM, RD::DATA_FORMAT_R8_UINT, RD::DATA_FORMAT_R8_SINT,
		RD::DATA_FORMAT_R8G8_UNORM, RD::DATA_FORMAT_R8G8_SNORM, RD::DATA_FORMAT_R8G8_UINT, RD::DATA_FORMAT_R8G8_SINT,
		RD::DATA_FORMAT_R8G8B8A8_UNORM, RD::DATA_FORMAT_R8G8B8A8_SNORM, RD::DATA_FORMAT_R8G8B8A8_UINT, RD::DATA_FORMAT_R8G8B8A8_SINT, RD::DATA_FORMAT_R8G8B8A8_SRGB,
		RD::DATA_FORMAT_R16_UNORM, RD::DATA_FORMAT_R16_SNORM, RD::DATA_FORMAT_R16_UINT, RD::DATA_FORMAT_R16_SINT, RD::DATA_FORMAT_R16_SFLOAT,
		RD::DATA_FORMAT_R16G16_UNORM, RD::DATA_FORMAT_R16G16_SNORM, RD::DATA_FORMAT_R16G16_UINT, RD::DATA_FORMAT_R16G16_SINT, RD::DATA_FORMAT_R16G16_SFLOAT,
		RD::DATA_FORMAT_R16G16B16A16_UNORM, RD::DATA_FORMAT_R16G16B16A16_SNORM, RD::DATA_FORMAT_R16G16B16A16_UINT, RD::DATA_FORMAT_R16G16B16A16_SINT, RD::DATA_FORMAT_R16G16B16A16_SFLOAT,
		RD::DATA_FORMAT_R32_UINT, RD::DATA_FORMAT_R32_SINT, RD::DATA_FORMAT_R32_SFLOAT,
		RD::DATA_FORMAT_R32G32_UINT, RD::DATA_FORMAT_R32G32_SINT, RD::DATA_FORMAT_R32G32_SFLOAT,
		RD::DATA_FORMAT_R32G32B32_UINT, RD::DATA_FORMAT_R32G32B32_SINT, RD::DATA_FORMAT_R32G32B32_SFLOAT,
		RD::DATA_FORMAT_R32G32B32A32_UINT, RD::DATA_FORMAT_R32G32B32A32_SINT, RD::DATA_FORMAT_R32G32B32A32_SFLOAT,
		RD::DATA_FORMAT_A2B10G10R10_UNORM_PACK32, RD::DATA_FORMAT_A2B10G10R10_UINT_PACK32, RD::DATA_FORMAT_B10G11R11_UFLOAT_PACK32, RD::DATA_FORMAT_E5B9G9R9_UFLOAT_PACK32
	};
	static_assert(sizeof(formats) / sizeof(formats[0]) == uint32_t(nrd::Format::MAX_NUM));
	return formats[uint32_t(f)];
}

bool KilnNRD::initialize(Size2i size) {
	ERR_FAIL_COND_V(impl || !available(), false);
	const auto *lib = nrd::GetLibraryDesc();
	ERR_FAIL_COND_V(lib->versionMajor != 4 || lib->versionMinor != 17 || lib->versionBuild != 3 || lib->normalEncoding != nrd::NormalEncoding::R10_G10_B10_A2_UNORM || lib->roughnessEncoding != nrd::RoughnessEncoding::LINEAR, false);
	impl = memnew(Impl);
	impl->size = size;
	// Preserve the two material-independent Schlick integrals in separate histories.
	nrd::DenoiserDesc denoisers[] = { { 0, nrd::Denoiser::RELAX_DIFFUSE_SPECULAR }, { 1, nrd::Denoiser::RELAX_SPECULAR } };
	nrd::InstanceCreationDesc create = {};
	create.denoisers = denoisers;
	create.denoisersNum = 2;
	ERR_FAIL_COND_V(nrd::CreateInstance(create, impl->instance) != nrd::Result::SUCCESS, false);
	const auto *desc = nrd::GetInstanceDesc(*impl->instance);
	RD *rd = RD::get_singleton();
	for (int pool = 0; pool < 2; pool++) {
		const auto *textures = pool ? desc->transientPool : desc->permanentPool;
		uint32_t count = pool ? desc->transientPoolSize : desc->permanentPoolSize;
		Vector<RID> &out = pool ? impl->transient : impl->permanent;
		for (uint32_t i = 0; i < count; i++) {
			RD::TextureFormat format;
			format.width = (size.x + textures[i].downsampleFactor - 1) / textures[i].downsampleFactor;
			format.height = (size.y + textures[i].downsampleFactor - 1) / textures[i].downsampleFactor;
			format.format = nrd_format(textures[i].format);
			format.usage_bits = RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_STORAGE_BIT | RD::TEXTURE_USAGE_CAN_COPY_TO_BIT;
			RID texture = rd->texture_create(format, RD::TextureView());
			ERR_FAIL_COND_V(texture.is_null(), false);
			out.push_back(texture);
			rd->texture_clear(texture, Color(), 0, 1, 0, 1);
		}
	}
	impl->shaders.resize(desc->pipelinesNum);
	impl->pipelines.resize(desc->pipelinesNum);
	impl->shared_sets.resize(desc->pipelinesNum);
	impl->shared_sets.fill(0);
	for (int i = 0; i < 2; i++) {
		RD::SamplerState s;
		s.min_filter = s.mag_filter = i ? RD::SAMPLER_FILTER_LINEAR : RD::SAMPLER_FILTER_NEAREST;
		impl->samplers[i] = rd->sampler_create(s);
	}
	print_line("[KILN_NRD] NRD 4.17.3 RELAX: diffuse + Schlick base/fresnel, native Vulkan RD adapter");
	return true;
}

bool KilnNRD::denoise(RenderSceneDataRD *scene, const Projection &prev_projection, const Transform3D &prev_camera, uint32_t frame, bool reset, bool changing, RID motion, RID normal, RID depth, RID diffuse, RID base, RID fresnel, RID out_diffuse, RID out_base, RID out_fresnel) {
	ERR_FAIL_COND_V(!impl || !impl->instance, false);
	nrd::CommonSettings settings = {};
	Projection correction;
	// NRD uses D3D-style UV reconstruction (Y down), unlike Godot's GPU
	// projection, which already folds the framebuffer Y inversion into it.
	correction.set_depth_correction(false);
	MaterialStorage::store_camera(correction * scene->cam_projection, settings.viewToClipMatrix);
	MaterialStorage::store_camera(correction * (reset ? scene->cam_projection : prev_projection), settings.viewToClipMatrixPrev);
	MaterialStorage::store_camera(Projection(scene->cam_transform.affine_inverse()), settings.worldToViewMatrix);
	MaterialStorage::store_camera(Projection((reset ? scene->cam_transform : prev_camera).affine_inverse()), settings.worldToViewMatrixPrev);
	for (int i = 0; i < 2; i++) {
		settings.resourceSize[i] = settings.resourceSizePrev[i] = settings.rectSize[i] = settings.rectSizePrev[i] = impl->size[i];
		settings.cameraJitter[i] = -scene->taa_jitter[i] * impl->size[i] * 0.5f;
		settings.cameraJitterPrev[i] = -scene->prev_taa_jitter[i] * impl->size[i] * 0.5f;
	}
	settings.frameIndex = frame;
	settings.denoisingRange = 10000.0f;
	settings.accumulationMode = reset ? nrd::AccumulationMode::CLEAR_AND_RESTART : nrd::AccumulationMode::CONTINUE;
	ERR_FAIL_COND_V(nrd::SetCommonSettings(*impl->instance, settings) != nrd::Result::SUCCESS, false);
	nrd::RelaxSettings relax = {};
	relax.enableAntiFirefly = true;
	relax.diffuseMaxAccumulatedFrameNum = changing ? 8 : 60;
	relax.specularMaxAccumulatedFrameNum = changing ? 8 : 30;
	relax.atrousIterationNum = 6;
	relax.diffusePhiLuminance = 3.0f;
	const nrd::Identifier identifiers[] = { 0, 1 };
	for (auto id : identifiers) {
		nrd::SetDenoiserSettings(*impl->instance, id, &relax);
	}
	const nrd::DispatchDesc *dispatches;
	uint32_t count;
	ERR_FAIL_COND_V(nrd::GetComputeDispatches(*impl->instance, identifiers, 2, dispatches, count) != nrd::Result::SUCCESS, false);
	const auto *desc = nrd::GetInstanceDesc(*impl->instance);
	const auto &offsets = nrd::GetLibraryDesc()->spirvBindingOffsets;
	RD *rd = RD::get_singleton();
	for (uint32_t i = 0; i < count; i++) {
		const auto &d = dispatches[i];
		const auto &p = desc->pipelines[d.pipelineIndex];
		RID &shader = impl->shaders.write[d.pipelineIndex];
		RID &pipeline = impl->pipelines.write[d.pipelineIndex];
		if (shader.is_null()) {
			// Godot uses entry point "main". Rename only OpEntryPoint's string;
			// interface IDs and all executable SPIR-V instructions remain intact.
			const uint32_t *words = static_cast<const uint32_t *>(p.computeShaderSPIRV.bytecode);
			ERR_FAIL_COND_V(!words || !p.computeShaderSPIRV.size, false);
			Vector<uint32_t> patched;
			for (uint32_t j = 0; j < 5; j++) {
				patched.push_back(words[j]);
			}
			for (uint32_t j = 5; j < p.computeShaderSPIRV.size / 4;) {
				uint32_t n = words[j] >> 16;
				if ((words[j] & 65535) == 71 && n == 4 && words[j + 2] == 34 && words[j + 3] == desc->constantBufferAndSamplersSpaceIndex) {
					impl->shared_sets.write[d.pipelineIndex] = 1;
				}
				if ((words[j] & 65535) == 15) {
					uint32_t old_string = (strlen(reinterpret_cast<const char *>(words + j + 3)) + 4) / 4;
					patched.push_back(((n - old_string + 2) << 16) | 15);
					patched.push_back(words[j + 1]);
					patched.push_back(words[j + 2]);
					patched.push_back(0x6e69616d);
					patched.push_back(0);
					for (uint32_t k = 3 + old_string; k < n; k++) {
						patched.push_back(words[j + k]);
					}
				} else {
					for (uint32_t k = 0; k < n; k++) {
						patched.push_back(words[j + k]);
					}
				}
				j += n;
			}
			RD::ShaderStageSPIRVData stage;
			stage.shader_stage = RD::SHADER_STAGE_COMPUTE;
			stage.spirv.resize(patched.size() * 4);
			memcpy(stage.spirv.ptrw(), patched.ptr(), stage.spirv.size());
			if (OS::get_singleton()->has_environment("KILN_NRD_SPIRV_DUMP")) {
				Ref<FileAccess> dump = FileAccess::open(OS::get_singleton()->get_environment("KILN_NRD_SPIRV_DUMP").path_join(itos(d.pipelineIndex) + ".spv"), FileAccess::WRITE);
				if (dump.is_valid()) {
					dump->store_buffer(stage.spirv);
				}
			}
			Vector<RD::ShaderStageSPIRVData> stages;
			stages.push_back(stage);
			shader = rd->shader_create_from_spirv(stages, String("Kiln NRD / ") + p.shaderIdentifier);
			ERR_FAIL_COND_V(shader.is_null(), false);
			pipeline = rd->compute_pipeline_create(shader);
			ERR_FAIL_COND_V(pipeline.is_null(), false);
		}
		while (impl->constants.size() <= int(i)) {
			impl->constants.push_back(rd->uniform_buffer_create(desc->constantBufferMaxDataSize));
		}
		LocalVector<RD::Uniform> resources, shared;
		uint32_t srv = offsets.textureOffset + desc->resourcesBaseRegisterIndex;
		uint32_t uav = offsets.storageTextureAndBufferOffset + desc->resourcesBaseRegisterIndex;
		for (uint32_t j = 0; j < d.resourcesNum; j++) {
			const auto &r = d.resources[j];
			RID texture;
			switch (r.type) {
				case nrd::ResourceType::PERMANENT_POOL:
					texture = impl->permanent[r.indexInPool];
					break;
				case nrd::ResourceType::TRANSIENT_POOL:
					texture = impl->transient[r.indexInPool];
					break;
				case nrd::ResourceType::IN_MV:
					texture = motion;
					break;
				case nrd::ResourceType::IN_NORMAL_ROUGHNESS:
					texture = normal;
					break;
				case nrd::ResourceType::IN_VIEWZ:
					texture = depth;
					break;
				case nrd::ResourceType::IN_DIFF_RADIANCE_HITDIST:
					texture = diffuse;
					break;
				case nrd::ResourceType::IN_SPEC_RADIANCE_HITDIST:
					texture = d.identifier == 0 ? base : fresnel;
					break;
				case nrd::ResourceType::OUT_DIFF_RADIANCE_HITDIST:
					texture = out_diffuse;
					break;
				case nrd::ResourceType::OUT_SPEC_RADIANCE_HITDIST:
					texture = d.identifier == 0 ? out_base : out_fresnel;
					break;
				default:
					ERR_FAIL_V_MSG(false, "Unexpected NRD resource.");
			}
			RD::Uniform u;
			bool storage = r.descriptorType == nrd::DescriptorType::STORAGE_TEXTURE;
			u.uniform_type = storage ? RD::UNIFORM_TYPE_IMAGE : RD::UNIFORM_TYPE_TEXTURE;
			u.binding = storage ? uav++ : srv++;
			u.append_id(texture);
			resources.push_back(u);
		}
		for (uint32_t j = 0; j < desc->samplersNum; j++) {
			RD::Uniform u;
			u.uniform_type = RD::UNIFORM_TYPE_SAMPLER;
			u.binding = offsets.samplerOffset + desc->samplersBaseRegisterIndex + j;
			u.append_id(impl->samplers[uint32_t(desc->samplers[j])]);
			shared.push_back(u);
		}
		if (p.hasConstantData) {
			rd->buffer_update(impl->constants[i], 0, d.constantBufferDataSize, d.constantBufferData);
			RD::Uniform u;
			u.uniform_type = RD::UNIFORM_TYPE_UNIFORM_BUFFER;
			u.binding = offsets.constantBufferOffset + desc->constantBufferRegisterIndex;
			u.append_id(impl->constants[i]);
			shared.push_back(u);
		}
		RID set0 = UniformSetCacheRD::get_singleton()->get_cache_vec(shader, desc->resourcesSpaceIndex, resources);
		bool has_shared = impl->shared_sets[d.pipelineIndex];
		RID set1 = has_shared ? UniformSetCacheRD::get_singleton()->get_cache_vec(shader, desc->constantBufferAndSamplersSpaceIndex, shared) : RID();
		ERR_FAIL_COND_V(set0.is_null() || (has_shared && set1.is_null()), false);
		auto list = rd->compute_list_begin();
		rd->compute_list_bind_compute_pipeline(list, pipeline);
		rd->compute_list_bind_uniform_set(list, set0, desc->resourcesSpaceIndex);
		if (has_shared) {
			rd->compute_list_bind_uniform_set(list, set1, desc->constantBufferAndSamplersSpaceIndex);
		}
		rd->compute_list_dispatch(list, d.gridWidth, d.gridHeight, 1);
		rd->compute_list_end();
	}
	return true;
}

KilnNRD::~KilnNRD() {
	if (!impl) {
		return;
	}
	RD *rd = RD::get_singleton();
	for (const auto *pool : { &impl->permanent, &impl->transient, &impl->shaders, &impl->constants }) {
		for (RID rid : *pool) {
			if (rid.is_valid()) {
				rd->free_rid(rid);
			}
		}
	}
	for (RID rid : impl->samplers) {
		if (rid.is_valid()) {
			rd->free_rid(rid);
		}
	}
	if (impl->instance) {
		nrd::DestroyInstance(*impl->instance);
	}
	memdelete(impl);
}
#else
using namespace RendererRD;
bool KilnNRD::available() {
	return false;
}
bool KilnNRD::initialize(Size2i) {
	return false;
}
bool KilnNRD::denoise(RenderSceneDataRD *, const Projection &, const Transform3D &, uint32_t, bool, bool, RID, RID, RID, RID, RID, RID, RID, RID, RID) {
	return false;
}
KilnNRD::~KilnNRD() {}
#endif
