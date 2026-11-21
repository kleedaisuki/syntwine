#include "syntwine/detail/platform/tokenizer.hpp"

#include <algorithm>
#include <bit>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <queue>
#include <stdexcept>
#include <vector>

#define PCRE2_CODE_UNIT_WIDTH 8
#include <pcre2.h>

namespace syntwine {
namespace {

using Rank = std::uint32_t;
constexpr Rank unranked = std::numeric_limits<Rank>::max();

std::filesystem::path tokenizer_directory;
std::mutex tokenizer_directory_lock;

std::filesystem::path
vocabulary_path(std::string_view name) {
    const std::lock_guard lock(tokenizer_directory_lock);
    return tokenizer_directory / (std::string(name) + ".bin");
}

std::string
read_vocabulary(std::string_view name) {
    std::ifstream input(vocabulary_path(name), std::ios::binary | std::ios::ate);
    if (!input)
        throw std::runtime_error(
            "cannot open installed tokenizer vocabulary: " + std::string(name)
        );
    const auto size = static_cast<std::streamoff>(input.tellg());
    if (size == 0)
        throw std::runtime_error("empty tokenizer vocabulary: " + std::string(name));
    if (size < 0
        || size > std::numeric_limits<std::uint32_t>::max()
        || size > std::numeric_limits<std::streamsize>::max())
        throw std::runtime_error(
            "invalid installed tokenizer vocabulary size: " + std::string(name)
        );
    std::string text(static_cast<std::size_t>(size), '\0');
    input.seekg(0);
    if (!input.read(text.data(), static_cast<std::streamsize>(text.size())))
        throw std::runtime_error(
            "cannot read installed tokenizer vocabulary: " + std::string(name)
        );
    return text;
}

constexpr std::string_view r50k_pattern =
    R"('(?:[sdmt]|ll|ve|re)| ?\p{L}++| ?\p{N}++| ?[^\s\p{L}\p{N}]++|\s++$|\s+(?!\S)|\s)";
constexpr std::string_view cl100k_pattern =
    R"('(?i:[sdmt]|ll|ve|re)|[^\r\n\p{L}\p{N}]?+\p{L}++|\p{N}{1,3}+| ?[^\s\p{L}\p{N}]++[\r\n]*+|\s++$|(?:\h*+[\r\n])++|\s+(?!\S)|\s)";

// In the first alternative, greedy Upper* Lower+ either reaches a Ll, or
// falls back to the last character in Upper ∩ Lower. Spell out both cases
// to avoid backtracking through a whole run of uppercase letters.
constexpr std::string_view o200k_pattern =
    R"([^\r\n\p{L}\p{N}]?(?:[\p{Lu}\p{Lt}\p{Lm}\p{Lo}\p{M}]*+\p{Ll}[\p{Ll}\p{Lm}\p{Lo}\p{M}]*+|(?:[\p{Lu}\p{Lt}]*+[\p{Lm}\p{Lo}\p{M}])++)(?i:'s|'t|'re|'ve|'m|'ll|'d)?)"
    R"(|[^\r\n\p{L}\p{N}]?[\p{Lu}\p{Lt}\p{Lm}\p{Lo}\p{M}]++[\p{Ll}\p{Lm}\p{Lo}\p{M}]*+(?i:'s|'t|'re|'ve|'m|'ll|'d)?)"
    R"(|\p{N}{1,3}| ?[^\s\p{L}\p{N}]++[\r\n/]*+|(?:\h*+[\r\n])++|\s+(?!\S)|\s+)";

template <class UInt>
UInt
load_little(const char* data) noexcept {
    UInt word;
    std::memcpy(&word, data, sizeof(word));
    if constexpr (std::endian::native == std::endian::big)
        return std::byteswap(word);
    return word;
}

std::uint64_t
tail_word(std::string_view token) noexcept {
    // At most seven bytes remain: load the available 4-, 2-, and 1-byte groups.
    const auto size = token.size();
    const auto* data = token.data();
    std::uint64_t result = 0;
    if (size & 4)
        result = load_little<std::uint32_t>(data);
    if (size & 2) {
        const auto offset = size & 4;
        result |= std::uint64_t(load_little<std::uint16_t>(data + offset)) << (8 * offset);
    }
    if (size & 1) {
        const auto offset = size & 6;
        result |= std::uint64_t(static_cast<unsigned char>(data[offset])) << (8 * offset);
    }
    return result;
}

std::uint64_t
vocabulary_hash(std::string_view token) noexcept {
    // MurmurHash64A over little-endian words, with constants fixed by the index format.
    constexpr std::uint64_t mix = 0xc6a4a7935bd1e995ULL;
    constexpr unsigned shift = 47;
    auto hash = 0xc70f6907ULL ^ (static_cast<std::uint64_t>(token.size()) * mix);
    while (token.size() >= 8) {
        auto word = load_little<std::uint64_t>(token.data()) * mix;
        word ^= word >> shift;
        hash = (hash ^ (word * mix)) * mix;
        token.remove_prefix(8);
    }
    if (!token.empty())
        hash = (hash ^ tail_word(token)) * mix;
    hash ^= hash >> shift;
    hash *= mix;
    return hash ^ (hash >> shift);
}

class Vocabulary {
public:
    explicit Vocabulary(std::string_view name)
        : bytes_(read_vocabulary(name)) {
        if (bytes_.size() < header_size || !bytes_.starts_with("SVOCAB01"))
            throw std::runtime_error("unsupported tokenizer vocabulary header");
        const auto* header = bytes_.data();
        const auto capacity = load_little<std::uint32_t>(header + 8);
        const auto entries = load_little<std::uint32_t>(header + 12);
        max_length_ = load_little<std::uint32_t>(header + 16);
        const auto token_size = load_little<std::uint32_t>(header + 20);
        const auto available = bytes_.size() - header_size;
        if (!std::has_single_bit(capacity)
            || capacity > available / slot_size
            || entries > capacity / 2
            || entries < 256
            || max_length_ == 0
            || max_length_ > 255)
            throw std::runtime_error("invalid tokenizer vocabulary dimensions");
        const auto table_size = std::size_t(capacity) * slot_size;
        if (token_size != available - table_size)
            throw std::runtime_error("invalid tokenizer vocabulary size");
        token_offset_ = header_size + table_size;
        mask_ = capacity - 1;
        validate_slots(entries, token_size);
    }

