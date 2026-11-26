#include "ffx_frameinterpolation_reconstruct_previous_depth_pass_16bit_a45f665367e1d5b39e328908d85728b0.h"
#include "ffx_frameinterpolation_reconstruct_previous_depth_pass_16bit_b58579cf38e249a2cecb3d0ae84a2857.h"

typedef union ffx_frameinterpolation_reconstruct_previous_depth_pass_16bit_PermutationKey {
    struct {
        uint32_t FFX_FRAMEINTERPOLATION_OPTION_INVERTED_DEPTH : 1;
        uint32_t FFX_FRAMEINTERPOLATION_OPTION_LOW_RES_MOTION_VECTORS : 1;
        uint32_t FFX_FRAMEINTERPOLATION_OPTION_JITTER_MOTION_VECTORS : 1;
    };
    uint32_t index;
} ffx_frameinterpolation_reconstruct_previous_depth_pass_16bit_PermutationKey;

typedef struct ffx_frameinterpolation_reconstruct_previous_depth_pass_16bit_PermutationInfo {
    const uint32_t       blobSize;
    const unsigned char* blobData;


    const uint32_t  numConstantBuffers;
    const char**    constantBufferNames;
    const uint32_t* constantBufferBindings;
    const uint32_t* constantBufferCounts;
    const uint32_t* constantBufferSpaces;

    const uint32_t  numSRVTextures;
    const char**    srvTextureNames;
    const uint32_t* srvTextureBindings;
    const uint32_t* srvTextureCounts;
    const uint32_t* srvTextureSpaces;

    const uint32_t  numUAVTextures;
    const char**    uavTextureNames;
    const uint32_t* uavTextureBindings;
    const uint32_t* uavTextureCounts;
    const uint32_t* uavTextureSpaces;

    const uint32_t  numSRVBuffers;
    const char**    srvBufferNames;
    const uint32_t* srvBufferBindings;
    const uint32_t* srvBufferCounts;
    const uint32_t* srvBufferSpaces;

    const uint32_t  numUAVBuffers;
    const char**    uavBufferNames;
    const uint32_t* uavBufferBindings;
    const uint32_t* uavBufferCounts;
    const uint32_t* uavBufferSpaces;

    const uint32_t  numSamplers;
    const char**    samplerNames;
    const uint32_t* samplerBindings;
    const uint32_t* samplerCounts;
    const uint32_t* samplerSpaces;

    const uint32_t  numRTAccelerationStructures;
    const char**    rtAccelerationStructureNames;
    const uint32_t* rtAccelerationStructureBindings;
    const uint32_t* rtAccelerationStructureCounts;
    const uint32_t* rtAccelerationStructureSpaces;
} ffx_frameinterpolation_reconstruct_previous_depth_pass_16bit_PermutationInfo;

static const uint32_t g_ffx_frameinterpolation_reconstruct_previous_depth_pass_16bit_IndirectionTable[] = {
    1,
    0,
    1,
    0,
    1,
    0,
    1,
    0,
};

