/// @file test_config.cpp
/// @brief Test configuration implementation.

#include <kernel/test/test_config.hpp>
#include <initrd/initrd.hpp>
#include <string.hpp>
#include <logger.hpp>
#include <kernel/memory/mempool.hpp>

namespace kernel::test {

static const char *g_test_classes[32] = {nullptr};
static size_t g_test_class_count = 0;

void parse_test_config(const char *path) {
    g_test_class_count = 0;

    initrd::InitrdFile f = initrd::find(path);
    if (!f.data) {
        Logger::info("[TEST] Config file '%s' not found, defaulting to 'safe'",
                     path);
        g_test_classes[0] = "safe";
        g_test_class_count = 1;
        return;
    }

    const char *data = reinterpret_cast<const char *>(f.data);
    size_t len = f.size;
    size_t pos = 0;

    while (pos < len && g_test_class_count < 32) {
        while (pos < len && (data[pos] == ' ' || data[pos] == '\t' ||
                             data[pos] == '\r' || data[pos] == '\n')) {
            ++pos;
        }
        if (pos >= len)
            break;

        if (data[pos] == '#') {
            while (pos < len && data[pos] != '\n')
                ++pos;
            continue;
        }

        size_t start = pos;
        while (pos < len && data[pos] != '\n' && data[pos] != '\r' &&
               data[pos] != ' ' && data[pos] != '\t') {
            ++pos;
        }
        size_t name_len = pos - start;
        if (name_len == 0)
            continue;

        char *name_buf = reinterpret_cast<char *>(MemPool::alloc(name_len + 1));
        if (!name_buf)
            break;

        __builtin_memcpy(name_buf, data + start, name_len);
        name_buf[name_len] = '\0';

        g_test_classes[g_test_class_count++] = name_buf;

        while (pos < len && (data[pos] == '\n' || data[pos] == '\r'))
            ++pos;
    }

    if (g_test_class_count == 0) {
        Logger::warn("[TEST] Config file '%s' empty, defaulting to 'safe'",
                     path);
        g_test_classes[0] = "safe";
        g_test_class_count = 1;
    } else if (g_test_class_count != 1 ||
                 strcmp(g_test_classes[0], "none") != 0) {
        // Skip the listing when no test class will run (issue #244):
        // class "none" means interactive shell, not a test session.
        Logger::info("[TEST] Loaded %u test class(es) from '%s'",
                     (unsigned)g_test_class_count, path);
        for (size_t i = 0; i < g_test_class_count; ++i) {
            Logger::info("[TEST]   Class: %s", g_test_classes[i]);
        }
    }
}

const char **get_test_classes() {
    return g_test_classes;
}

size_t get_test_class_count() {
    return g_test_class_count;
}

bool should_run_class(const char *class_name) {
    for (size_t i = 0; i < g_test_class_count; ++i) {
        if (strcmp(g_test_classes[i], class_name) == 0) {
            return true;
        }
    }
    return false;
}

} // namespace kernel::test