    Rank
    rank_of(std::string_view token) const noexcept {
        if (token.size() > max_length_)
            return unranked;
        auto index = static_cast<std::size_t>(vocabulary_hash(token)) & mask_;
        const auto* data = bytes_.data() + token_offset_;
        while (const auto slot = load_slot(index)) {
            if (slot >> 56 == token.size()) {
                const auto offset = static_cast<std::uint32_t>(slot);
                if (std::string_view(data + offset, token.size()) == token)
                    return static_cast<Rank>(slot >> 32 & rank_mask) - 1;
            }
            index = (index + 1) & mask_;
        }
        return unranked;
    }

private:
    static constexpr std::size_t header_size = 24;
    static constexpr std::size_t slot_size = sizeof(std::uint64_t);
    static constexpr std::uint64_t rank_mask = 0xffffff;

    std::uint64_t
    load_slot(std::size_t index) const noexcept {
        return load_little<std::uint64_t>(bytes_.data() + header_size + index * slot_size);
    }

    void
    validate_slots(std::uint32_t expected, std::size_t token_size) const {
        std::size_t occupied = 0;
        for (std::size_t index = 0; index <= mask_; ++index) {
            const auto slot = load_slot(index);
            if (slot == 0)
                continue;
            const auto offset = static_cast<std::uint32_t>(slot);
            const auto length = slot >> 56;
            if ((slot >> 32 & rank_mask) == 0
                || length == 0
                || length > max_length_
                || offset > token_size
                || length > token_size - offset)
                throw std::runtime_error("invalid tokenizer vocabulary slot");
            ++occupied;
        }
        if (occupied != expected)
            throw std::runtime_error("invalid tokenizer vocabulary entry count");
    }

