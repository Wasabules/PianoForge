#include <pianoforge/modal_profile.hpp>

#include <fstream>

namespace pf {

// Kept apart from the parser: a build without a file system (WebAssembly) links the parser alone.
std::shared_ptr<const ModalProfile> loadModalProfile(const std::filesystem::path& path, std::string& error)
{
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) {
        error = "cannot open " + path.string();
        return nullptr;
    }
    std::vector<char> data((std::size_t) in.tellg());
    in.seekg(0);
    in.read(data.data(), (std::streamsize) data.size());
    if (!in) {
        error = "cannot read " + path.string();
        return nullptr;
    }
    return parseModalProfile(std::move(data), path.string(), error);
}

} // namespace pf
