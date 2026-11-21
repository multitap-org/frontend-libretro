#include "core_catalog.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>

#include <SDL3/SDL.h>

#include "host_fs.h"

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static bool ends_with(const char* text, const char* suffix)
{
    size_t text_length = std::strlen(text);
    size_t suffix_length = std::strlen(suffix);
    return text_length >= suffix_length && std::strcmp(text + text_length - suffix_length, suffix) == 0;
}

static bool is_core_file(const char* name)
{
    return ends_with(name, ".so") || ends_with(name, ".dll") || ends_with(name, ".dylib");
}

static void copy_text(char* destination, size_t size, const char* source)
{
    std::snprintf(destination, size, "%s", source != nullptr ? source : "");
}

bool core_catalog_path(const CoreCatalog* catalog, const CoreCatalogEntry* entry, char* out, size_t out_size)
{
    int written = std::snprintf(out, out_size, "%s/%s", catalog->directory, entry->file_name);
    return written > 0 && static_cast<size_t>(written) < out_size;
}

// Usable cores first, by name (ignoring case), then the rest by file name.
static int compare_entries(const CoreCatalogEntry* x, const CoreCatalogEntry* y)
{
    if (x->info.usable != y->info.usable)
    {
        return x->info.usable ? -1 : 1;
    }
    int by_name = x->info.usable ? SDL_strcasecmp(x->info.name, y->info.name) : 0;
    return by_name != 0 ? by_name : std::strcmp(x->file_name, y->file_name);
}

// Insertion sort, not qsort: an entry holds std::vectors (the subsystems),
// which qsort's byte copies must not move. At most CORE_CATALOG_MAX entries.
static void sort_entries(CoreCatalogEntry* entries, int32_t count)
{
    for (int32_t i = 1; i < count; i++)
    {
        for (int32_t j = i; j > 0 && compare_entries(&entries[j], &entries[j - 1]) < 0; j--)
        {
            std::swap(entries[j], entries[j - 1]);
        }
    }
}

// ---------------------------------------------------------------------------
// Cache file
// ---------------------------------------------------------------------------

// The cache's format. A file with another version (or none: before
// subsystems were cached) is ignored, so every core is peeked again once.
static const char CACHE_VERSION[] = "2";

static bool is_key(const char* key, const char* wanted)
{
    return std::strcmp(key, wanted) == 0;
}

static bool is_one(const char* value)
{
    return std::strcmp(value, "1") == 0;
}

// Subsystems, one key per line, each applying to the last subsystem / file:
//   subsystem=sgb                 starts a subsystem (its ident)
//   subsystem.id=257
//   subsystem.description=Super Game Boy
//   subsystem.rom=Game Boy ROM    starts one of its files (its description)
//   subsystem.rom.extensions=gb|gbc
//   subsystem.rom.flags=101       required, need_fullpath, block_extract
//   subsystem.rom.memory=0x101 sav
static void set_subsystem_field(LibretroCoreInfo* info, const char* key, const char* value)
{
    std::vector<LibretroSubsystem>* list = &info->subsystems.list;
    if (is_key(key, "subsystem"))
    {
        if (list->size() < LIBRETRO_SUBSYSTEMS_MAX)
        {
            LibretroSubsystem subsystem = {};
            copy_text(subsystem.ident, sizeof(subsystem.ident), value);
            list->push_back(subsystem);
        }
        return;
    }
    if (list->empty())
    {
        return;
    }
    LibretroSubsystem* subsystem = &list->back();
    if (is_key(key, "subsystem.id"))
    {
        subsystem->id = static_cast<unsigned>(std::strtoul(value, nullptr, 10));
    }
    else if (is_key(key, "subsystem.description"))
    {
        copy_text(subsystem->description, sizeof(subsystem->description), value);
    }
    else if (is_key(key, "subsystem.rom"))
    {
        if (subsystem->roms.size() < LIBRETRO_SUBSYSTEM_ROMS_MAX)
        {
            LibretroSubsystemRom rom = {};
            copy_text(rom.description, sizeof(rom.description), value);
            subsystem->roms.push_back(rom);
        }
    }
    else if (!subsystem->roms.empty())
    {
        LibretroSubsystemRom* rom = &subsystem->roms.back();
        if (is_key(key, "subsystem.rom.extensions"))
        {
            copy_text(rom->extensions, sizeof(rom->extensions), value);
        }
        else if (is_key(key, "subsystem.rom.flags") && std::strlen(value) == 3)
        {
            rom->required      = value[0] == '1';
            rom->need_fullpath = value[1] == '1';
            rom->block_extract = value[2] == '1';
        }
        else if (is_key(key, "subsystem.rom.memory") && rom->memory_count < LIBRETRO_SUBSYSTEM_MEMORY_MAX)
        {
            LibretroSubsystemMemory* memory = &rom->memory[rom->memory_count];
            char* end = nullptr;
            memory->type = static_cast<unsigned>(std::strtoul(value, &end, 16));
            if (end != value && *end == ' ' && end[1] != '\0')
            {
                copy_text(memory->extension, sizeof(memory->extension), end + 1);
                rom->memory_count++;
            }
        }
    }
}