    std::string bytes_;
    std::size_t token_offset_{}, mask_{}, max_length_{};
};

using Pattern = std::unique_ptr<pcre2_code, decltype(&pcre2_code_free)>;

std::string
with_unicode_whitespace(std::string_view pattern) {
    // Here \h means White_Space except CR and LF, so the newline alternative
    // can match through the last CR/LF without backtracking over a space run.
    constexpr auto other_spaces =
        R"(\x{20}\x{85}\x{a0}\x{1680}\x{2000}-\x{200a}\x{2028}\x{2029}\x{202f}\x{205f}\x{3000})";
    std::string exact;
    bool in_class = false;
    for (std::size_t i = 0; i < pattern.size(); ++i) {
        const auto c = pattern[i];
        if (c == '[' || c == ']')
            in_class = c == '[';
        if (c != '\\'
            || i + 1 == pattern.size()
            || std::string_view("sSh").find(pattern[i + 1]) == std::string_view::npos) {
            exact += c;
            continue;
        }
        const auto property = pattern[++i];
        if (!in_class)
            exact += property == 'S' ? "[^" : "[";
        exact += property == 'h' ? R"(\x{9}\x{b}\x{c})" : R"(\x{9}-\x{d})";
        exact += other_spaces;
        if (!in_class)
            exact += ']';
    }
    return exact;
}

Pattern
compile_pattern(std::string_view pattern) {
    const auto exact = with_unicode_whitespace(pattern);
    int error;
    PCRE2_SIZE offset;
    Pattern result(
        pcre2_compile(
            reinterpret_cast<PCRE2_SPTR>(exact.data()),
            exact.size(),
            PCRE2_UTF | PCRE2_UCP | PCRE2_DOLLAR_ENDONLY,
            &error,
            &offset,
            nullptr
        ),
        pcre2_code_free
    );
    if (!result)
        throw std::runtime_error("cannot compile tokenizer pretokenization pattern");
    pcre2_jit_compile(result.get(), PCRE2_JIT_COMPLETE);
    return result;
}

struct Merge {
    Rank rank;
    std::size_t left, split, end;

    bool
    operator>(const Merge& other) const {
        return rank != other.rank ? rank > other.rank : left > other.left;
    }
};

class Encoding {
public:
    Encoding(std::string_view name, std::string_view pattern)
        : pattern_(compile_pattern(pattern)),
          vocabulary_(name) {}

    std::size_t
    count(std::string_view input) const {
        if (input.empty())
            return 0;
        std::unique_ptr<pcre2_match_data, decltype(&pcre2_match_data_free)> match(
            pcre2_match_data_create_from_pattern(pattern_.get(), nullptr),
            pcre2_match_data_free
        );
        if (!match)
            throw std::bad_alloc();
        std::size_t total = 0, start = 0;
        while (start < input.size()) {
            auto status = pcre2_match(
                pattern_.get(),
                reinterpret_cast<PCRE2_SPTR>(input.data()),
                input.size(),
                start,
                PCRE2_NO_UTF_CHECK,
                match.get(),
                nullptr
            );
            if (status == PCRE2_ERROR_JIT_STACKLIMIT || status == PCRE2_ERROR_MATCHLIMIT)
                status = match_large(input, start, match.get(), status);
            if (status < 0)
                throw std::runtime_error(
                    "tokenizer pretokenization failed: " + std::to_string(status)
                );
            const auto span = pcre2_get_ovector_pointer(match.get());
            if (span[0] != start || span[1] <= start)
                throw std::runtime_error("tokenizer skipped input");
            total += count_piece(input.substr(start, span[1] - start));
            start = span[1];
        }
        return total;
    }

private:
    int
    match_large(
        std::string_view input,
        std::size_t start,
        pcre2_match_data* match,
        int error
    ) const {
        std::unique_ptr<pcre2_match_context, decltype(&pcre2_match_context_free)> context(
            pcre2_match_context_create(nullptr),
            pcre2_match_context_free
        );
        if (!context)
            throw std::bad_alloc();
        // The fixed patterns can backtrack over a whole run of uppercase letters or spaces.
        // PCRE2's default work limit is independent of input length.
        constexpr auto limit = std::numeric_limits<std::uint32_t>::max();
        const auto remaining = std::min<std::size_t>(input.size() - start, limit / 16);
        pcre2_set_match_limit(
            context.get(),
            static_cast<std::uint32_t>(std::max<std::size_t>(remaining * 16, 10'000'000))
        );
        const auto subject = reinterpret_cast<PCRE2_SPTR>(input.data());
        if (error == PCRE2_ERROR_MATCHLIMIT) {
            error = pcre2_match(
                pattern_.get(),
                subject,
                input.size(),
                start,
                PCRE2_NO_UTF_CHECK,
                match,
                context.get()
            );
            if (error != PCRE2_ERROR_JIT_STACKLIMIT)
                return error;
        }
        return pcre2_match(
            pattern_.get(),
            subject,
            input.size(),
            start,
            PCRE2_NO_UTF_CHECK | PCRE2_NO_JIT,
            match,
            context.get()
        );
    }

