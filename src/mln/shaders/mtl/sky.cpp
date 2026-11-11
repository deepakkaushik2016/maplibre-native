#include <mln/shaders/mtl/sky.hpp>
#include <mln/shaders/shader_defines.hpp>

namespace mln {
namespace shaders {

using SkyShaderSource = ShaderSource<BuiltIn::SkyShader, gfx::Backend::Type::Metal>;

const std::array<AttributeInfo, 1> SkyShaderSource::attributes = {
    AttributeInfo{0, gfx::AttributeDataType::Short2, skyUBOCount + 0, idSkyPosVertexAttribute},
};
const std::array<TextureInfo, 0> SkyShaderSource::textures = {};

} // namespace shaders
} // namespace mln