static void set_field(CoreCatalogEntry* entry, const char* key, const char* value)
{
    LibretroCoreInfo* info = &entry->info;
    if (is_key(key, "size"))
    {
        entry->size = std::strtoull(value, nullptr, 10);
    }
    else if (is_key(key, "mtime"))
    {
        entry->mtime = std::strtoll(value, nullptr, 10);
    }
    else if (is_key(key, "usable"))
    {
        info->usable = is_one(value);
    }
    else if (is_key(key, "error"))
    {
        copy_text(info->error, sizeof(info->error), value);
    }
    else if (is_key(key, "name"))
    {
        copy_text(info->name, sizeof(info->name), value);
    }
    else if (is_key(key, "version"))
    {
        copy_text(info->version, sizeof(info->version), value);
    }
    else if (is_key(key, "extensions"))
    {
        copy_text(info->extensions, sizeof(info->extensions), value);
    }
    else if (is_key(key, "need_fullpath"))
    {
        info->need_fullpath = is_one(value);
    }
    else if (is_key(key, "block_extract"))
    {
        info->block_extract = is_one(value);
    }
    else if (is_key(key, "supports_no_game"))
    {
        info->supports_no_game = is_one(value);
    }
    else if (is_key(key, "option_count"))
    {
        info->option_count = static_cast<uint32_t>(std::strtoul(value, nullptr, 10));
    }
    else if (is_key(key, "options_api"))
    {
        info->options_api = static_cast<uint32_t>(std::strtoul(value, nullptr, 10));
    }
    else if (is_key(key, "api_version"))
    {
        info->api_version = static_cast<unsigned>(std::strtoul(value, nullptr, 10));
    }
    else
    {
        set_subsystem_field(info, key, value);
    }
}

static void load_cache(CoreCatalog* catalog)
{
    catalog->count = 0;
    uint8_t* data = nullptr;
    size_t size = 0;
    if (!host_fs_exists(catalog->cache_path) || !host_fs_read_file(catalog->cache_path, &data, &size))
    {
        return;
    }

    // Line by line, copying each line into a small buffer so it can be split.
    const char* text = static_cast<const char*>(static_cast<const void*>(data));
    CoreCatalogEntry* entry = nullptr;
    bool version_ok = false;
    size_t start = 0;
    while (start < size)
    {
        size_t end = start;
        while (end < size && text[end] != '\n')
        {
            end++;
        }
        char line[LIBRETRO_EXTENSIONS_MAX + 64] = {};
        size_t length = end - start < sizeof(line) - 1 ? end - start : sizeof(line) - 1;
        std::memcpy(line, text + start, length);
        start = end + 1;

        if (line[0] == '[' && length > 2 && line[length - 1] == ']')
        {
            entry = nullptr;
            if (catalog->count < CORE_CATALOG_MAX)
            {
                entry = &catalog->entries[catalog->count];
                *entry = CoreCatalogEntry{};
                line[length - 1] = '\0';
                copy_text(entry->file_name, sizeof(entry->file_name), line + 1);
                catalog->count++;
            }
            continue;
        }
        char* equals = std::strchr(line, '=');
        if (equals == nullptr)
        {
            continue;
        }
        *equals = '\0';
        if (entry == nullptr && is_key(line, "version"))
        {
            version_ok = std::strcmp(equals + 1, CACHE_VERSION) == 0;
        }
        else if (entry != nullptr)
        {
            set_field(entry, line, equals + 1);
        }
    }
    delete[] data;
    if (!version_ok)
    {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Core catalog: %s is from another version, every core is peeked again",
                    catalog->cache_path);
        for (int32_t i = 0; i < catalog->count; i++)
        {
            catalog->entries[i] = CoreCatalogEntry{};
        }
        catalog->count = 0;
    }
}

