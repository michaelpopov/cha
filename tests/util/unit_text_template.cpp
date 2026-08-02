#include "util/text_template.h"
#include "util/utf8_path.h"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace cha {
namespace {

std::filesystem::path unique_root(std::string_view label) {
    return std::filesystem::temp_directory_path()
        / ("cha_template_" + std::string(label) + "_"
           + std::to_string(
               std::chrono::steady_clock::now().time_since_epoch().count()));
}

// Removes the directory on destruction so ASSERT_*/throws cannot skip cleanup.
class TempDir {
public:
    explicit TempDir(std::string_view label)
        : path_(unique_root(label)) {}

    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    ~TempDir() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    const std::filesystem::path& path() const {
        return path_;
    }

private:
    std::filesystem::path path_;
};

void write_file(const std::filesystem::path& path, std::string_view contents) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream file(path, std::ios::binary);
    file << contents;
}

TemplateOptions options_for(const std::filesystem::path& root) {
    return TemplateOptions{.containment_root = root};
}

std::string expand_in(
    const std::filesystem::path& root,
    const std::filesystem::path& path,
    TemplateOptions options = {}) {
    options.containment_root = root;
    return expand_template_file(path, options);
}

TEST(TextTemplate, PassesLiteralsAndPlainDollarsThrough) {
    const TempDir root("literal");
    const std::filesystem::path file = root.path() / "a.md";
    write_file(file, "price $5 and $$x and end$\n$$");
    EXPECT_EQ(expand_in(root.path(), file), "price $5 and $$x and end$\n$$");
}

TEST(TextTemplate, EscapesDollarDollarWithThirdDollar) {
    const TempDir root("escape");
    const std::filesystem::path file = root.path() / "a.md";
    write_file(file, "Send $$$(this) through literally.");
    EXPECT_EQ(expand_in(root.path(), file), "Send $$(this) through literally.");
}

TEST(TextTemplate, ExpandsVariablesAndTrimsBodies) {
    const TempDir root("vars");
    const std::filesystem::path file = root.path() / "a.md";
    write_file(file, "Hello $${ name }!");
    TemplateOptions options = options_for(root.path());
    options.initial_scope = {{"name", "Seneca"}};
    EXPECT_EQ(expand_template_file(file, options), "Hello Seneca!");
}

TEST(TextTemplate, HandlesMacrosAtEdgesAndUtf8Neighbors) {
    const TempDir root("edges");
    const std::filesystem::path file = root.path() / "a.md";
    write_file(file, "$${a}\xE2\x9C\x93$${b}");
    TemplateOptions options = options_for(root.path());
    options.initial_scope = {{"a", "X"}, {"b", "Y"}};
    EXPECT_EQ(expand_template_file(file, options), "X\xE2\x9C\x93Y");
}

TEST(TextTemplate, RejectsUnterminatedAndEmptyMacros) {
    const TempDir root("badmacro");
    {
        const std::filesystem::path file = root.path() / "nl.md";
        write_file(file, "$$(foo\n)");
        EXPECT_THROW((void)expand_in(root.path(), file), std::runtime_error);
    }
    {
        const std::filesystem::path file = root.path() / "eof.md";
        write_file(file, "$${name");
        try {
            (void)expand_in(root.path(), file);
            FAIL();
        } catch (const std::runtime_error& error) {
            EXPECT_NE(
                std::string(error.what()).find("unterminated macro"),
                std::string::npos);
        }
    }
    {
        const std::filesystem::path file = root.path() / "empty.md";
        write_file(file, "$${   }");
        try {
            (void)expand_in(root.path(), file);
            FAIL();
        } catch (const std::runtime_error& error) {
            EXPECT_NE(
                std::string(error.what()).find("empty variable name"),
                std::string::npos);
        }
    }
    {
        const std::filesystem::path file = root.path() / "badname.md";
        write_file(file, "$${has space}");
        try {
            (void)expand_in(root.path(), file);
            FAIL();
        } catch (const std::runtime_error& error) {
            EXPECT_NE(
                std::string(error.what()).find("invalid variable name"),
                std::string::npos);
        }
    }
}

TEST(TextTemplate, IncludesRelativeToIncludingFileVerbatim) {
    const TempDir root("include");
    write_file(root.path() / "shared" / "part.md", "PART");
    write_file(
        root.path() / "dir" / "main.md",
        "A$$(../shared/part.md)B\n$$(../shared/part.md)");
    EXPECT_EQ(
        expand_in(root.path(), root.path() / "dir" / "main.md"),
        "APARTB\nPART");
}

