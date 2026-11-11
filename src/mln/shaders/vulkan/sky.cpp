#include <mln/shaders/vulkan/sky.hpp>
#include <mln/shaders/shader_defines.hpp>

namespace mln {
namespace shaders {

using SkyShaderSource = ShaderSource<BuiltIn::SkyShader, gfx::Backend::Type::Vulkan>;

const std::array<AttributeInfo, 1> SkyShaderSource::attributes = {
    AttributeInfo{0, gfx::AttributeDataType::Short2, idSkyPosVertexAttribute},
};
const std::array<TextureInfo, 0> SkyShaderSource::textures = {};

} // namespace shaders
} // namespace mln