// Appends one formatted line (without the newline) to `text`.
static void add_line(std::string* text, const char* key, const char* value)
{
    *text += key;
    *text += '=';
    *text += value;
    *text += '\n';
}

static void add_number(std::string* text, const char* key, unsigned long long value)
{
    char number[32] = {};
    std::snprintf(number, sizeof(number), "%llu", value);
    add_line(text, key, number);
}

static void add_subsystems(std::string* text, const LibretroSubsystems* subsystems)
{
    for (size_t i = 0; i < subsystems->list.size(); i++)
    {
        const LibretroSubsystem* subsystem = &subsystems->list[i];
        add_line(text, "subsystem", subsystem->ident);
        add_number(text, "subsystem.id", subsystem->id);
        add_line(text, "subsystem.description", subsystem->description);
        for (size_t r = 0; r < subsystem->roms.size(); r++)
        {
            const LibretroSubsystemRom* rom = &subsystem->roms[r];
            char flags[4] = { rom->required ? '1' : '0', rom->need_fullpath ? '1' : '0', rom->block_extract ? '1' : '0', '\0' };
            add_line(text, "subsystem.rom", rom->description);
            add_line(text, "subsystem.rom.extensions", rom->extensions);
            add_line(text, "subsystem.rom.flags", flags);
            for (uint32_t m = 0; m < rom->memory_count; m++)
            {
                char memory[64] = {};
                std::snprintf(memory, sizeof(memory), "0x%x %s", rom->memory[m].type, rom->memory[m].extension);
                add_line(text, "subsystem.rom.memory", memory);
            }
        }
    }
}

static void save_cache(const CoreCatalog* catalog)
{
    std::string text;
    add_line(&text, "version", CACHE_VERSION);
    text += '\n';
    for (int32_t i = 0; i < catalog->count; i++)
    {
        const CoreCatalogEntry* entry = &catalog->entries[i];
        const LibretroCoreInfo* info = &entry->info;
        text += '[';
        text += entry->file_name;
        text += "]\n";
        add_number(&text, "size", static_cast<unsigned long long>(entry->size));
        char mtime[32] = {};
        std::snprintf(mtime, sizeof(mtime), "%lld", static_cast<long long>(entry->mtime));
        add_line(&text, "mtime", mtime);
        add_number(&text, "usable", info->usable ? 1 : 0);
        add_line(&text, "error", info->error);
        add_line(&text, "name", info->name);
        add_line(&text, "version", info->version);
        add_line(&text, "extensions", info->extensions);
        add_number(&text, "need_fullpath", info->need_fullpath ? 1 : 0);
        add_number(&text, "block_extract", info->block_extract ? 1 : 0);
        add_number(&text, "supports_no_game", info->supports_no_game ? 1 : 0);
        add_number(&text, "option_count", info->option_count);
        add_number(&text, "options_api", info->options_api);
        add_number(&text, "api_version", info->api_version);
        add_subsystems(&text, &info->subsystems);
        text += '\n';
    }
    host_fs_write_file(catalog->cache_path, text.data(), text.size());
}

// ---------------------------------------------------------------------------
// Scan
// ---------------------------------------------------------------------------

void core_catalog_init(CoreCatalog* catalog, const char* directory, const char* cache_path)
{
    copy_text(catalog->directory, sizeof(catalog->directory), directory);
    copy_text(catalog->cache_path, sizeof(catalog->cache_path), cache_path);
    load_cache(catalog);
}