TEST(TextTemplate, AllowsSameFileTwiceAndNestedIncludes) {
    const TempDir root("nested");
    write_file(root.path() / "c.md", "C");
    write_file(root.path() / "b.md", "B$$(c.md)");
    write_file(root.path() / "a.md", "$$(b.md)+$$(b.md)");
    EXPECT_EQ(expand_in(root.path(), root.path() / "a.md"), "BC+BC");
}

TEST(TextTemplate, RejectsMissingAbsoluteEscapingAndDirectoryTargets) {
    const TempDir root("reject");
    std::filesystem::create_directories(root.path() / "sub");
    {
        write_file(root.path() / "missing.md", "$$(nope.md)");
        try {
            (void)expand_in(root.path(), root.path() / "missing.md");
            FAIL();
        } catch (const std::runtime_error& error) {
            EXPECT_NE(
                std::string(error.what()).find("cannot read included file"),
                std::string::npos);
        }
    }
    {
        write_file(root.path() / "abs.md", "$$(/etc/passwd)");
        try {
            (void)expand_in(root.path(), root.path() / "abs.md");
            FAIL();
        } catch (const std::runtime_error& error) {
            EXPECT_NE(
                std::string(error.what()).find("include path must be relative"),
                std::string::npos);
        }
    }
    {
        write_file(root.path() / "escape.md", "$$(../outside.md)");
        write_file(root.path().parent_path() / "outside.md", "x");
        try {
            (void)expand_in(root.path(), root.path() / "escape.md");
            FAIL();
        } catch (const std::runtime_error& error) {
            EXPECT_NE(
                std::string(error.what()).find("include path escapes the forum"),
                std::string::npos);
        }
        std::filesystem::remove(root.path().parent_path() / "outside.md");
    }
    {
        write_file(root.path() / "dirtarget.md", "$$(sub)");
        try {
            (void)expand_in(root.path(), root.path() / "dirtarget.md");
            FAIL();
        } catch (const std::runtime_error& error) {
            EXPECT_NE(
                std::string(error.what()).find(
                    "included path is not a regular file"),
                std::string::npos);
        }
    }
}

TEST(TextTemplate, RejectsSiblingForumAndPrefixSiblingNames) {
    const TempDir workspace("forums");
    const std::filesystem::path stoics = workspace.path() / "stoics";
    const std::filesystem::path evil = workspace.path() / "stoics-evil";
    const std::filesystem::path other = workspace.path() / "lobby";
    std::filesystem::create_directories(stoics);
    std::filesystem::create_directories(evil);
    std::filesystem::create_directories(other);
    write_file(evil / "secret.md", "SECRET");
    write_file(other / "x.md", "OTHER");
    write_file(stoics / "a.md", "$$(../stoics-evil/secret.md)");
    write_file(stoics / "b.md", "$$(../lobby/x.md)");

    try {
        (void)expand_in(stoics, stoics / "a.md");
        FAIL();
    } catch (const std::runtime_error& error) {
        EXPECT_NE(
            std::string(error.what()).find("escapes the forum"),
            std::string::npos);
    }
    try {
        (void)expand_in(stoics, stoics / "b.md");
        FAIL();
    } catch (const std::runtime_error& error) {
        EXPECT_NE(
            std::string(error.what()).find("escapes the forum"),
            std::string::npos);
    }
}

TEST(TextTemplate, AcceptsNonCanonicalContainmentRoot) {
    const TempDir root("noncanon");
    std::filesystem::create_directories(root.path() / "forum");
    write_file(root.path() / "forum" / "a.md", "OK");
    const std::filesystem::path noncanonical = root.path() / "forum" / "." / ".";
    EXPECT_EQ(
        expand_template_file(
            root.path() / "forum" / "a.md",
            TemplateOptions{.containment_root = noncanonical}),
        "OK");
}

#ifndef _WIN32
TEST(TextTemplate, RejectsSymlinkEscape) {
    const TempDir root("symlink");
    const TempDir outside("symlink_out");
    write_file(outside.path() / "secret.md", "SECRET");
    write_file(root.path() / "a.md", "$$(link.md)");
    std::filesystem::create_symlink(
        outside.path() / "secret.md", root.path() / "link.md");
    try {
        (void)expand_in(root.path(), root.path() / "a.md");
        FAIL();
    } catch (const std::runtime_error& error) {
        EXPECT_NE(
            std::string(error.what()).find("escapes the forum"),
            std::string::npos);
    }
}