static const ffx_frameinterpolation_reconstruct_previous_depth_pass_16bit_PermutationInfo g_ffx_frameinterpolation_reconstruct_previous_depth_pass_16bit_PermutationInfo[] = {
    { g_ffx_frameinterpolation_reconstruct_previous_depth_pass_16bit_a45f665367e1d5b39e328908d85728b0_size, g_ffx_frameinterpolation_reconstruct_previous_depth_pass_16bit_a45f665367e1d5b39e328908d85728b0_data, 1, g_ffx_frameinterpolation_reconstruct_previous_depth_pass_16bit_a45f665367e1d5b39e328908d85728b0_CBVResourceNames, g_ffx_frameinterpolation_reconstruct_previous_depth_pass_16bit_a45f665367e1d5b39e328908d85728b0_CBVResourceBindings, g_ffx_frameinterpolation_reconstruct_previous_depth_pass_16bit_a45f665367e1d5b39e328908d85728b0_CBVResourceCounts, g_ffx_frameinterpolation_reconstruct_previous_depth_pass_16bit_a45f665367e1d5b39e328908d85728b0_CBVResourceSets, 4, g_ffx_frameinterpolation_reconstruct_previous_depth_pass_16bit_a45f665367e1d5b39e328908d85728b0_TextureSRVResourceNames, g_ffx_frameinterpolation_reconstruct_previous_depth_pass_16bit_a45f665367e1d5b39e328908d85728b0_TextureSRVResourceBindings, g_ffx_frameinterpolation_reconstruct_previous_depth_pass_16bit_a45f665367e1d5b39e328908d85728b0_TextureSRVResourceCounts, g_ffx_frameinterpolation_reconstruct_previous_depth_pass_16bit_a45f665367e1d5b39e328908d85728b0_TextureSRVResourceSets, 1, g_ffx_frameinterpolation_reconstruct_previous_depth_pass_16bit_a45f665367e1d5b39e328908d85728b0_TextureUAVResourceNames, g_ffx_frameinterpolation_reconstruct_previous_depth_pass_16bit_a45f665367e1d5b39e328908d85728b0_TextureUAVResourceBindings, g_ffx_frameinterpolation_reconstruct_previous_depth_pass_16bit_a45f665367e1d5b39e328908d85728b0_TextureUAVResourceCounts, g_ffx_frameinterpolation_reconstruct_previous_depth_pass_16bit_a45f665367e1d5b39e328908d85728b0_TextureUAVResourceSets, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, g_ffx_frameinterpolation_reconstruct_previous_depth_pass_16bit_a45f665367e1d5b39e328908d85728b0_SamplerResourceNames, g_ffx_frameinterpolation_reconstruct_previous_depth_pass_16bit_a45f665367e1d5b39e328908d85728b0_SamplerResourceBindings, g_ffx_frameinterpolation_reconstruct_previous_depth_pass_16bit_a45f665367e1d5b39e328908d85728b0_SamplerResourceCounts, g_ffx_frameinterpolation_reconstruct_previous_depth_pass_16bit_a45f665367e1d5b39e328908d85728b0_SamplerResourceSets, 0, 0, 0, 0, 0, },
    { g_ffx_frameinterpolation_reconstruct_previous_depth_pass_16bit_b58579cf38e249a2cecb3d0ae84a2857_size, g_ffx_frameinterpolation_reconstruct_previous_depth_pass_16bit_b58579cf38e249a2cecb3d0ae84a2857_data, 1, g_ffx_frameinterpolation_reconstruct_previous_depth_pass_16bit_b58579cf38e249a2cecb3d0ae84a2857_CBVResourceNames, g_ffx_frameinterpolation_reconstruct_previous_depth_pass_16bit_b58579cf38e249a2cecb3d0ae84a2857_CBVResourceBindings, g_ffx_frameinterpolation_reconstruct_previous_depth_pass_16bit_b58579cf38e249a2cecb3d0ae84a2857_CBVResourceCounts, g_ffx_frameinterpolation_reconstruct_previous_depth_pass_16bit_b58579cf38e249a2cecb3d0ae84a2857_CBVResourceSets, 4, g_ffx_frameinterpolation_reconstruct_previous_depth_pass_16bit_b58579cf38e249a2cecb3d0ae84a2857_TextureSRVResourceNames, g_ffx_frameinterpolation_reconstruct_previous_depth_pass_16bit_b58579cf38e249a2cecb3d0ae84a2857_TextureSRVResourceBindings, g_ffx_frameinterpolation_reconstruct_previous_depth_pass_16bit_b58579cf38e249a2cecb3d0ae84a2857_TextureSRVResourceCounts, g_ffx_frameinterpolation_reconstruct_previous_depth_pass_16bit_b58579cf38e249a2cecb3d0ae84a2857_TextureSRVResourceSets, 1, g_ffx_frameinterpolation_reconstruct_previous_depth_pass_16bit_b58579cf38e249a2cecb3d0ae84a2857_TextureUAVResourceNames, g_ffx_frameinterpolation_reconstruct_previous_depth_pass_16bit_b58579cf38e249a2cecb3d0ae84a2857_TextureUAVResourceBindings, g_ffx_frameinterpolation_reconstruct_previous_depth_pass_16bit_b58579cf38e249a2cecb3d0ae84a2857_TextureUAVResourceCounts, g_ffx_frameinterpolation_reconstruct_previous_depth_pass_16bit_b58579cf38e249a2cecb3d0ae84a2857_TextureUAVResourceSets, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, g_ffx_frameinterpolation_reconstruct_previous_depth_pass_16bit_b58579cf38e249a2cecb3d0ae84a2857_SamplerResourceNames, g_ffx_frameinterpolation_reconstruct_previous_depth_pass_16bit_b58579cf38e249a2cecb3d0ae84a2857_SamplerResourceBindings, g_ffx_frameinterpolation_reconstruct_previous_depth_pass_16bit_b58579cf38e249a2cecb3d0ae84a2857_SamplerResourceCounts, g_ffx_frameinterpolation_reconstruct_previous_depth_pass_16bit_b58579cf38e249a2cecb3d0ae84a2857_SamplerResourceSets, 0, 0, 0, 0, 0, },
};