// The running core's info, from its host (it can't be peeked: same library).
static void info_from_running_host(const LibretroHost* host, LibretroCoreInfo* info)
{
    *info = LibretroCoreInfo{};
    info->usable = true;
    copy_text(info->name, sizeof(info->name), host->system_info.library_name);
    copy_text(info->version, sizeof(info->version), host->system_info.library_version);
    copy_text(info->extensions, sizeof(info->extensions), host->system_info.valid_extensions);
    info->need_fullpath = host->system_info.need_fullpath;
    info->block_extract = host->system_info.block_extract;
    info->supports_no_game = host->supports_no_game;
    info->option_count = host->options.count;
    info->options_api = host->options.api_used;
    info->api_version = RETRO_API_VERSION;
    info->subsystems = host->subsystems;
}

void core_catalog_scan(CoreCatalog* catalog, const LibretroHost* running)
{
    catalog->peeked_last_scan = 0;
    int file_count = 0;
    char** names = SDL_GlobDirectory(catalog->directory, "*", 0, &file_count);

    CoreCatalogEntry* found = new CoreCatalogEntry[CORE_CATALOG_MAX];
    int32_t found_count = 0;
    bool changed = false;

    for (int i = 0; names != nullptr && i < file_count && found_count < CORE_CATALOG_MAX; i++)
    {
        CoreCatalogEntry* entry = &found[found_count];
        if (!is_core_file(names[i]) || std::strlen(names[i]) >= sizeof(entry->file_name))
        {
            continue;
        }
        *entry = CoreCatalogEntry{};
        copy_text(entry->file_name, sizeof(entry->file_name), names[i]);

        char path[LIBRETRO_PATH_MAX] = {};
        SDL_PathInfo file = {};
        if (!core_catalog_path(catalog, entry, path, sizeof(path)) || !SDL_GetPathInfo(path, &file))
        {
            continue;
        }
        entry->size = file.size;
        entry->mtime = file.modify_time;
        found_count++;

        // Cached and unchanged: nothing to load.
        bool cached = false;
        for (int32_t j = 0; j < catalog->count; j++)
        {
            const CoreCatalogEntry* old = &catalog->entries[j];
            if (std::strcmp(old->file_name, entry->file_name) == 0 && old->size == entry->size && old->mtime == entry->mtime)
            {
                entry->info = old->info;
                cached = true;
                break;
            }
        }
        if (cached)
        {
            continue;
        }

        changed = true;
        bool is_running = running != nullptr && running->core.library.handle != nullptr &&
                          std::strcmp(running->core_path, path) == 0;
        if (is_running)
        {
            info_from_running_host(running, &entry->info);
        }
        else
        {
            libretro_host_peek_core(path, &entry->info);
            catalog->peeked_last_scan++;
            // The host refused: it's loaded under another spelling of its path.
            // If it's the running core, its host has the real info.
            if (entry->info.already_loaded && running != nullptr && running->core.library.handle != nullptr)
            {
                info_from_running_host(running, &entry->info);
            }
            // Never cache "already loaded": once it's unloaded, the next scan must peek it.
            if (entry->info.already_loaded)
            {
                entry->mtime = -1;
            }
        }
    }
    if (names != nullptr)
    {
        SDL_free(names);
    }

    // Cores removed from the folder also change the cache.
    changed = changed || found_count != catalog->count;

    sort_entries(found, found_count);
    for (int32_t i = 0; i < found_count; i++)
    {
        catalog->entries[i] = found[i];
    }
    catalog->count = found_count;
    delete[] found;

    if (changed)
    {
        save_cache(catalog);
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Core catalog: %d cores, %d peeked, cache written to %s",
                    catalog->count, catalog->peeked_last_scan, catalog->cache_path);
    }
}

void core_catalog_refresh(CoreCatalog* catalog, const LibretroHost* running)
{
    catalog->count = 0;   // nothing cached: every core gets peeked
    core_catalog_scan(catalog, running);
}