TEST(TextTemplate, RejectsRootTemplateSymlinkEscape) {
    const TempDir root("root_symlink");
    const TempDir outside("root_symlink_out");
    write_file(outside.path() / "SYSTEM.md", "SECRET");
    std::filesystem::create_directories(root.path());
    std::filesystem::create_symlink(
        outside.path() / "SYSTEM.md", root.path() / "SYSTEM.md");

    try {
        (void)expand_in(root.path(), root.path() / "SYSTEM.md");
        FAIL();
    } catch (const std::runtime_error& error) {
        EXPECT_NE(
            std::string(error.what()).find(
                "template path escapes containment root"),
            std::string::npos);
    }
}

TEST(TextTemplate, RejectsDirectoryScopeSymlinkEscape) {
    const TempDir root("scope_symlink");
    const TempDir outside("scope_symlink_out");
    write_file(
        outside.path() / "config.toml",
        "[prompt]\nsecret = \"outside\"\n");
    write_file(root.path() / "scoped" / "a.md", "$${secret}");
    std::filesystem::create_symlink(
        outside.path() / "config.toml",
        root.path() / "scoped" / "config.toml");

    try {
        (void)expand_in(root.path(), root.path() / "scoped" / "a.md");
        FAIL();
    } catch (const std::runtime_error& error) {
        const std::string message = error.what();
        EXPECT_NE(
            message.find("scope file escapes containment root"),
            std::string::npos);
        EXPECT_NE(message.find("in scoped/a.md:1:1"), std::string::npos);
    }
}
#endif

TEST(TextTemplate, DetectsCyclesWithChain) {
    const TempDir root("cycle");
    write_file(root.path() / "a.md", "$$(b.md)");
    write_file(root.path() / "b.md", "$$(a.md)");
    try {
        (void)expand_in(root.path(), root.path() / "a.md");
        FAIL();
    } catch (const std::runtime_error& error) {
        const std::string message = error.what();
        EXPECT_NE(message.find("include cycle"), std::string::npos);
        EXPECT_NE(message.find("a.md"), std::string::npos);
        EXPECT_NE(message.find("b.md"), std::string::npos);
        EXPECT_NE(message.find("->"), std::string::npos);
    }
    write_file(root.path() / "self.md", "$$(self.md)");
    EXPECT_THROW(
        (void)expand_in(root.path(), root.path() / "self.md"),
        std::runtime_error);
}

TEST(TextTemplate, EnforcesDepthOutputAndIncludeCountLimits) {
    const TempDir root("limits");

    for (int depth = 0; depth < 20; ++depth) {
        const std::filesystem::path file =
            root.path() / ("d" + std::to_string(depth) + ".md");
        if (depth == 0) {
            write_file(file, "leaf");
        } else {
            write_file(file, "$$(d" + std::to_string(depth - 1) + ".md)");
        }
    }
    TemplateOptions deep = options_for(root.path());
    deep.limits.max_include_depth = 4;
    try {
        (void)expand_template_file(root.path() / "d19.md", deep);
        FAIL();
    } catch (const std::runtime_error& error) {
        EXPECT_NE(
            std::string(error.what()).find("maximum include depth"),
            std::string::npos);
    }

    write_file(root.path() / "big.md", "$$(chunk.md)$$(chunk.md)");
    write_file(root.path() / "chunk.md", std::string(100, 'x'));
    TemplateOptions sized = options_for(root.path());
    sized.limits.max_output_bytes = 150;
    try {
        (void)expand_template_file(root.path() / "big.md", sized);
        FAIL();
    } catch (const std::runtime_error& error) {
        EXPECT_NE(
            std::string(error.what()).find("expanded prompt exceeds"),
            std::string::npos);
    }

    // Depth-3 chain, true size 100. Limit 250 is above the real size but below
    // what a double-charge-per-level implementation would count (100 * 3 = 300).
    write_file(root.path() / "leaf.md", std::string(100, 'y'));
    write_file(root.path() / "n2.md", "$$(leaf.md)");
    write_file(root.path() / "n1.md", "$$(n2.md)");
    write_file(root.path() / "n0.md", "$$(n1.md)");
    TemplateOptions nested_budget = options_for(root.path());
    nested_budget.limits.max_output_bytes = 250;
    EXPECT_EQ(
        expand_template_file(root.path() / "n0.md", nested_budget),
        std::string(100, 'y'));

    // Zero-output exponential fan-out — only the include count can stop it.
    write_file(root.path() / "e0.md", "");
    for (int level = 1; level <= 10; ++level) {
        const std::string prev = "e" + std::to_string(level - 1) + ".md";
        write_file(
            root.path() / ("e" + std::to_string(level) + ".md"),
            "$$(" + prev + ")$$(" + prev + ")");
    }
    TemplateOptions counted = options_for(root.path());
    counted.limits.max_includes = 32;
    try {
        (void)expand_template_file(root.path() / "e10.md", counted);
        FAIL();
    } catch (const std::runtime_error& error) {
        EXPECT_NE(
            std::string(error.what()).find("maximum include count"),
            std::string::npos);
    }
}

