#include "syntwine/detail/platform/tokenizer.hpp"
#include "syntwine/detail/platform/utf8.hpp"
#include "syntwine/detail/platform/writer.hpp"

#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace {
using namespace syntwine;
using namespace std::string_view_literals;

void
check(bool condition, std::string_view why) {
    if (!condition)
        throw std::runtime_error(std::string(why));
}

void
test_encoding() {
    check(utf8_characters("Hello, 世界🌍!\r\n") == 13, "UTF-8 counts code points");
    check(utf8_characters("a\0b"sv) == 3, "NUL is a code point");
    check(!utf8_characters("\xc0\x80"), "overlong UTF-8");
    check(!utf8_characters("\xed\xa0\x80"), "surrogate UTF-8");
    check(!utf8_characters("\xf4\x90\x80\x80"), "out-of-range UTF-8");
    check(!utf8_characters("\xf0\x9f\x8c"), "truncated UTF-8");

    constexpr std::array names{"o200k_base", "cl100k_base", "p50k_base", "r50k_base"};
    constexpr std::array multilingual{6, 10, 13, 13};
    constexpr std::array long_word{513, 513, 1025, 1025};
    constexpr std::array spaces{79, 79, 625, 10000};
    for (std::size_t i = 0; i < names.size(); ++i) {
        check(token_count("", names[i]) == 0, names[i]);
        check(token_count("Hello, 世界🌍!\r\n", names[i]) == multilingual[i], names[i]);
        check(token_count("<|endoftext|>", names[i]) == 7, names[i]);
        check(token_count(std::string(4097, 'a'), names[i]) == long_word[i], names[i]);
        check(token_count(std::string(10000, ' '), names[i]) == spaces[i], names[i]);
    }
}

void
test_writer(const std::filesystem::path& directory) {
    const auto path = directory / std::filesystem::path(u8"产物.bin");
    atomic_write(path, "initial");
    atomic_write(path, "\x00\xff\r\n"sv);
    {
        std::ifstream contents(path, std::ios::binary);
        const std::string actual{std::istreambuf_iterator<char>(contents), {}};
        check(actual == "\x00\xff\r\n"sv, "atomic write must replace the full binary file");
    }
    std::filesystem::remove(path);
}
} // namespace

#ifdef _WIN32
int
wmain(int argc, wchar_t* argv[]) {
#else
int
main(int argc, char* argv[]) {
#endif
    if (argc != 3)
        return 2;
    try {
        set_tokenizer_data_dir(std::filesystem::path(argv[1]));
        test_encoding();
        test_writer(std::filesystem::path(argv[2]));
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