    std::size_t
    count_piece(std::string_view piece) const {
        if (piece.size() == 1 || vocabulary_.rank_of(piece) != unranked)
            return 1;
        // Most pieces are short. A small stack buffer avoids heap allocation in the common case.
        if (piece.size() < 48)
            return count_small(piece);
        return count_large(piece);
    }

    std::size_t
    count_small(std::string_view piece) const {
        std::size_t offsets[48];
        auto size = piece.size();
        for (std::size_t i = 0; i <= size; ++i)
            offsets[i] = i;
        while (size > 1) {
            Rank best = unranked;
            std::size_t at = 0;
            for (std::size_t i = 0; i + 1 < size; ++i) {
                const auto rank =
                    vocabulary_.rank_of(piece.substr(offsets[i], offsets[i + 2] - offsets[i]));
                if (rank < best) {
                    best = rank;
                    at = i + 1;
                }
            }
            if (best == unranked)
                break;
            std::move(offsets + at + 1, offsets + size + 1, offsets + at);
            --size;
        }
        return size;
    }

    std::size_t
    count_large(std::string_view piece) const {
        struct Edge {
            std::size_t prev, next;
        };

        auto size = piece.size();
        std::vector<Edge> chain(size + 1);
        std::priority_queue<Merge, std::vector<Merge>, std::greater<>> merges;
        const auto add = [&](std::size_t left) {
            const auto split = chain[left].next;
            if (split == piece.size())
                return;
            const auto end = chain[split].next;
            const auto rank = vocabulary_.rank_of(piece.substr(left, end - left));
            if (rank != unranked)
                merges.push({rank, left, split, end});
        };
        for (std::size_t i = 0; i <= size; ++i)
            chain[i] = {i == 0 ? size : i - 1, i + 1};
        for (std::size_t i = 0; i + 1 < size; ++i)
            add(i);
        while (!merges.empty()) {
            const auto [rank, left, split, end] = merges.top();
            merges.pop();
            if (chain[left].next != split || chain[split].next != end)
                continue;
            chain[left].next = end;
            chain[end].prev = left;
            chain[split].next = split;
            --size;
            if (left != 0)
                add(chain[left].prev);
            add(left);
        }
        return size;
    }

    Pattern pattern_;
    Vocabulary vocabulary_;
};

} // namespace

void
set_tokenizer_data_dir(std::filesystem::path directory) {
    const std::lock_guard lock(tokenizer_directory_lock);
    tokenizer_directory = std::move(directory);
}

std::size_t
token_count(std::string_view input, std::string_view name) {
    if (name == "o200k_base" || name == "o200k_harmony") {
        static const Encoding encoding("o200k_base", o200k_pattern);
        return encoding.count(input);
    }
    if (name == "cl100k_base") {
        static const Encoding encoding("cl100k_base", cl100k_pattern);
        return encoding.count(input);
    }
    if (name == "p50k_base" || name == "p50k_edit") {
        static const Encoding encoding("p50k_base", r50k_pattern);
        return encoding.count(input);
    }
    if (name == "r50k_base" || name == "gpt2") {
        static const Encoding encoding("r50k_base", r50k_pattern);
        return encoding.count(input);
    }
    throw std::invalid_argument("unknown tokenizer encoding: " + std::string(name));
}

} // namespace syntwine