TEST(TextTemplate, LoadTemplateScopeScalarsAndRejections) {
    const TempDir root("scope");
    EXPECT_EQ(
        load_template_scope(root.path() / "missing.toml", "prompt"),
        std::nullopt);

    write_file(root.path() / "empty.toml", "host = \"x\"\n");
    const std::optional<TemplateScope> absent =
        load_template_scope(root.path() / "empty.toml", "prompt");
    ASSERT_TRUE(absent);
    EXPECT_TRUE(absent->empty());

    write_file(
        root.path() / "scalars.toml",
        "[prompt]\n"
        "name = \"Seneca\"\n"
        "count = 3\n"
        "ratio = 1.5\n"
        "flag = true\n");
    const std::optional<TemplateScope> scalars =
        load_template_scope(root.path() / "scalars.toml", "prompt");
    ASSERT_TRUE(scalars);
    EXPECT_EQ(scalars->at("name"), "Seneca");
    EXPECT_EQ(scalars->at("count"), "3");
    EXPECT_EQ(scalars->at("ratio"), "1.5");
    EXPECT_EQ(scalars->at("flag"), "true");

    write_file(root.path() / "array.toml", "[prompt]\nitems = [1, 2]\n");
    EXPECT_THROW(
        (void)load_template_scope(root.path() / "array.toml", "prompt"),
        std::runtime_error);

    write_file(root.path() / "nested.toml", "[prompt.child]\nx = 1\n");
    EXPECT_THROW(
        (void)load_template_scope(root.path() / "nested.toml", "prompt"),
        std::runtime_error);

    write_file(
        root.path() / "inline_table.toml",
        "[prompt]\npoint = { x = 1, y = 2 }\n");
    EXPECT_THROW(
        (void)load_template_scope(root.path() / "inline_table.toml", "prompt"),
        std::runtime_error);

    write_file(root.path() / "date.toml", "[prompt]\nwhen = 2020-01-01\n");
    EXPECT_THROW(
        (void)load_template_scope(root.path() / "date.toml", "prompt"),
        std::runtime_error);

    write_file(root.path() / "time.toml", "[prompt]\nwhen = 12:30:00\n");
    EXPECT_THROW(
        (void)load_template_scope(root.path() / "time.toml", "prompt"),
        std::runtime_error);

    write_file(root.path() / "not_table.toml", "prompt = \"hello\"\n");
    try {
        (void)load_template_scope(root.path() / "not_table.toml", "prompt");
        FAIL();
    } catch (const std::runtime_error& error) {
        EXPECT_NE(
            std::string(error.what()).find("'prompt' must be a table"),
            std::string::npos)
            << error.what();
    }

    // TOML quoting does not affect semantic keys. Decoded names inside the
    // variable grammar remain addressable; names outside it are skipped.
    write_file(
        root.path() / "quoted.toml",
        "[prompt]\n"
        "ok = \"yes\"\n"
        "\"quoted_ok\" = \"also yes\"\n"
        "\"my key\" = \"no\"\n");
    const std::optional<TemplateScope> quoted =
        load_template_scope(root.path() / "quoted.toml", "prompt");
    ASSERT_TRUE(quoted);
    EXPECT_EQ(quoted->at("ok"), "yes");
    EXPECT_EQ(quoted->at("quoted_ok"), "also yes");
    EXPECT_EQ(quoted->find("my key"), quoted->end());

    write_file(root.path() / "bad.toml", "[prompt\n");
    try {
        (void)load_template_scope(root.path() / "bad.toml", "prompt");
        FAIL();
    } catch (const std::runtime_error& error) {
        EXPECT_NE(
            std::string(error.what()).find("cannot parse"),
            std::string::npos);
    }
}

