#include "core_index.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>

#include "host_log.h"
#include "settings_file.h"

const char* core_index_build_target()
{
    return MULTITAP_CORE_TARGET;
}

static std::string value_of(const SettingsFile* file, const char* section, const char* key)
{
    const char* value = settings_file_get(file, section, key);
    return value != nullptr ? std::string(value) : std::string();
}

// A name a core may be installed under: no folders, no "..", nothing hidden.
// The index is signed, but a mistake in it must still never write outside
// the cores folder.
static bool plain_file_name(const std::string& name)
{
    if (name.empty() || name[0] == '.')
    {
        return false;
    }
    for (size_t i = 0; i < name.size(); i++)
    {
        char c = name[i];
        bool allowed = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                       c == '_' || c == '-' || c == '.';
        if (!allowed)
        {
            return false;
        }
    }
    return true;
}

static bool is_sha256(const std::string& text)
{
    if (text.size() != 64)
    {
        return false;
    }
    for (size_t i = 0; i < text.size(); i++)
    {
        char c = text[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
        {
            return false;
        }
    }
    return true;
}

static int64_t to_size(const std::string& text)
{
    if (text.empty())
    {
        return 0;
    }
    char* end = nullptr;
    long long value = std::strtoll(text.c_str(), &end, 10);
    if (end == nullptr || *end != '\0' || value < 0)
    {
        return 0;
    }
    return static_cast<int64_t>(value);
}

static bool entry_name_less(const CoreIndexEntry& a, const CoreIndexEntry& b)
{
    return a.name < b.name;
}

bool core_index_parse(CoreIndex* index, const char* text, size_t size, const char* target)
{
    *index = {};
    SettingsFile file = {};
    settings_file_parse(&file, text, size, "core index");
    index->release = value_of(&file, "", "release");
    index->target  = value_of(&file, "", "target");
    if (index->target != target)
    {
        host_log(LogLevel::error, "core index: for %s, not %s", index->target.c_str(), target);
        return false;
    }

    std::string prefix = std::string(target) + "/";
    for (size_t i = 0; i < file.sections.size(); i++)
    {
        const char* section = file.sections[i].name.c_str();
        if (file.sections[i].name.empty())
        {
            continue;
        }
        if (file.sections[i].name.compare(0, prefix.size(), prefix) != 0)
        {
            host_log(LogLevel::warning, "core index: [%s] isn't for %s, skipped", section, target);
            continue;
        }
        CoreIndexEntry entry;
        entry.core           = value_of(&file, section, "core");
        entry.name           = value_of(&file, section, "name");
        entry.file           = value_of(&file, section, "file");
        entry.version        = value_of(&file, section, "version");
        entry.commit         = value_of(&file, section, "commit");
        entry.license        = value_of(&file, section, "license");
        entry.free           = value_of(&file, section, "free") == "true";
        entry.systems        = value_of(&file, section, "systems");
        entry.url            = value_of(&file, section, "url");
        entry.sha256         = value_of(&file, section, "sha256");
        entry.size           = to_size(value_of(&file, section, "size"));
        entry.library_sha256 = value_of(&file, section, "library_sha256");
        entry.library_size   = to_size(value_of(&file, section, "library_size"));
        entry.source_url     = value_of(&file, section, "source_url");

        if (entry.name.empty() || entry.url.empty() || entry.commit.empty() || entry.size <= 0 ||
            !plain_file_name(entry.file) || !is_sha256(entry.sha256) || !is_sha256(entry.library_sha256))
        {
            host_log(LogLevel::warning, "core index: [%s] is incomplete or invalid, skipped", section);
            continue;
        }
        if (core_index_find_file(index, entry.file.c_str()) >= 0)
        {
            host_log(LogLevel::warning, "core index: [%s] repeats %s, skipped", section, entry.file.c_str());
            continue;
        }
        index->entries.push_back(entry);
    }
    std::sort(index->entries.begin(), index->entries.end(), entry_name_less);
    return true;
}

int32_t core_index_find_file(const CoreIndex* index, const char* file)
{
    for (size_t i = 0; i < index->entries.size(); i++)
    {
        if (index->entries[i].file == file)
        {
            return static_cast<int32_t>(i);
        }
    }
    return -1;
}
