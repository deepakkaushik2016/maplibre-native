// CLI wrapper around the engine's own WGSL preprocessor: wgslpp DEFINE... < in > out
#include <mln/shaders/webgpu/wgsl_preprocessor.hpp>
#include <iostream>
#include <iterator>
int main(int argc, char** argv) {
    std::unordered_map<std::string, bool> defines;
    for (int i = 1; i < argc; ++i) defines.emplace(argv[i], true);
    std::string src((std::istreambuf_iterator<char>(std::cin)), std::istreambuf_iterator<char>());
    std::cout << mln::webgpu::detail::preprocessWGSL(src, defines);
}