TEST(TextTemplate, ScopeLookupOrderAndNoLeakOrReexpand) {
    const TempDir root("lookup");
    write_file(
        root.path() / "character" / "config.toml",
        "[prompt]\n"
        "register = \"energetic\"\n"
        "shared = \"from-character\"\n");
    write_file(
        root.path() / "inner" / "config.toml",
        "[prompt]\nshared = \"from-inner\"\n");
    write_file(
        root.path() / "inner" / "part.md",
        "$${shared}|$${register}|$${character.display_name}");
    write_file(
        root.path() / "character" / "SYSTEM.md",
        "before=$${shared}\n$$(../inner/part.md)\nafter=$${shared}\nmacro=$${raw}");

    TemplateOptions options = options_for(root.path());
    options.reserved = {{"character.display_name", "Seneca"}};
    options.initial_scope = {
        {"register", "measured"},
        {"shared", "from-initial"},
        {"raw", "$$(nope)$${x}"},
    };

    const std::string expanded =
        expand_template_file(root.path() / "character" / "SYSTEM.md", options);
    EXPECT_EQ(
        expanded,
        "before=from-character\nfrom-inner|energetic|Seneca\nafter=from-character\nmacro=$$(nope)$${x}");

    write_file(root.path() / "res" / "config.toml", "[prompt]\nname = \"file\"\n");
    write_file(root.path() / "res" / "a.md", "$${name}");
    TemplateOptions reserved_options = options_for(root.path());
    reserved_options.reserved = {{"name", "reserved"}};
    reserved_options.initial_scope = {{"name", "initial"}};
    EXPECT_EQ(
        expand_template_file(root.path() / "res" / "a.md", reserved_options),
        "reserved");

    write_file(root.path() / "unknown.md", "$${missing}");
    try {
        (void)expand_in(root.path(), root.path() / "unknown.md");
        FAIL();
    } catch (const std::runtime_error& error) {
        EXPECT_NE(
            std::string(error.what()).find("unknown variable"),
            std::string::npos);
    }
}

TEST(TextTemplate, DiagnosticsUseRelativePathsAndIncludeChain) {
    const TempDir root("diag");
    write_file(root.path() / "shared.md", "$${registr}");
    write_file(
        root.path() / "characters" / "seneca" / "SYSTEM.md",
        "$$(../../shared.md)");
    try {
        (void)expand_in(
            root.path(), root.path() / "characters" / "seneca" / "SYSTEM.md");
        FAIL();
    } catch (const std::runtime_error& error) {
        const std::string message = error.what();
        EXPECT_NE(message.find("unknown variable 'registr'"), std::string::npos);
        EXPECT_NE(message.find("shared.md"), std::string::npos);
        EXPECT_NE(message.find("included from"), std::string::npos);
        EXPECT_NE(message.find("SYSTEM.md"), std::string::npos);
        EXPECT_EQ(message.find(utf8_path(root.path())), std::string::npos)
            << message;
    }

    write_file(root.path() / "bad_dir" / "config.toml", "[prompt\n");
    write_file(root.path() / "bad_dir" / "a.md", "x");
    write_file(root.path() / "caller.md", "$$(bad_dir/a.md)");
    try {
        (void)expand_in(root.path(), root.path() / "caller.md");
        FAIL();
    } catch (const std::runtime_error& error) {
        const std::string message = error.what();
        EXPECT_NE(message.find("cannot parse"), std::string::npos);
        EXPECT_NE(message.find("bad_dir/config.toml"), std::string::npos)
            << message;
        EXPECT_NE(message.find("in bad_dir/a.md:1:1"), std::string::npos)
            << message;
        EXPECT_NE(message.find("included from"), std::string::npos);
        EXPECT_EQ(message.find(utf8_path(root.path())), std::string::npos)
            << message;
    }
}

TEST(TextTemplate, TruncatesIncludeBodyAtFirstCloser) {
    const TempDir root("closer");
    write_file(root.path() / "a.md", "$$(no)pe.md)");
    try {
        (void)expand_in(root.path(), root.path() / "a.md");
        FAIL();
    } catch (const std::runtime_error& error) {
        EXPECT_NE(
            std::string(error.what()).find("cannot read included file"),
            std::string::npos);
    }
}

} // namespace
} // namespace cha
