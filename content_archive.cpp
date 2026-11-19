#include "content_archive.h"

#include <SDL3/SDL.h>

#include "archive.h"
#include "content_cache.h"
#include "host_fs.h"

bool content_archive_core_takes_whole(const LibretroCoreInfo* core, const char* archive_path)
{
    if (core == nullptr || archive_format(archive_path) == ArchiveFormat::none)
    {
        return false;
    }
    if (core->block_extract)
    {
        return true;
    }
    // An empty extension list means "anything": not a statement that the
    // core reads archives, so those get the archive's files instead.
    return core->extensions[0] != '\0' && libretro_core_info_accepts(core, archive_path);
}

bool content_archive_resolve(const char* content_path, const char* cache_directory, std::string* load_path)
{
    std::string archive;
    std::string name;
    if (!archive_split_path(content_path, &archive, &name))
    {
        *load_path = content_path;
        return true;
    }
    if (name.empty())
    {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Content: %s names an archive, not a file in it", content_path);
        return false;
    }
    if (cache_directory == nullptr || cache_directory[0] == '\0')
    {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Content: no cache folder to extract %s into", content_path);
        return false;
    }
    return content_cache_extract(cache_directory, archive.c_str(), name.c_str(), load_path);
}

bool content_archive_game_name(const char* content_path, char* out, size_t out_size)
{
    std::string archive;
    std::string name;
    if (archive_split_path(content_path, &archive, &name))
    {
        return host_fs_stem(name.c_str(), out, out_size);
    }
    return host_fs_stem(content_path, out, out_size);
}
