#include "tokenizer.hpp"

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>
#include <queue>
#include <stdexcept>
#include <unordered_map>
#include <vector>

#define PCRE2_CODE_UNIT_WIDTH 8
#include <pcre2.h>

namespace syntwine {
namespace {

using Rank = std::uint32_t;
constexpr Rank unranked = std::numeric_limits<Rank>::max();

std::filesystem::path tokenizer_directory;
std::mutex tokenizer_directory_lock;

std::filesystem::path vocabulary_path(std::string_view name) {
    const std::lock_guard lock(tokenizer_directory_lock);
    return tokenizer_directory / (std::string(name) + ".tiktoken");
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

unsigned base64_digit(unsigned char c) {
    if (c >= 'A' && c <= 'Z')
        return c - 'A';
    if (c >= 'a' && c <= 'z')
        return c - 'a' + 26;
    if (c >= '0' && c <= '9')
        return c - '0' + 52;
    if (c == '+' || c == '/')
        return c == '+' ? 62 : 63;
    throw std::runtime_error("invalid tokenizer vocabulary");
}

void decode_base64(std::string_view text, std::string& out) {
    unsigned accumulator = 0, bits = 0;
    for (const auto c : text) {
        if (c == '=')
            break;
        accumulator = (accumulator << 6) | base64_digit(static_cast<unsigned char>(c));
        bits += 6;
        if (bits < 8)
            continue;
        bits -= 8;
        out.push_back(static_cast<char>((accumulator >> bits) & 255));
    }
}

using Pattern = std::unique_ptr<pcre2_code, decltype(&pcre2_code_free)>;

std::string with_unicode_whitespace(std::string_view pattern) {
    // Here \h means White_Space except CR and LF, so the newline alternative
    // can match through the last CR/LF without backtracking over a space run.
    constexpr auto other_spaces = R"(\x{20}\x{85}\x{a0}\x{1680}\x{2000}-\x{200a}\x{2028}\x{2029}\x{202f}\x{205f}\x{3000})";
    std::string exact;
    bool in_class = false;
    for (std::size_t i = 0; i < pattern.size(); ++i) {
        const auto c = pattern[i];
        if (c == '[' || c == ']')
            in_class = c == '[';
        if (c != '\\' || i + 1 == pattern.size() ||
            std::string_view("sSh").find(pattern[i + 1]) == std::string_view::npos) {
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

Pattern compile_pattern(std::string_view pattern) {
    const auto exact = with_unicode_whitespace(pattern);
    int error;
    PCRE2_SIZE offset;
    Pattern result(pcre2_compile(reinterpret_cast<PCRE2_SPTR>(exact.data()),
                                 exact.size(), PCRE2_UTF | PCRE2_UCP | PCRE2_DOLLAR_ENDONLY,
                                 &error, &offset, nullptr), pcre2_code_free);
    if (!result)
        throw std::runtime_error("cannot compile tokenizer pretokenization pattern");
    pcre2_jit_compile(result.get(), PCRE2_JIT_COMPLETE);
    return result;
}

struct Merge {
    Rank rank;
    std::size_t left, split, end;
    bool operator>(const Merge& other) const {
        return rank != other.rank ? rank > other.rank : left > other.left;
    }
};

class Encoding {
public:
    Encoding(std::string_view name, std::string_view pattern) : pattern_(compile_pattern(pattern)) {
        auto input = std::ifstream(vocabulary_path(name), std::ios::binary);
        if (!input)
            throw std::runtime_error("cannot open installed tokenizer vocabulary: " + std::string(name));
        const std::string ranks{std::istreambuf_iterator<char>(input), {}};
        if (ranks.empty())
            throw std::runtime_error("empty tokenizer vocabulary");
        storage_.reserve(ranks.size());
        ranks_.reserve(static_cast<std::size_t>(std::count(ranks.begin(), ranks.end(), '\n')));
        auto rest = std::string_view(ranks);
        while (!rest.empty()) {
            const auto newline = rest.find('\n');
            auto line = rest.substr(0, newline);
            rest.remove_prefix(newline == rest.npos ? rest.size() : newline + 1);
            if (line.ends_with('\r'))
                line.remove_suffix(1);
            const auto sep = line.find(' ');
            if (sep == line.npos)
                throw std::runtime_error("invalid tokenizer vocabulary line");
            Rank rank{};
            const auto end = line.data() + line.size();
            const auto [pos, error] = std::from_chars(line.data() + sep + 1, end, rank);
            if (error != std::errc{} || pos != end)
                throw std::runtime_error("invalid tokenizer vocabulary line");
            const auto start = storage_.size();
            decode_base64(line.substr(0, sep), storage_);
            ranks_.emplace(std::string_view(storage_).substr(start), rank);
        }
        for (unsigned i = 0; i < 256; ++i) {
            const auto byte = static_cast<char>(i);
            if (rank_of(std::string_view(&byte, 1)) == unranked)
                throw std::runtime_error("tokenizer vocabulary missing byte");
        }
    }

    std::size_t count(std::string_view input) const {
        if (input.empty())
            return 0;
        std::unique_ptr<pcre2_match_data, decltype(&pcre2_match_data_free)>
            match(pcre2_match_data_create_from_pattern(pattern_.get(), nullptr), pcre2_match_data_free);
        if (!match)
            throw std::bad_alloc();
        std::size_t total = 0, start = 0;
        while (start < input.size()) {
            auto status = pcre2_match(pattern_.get(), reinterpret_cast<PCRE2_SPTR>(input.data()),
                                      input.size(), start, PCRE2_NO_UTF_CHECK, match.get(), nullptr);
            if (status == PCRE2_ERROR_JIT_STACKLIMIT || status == PCRE2_ERROR_MATCHLIMIT)
                status = match_large(input, start, match.get(), status);
            if (status < 0)
                throw std::runtime_error("tokenizer pretokenization failed: " + std::to_string(status));
            const auto span = pcre2_get_ovector_pointer(match.get());
            if (span[0] != start || span[1] <= start)
                throw std::runtime_error("tokenizer skipped input");
            total += count_piece(input.substr(start, span[1] - start));
            start = span[1];
        }
        return total;
    }

private:
    int match_large(std::string_view input, std::size_t start, pcre2_match_data* match, int error) const {
        std::unique_ptr<pcre2_match_context, decltype(&pcre2_match_context_free)>
            context(pcre2_match_context_create(nullptr), pcre2_match_context_free);
        if (!context)
            throw std::bad_alloc();
        // The fixed patterns can backtrack over a whole run of uppercase letters or spaces.
        // PCRE2's default work limit is independent of input length.
        constexpr auto limit = std::numeric_limits<std::uint32_t>::max();
        const auto remaining = std::min<std::size_t>(input.size() - start, limit / 16);
        pcre2_set_match_limit(context.get(), static_cast<std::uint32_t>(
            std::max<std::size_t>(remaining * 16, 10'000'000)));
        const auto subject = reinterpret_cast<PCRE2_SPTR>(input.data());
        if (error == PCRE2_ERROR_MATCHLIMIT) {
            error = pcre2_match(pattern_.get(), subject, input.size(), start, PCRE2_NO_UTF_CHECK,
                                match, context.get());
            if (error != PCRE2_ERROR_JIT_STACKLIMIT)
                return error;
        }
        return pcre2_match(pattern_.get(), subject, input.size(), start,
                           PCRE2_NO_UTF_CHECK | PCRE2_NO_JIT, match, context.get());
    }

    Rank rank_of(std::string_view token) const {
        const auto it = ranks_.find(token);
        return it == ranks_.end() ? unranked : it->second;
    }

    std::size_t count_piece(std::string_view piece) const {
        if (piece.size() == 1 || rank_of(piece) != unranked)
            return 1;
        // Most pieces are short. A small stack buffer avoids heap allocation in the common case.
        if (piece.size() < 48)
            return count_small(piece);
        return count_large(piece);
    }

    std::size_t count_small(std::string_view piece) const {
        std::size_t offsets[48];
        auto size = piece.size();
        for (std::size_t i = 0; i <= size; ++i)
            offsets[i] = i;
        while (size > 1) {
            Rank best = unranked;
            std::size_t at = 0;
            for (std::size_t i = 0; i + 1 < size; ++i) {
                const auto rank = rank_of(piece.substr(offsets[i], offsets[i + 2] - offsets[i]));
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

    std::size_t count_large(std::string_view piece) const {
        struct Edge { std::size_t prev, next; };
        auto size = piece.size();
        std::vector<Edge> chain(size + 1);
        std::priority_queue<Merge, std::vector<Merge>, std::greater<>> merges;
        const auto add = [&](std::size_t left) {
            const auto split = chain[left].next;
            if (split == piece.size())
                return;
            const auto end = chain[split].next;
            const auto rank = rank_of(piece.substr(left, end - left));
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
    std::string storage_;
    std::unordered_map<std::string_view, Rank> ranks_;
};

} // namespace

void set_tokenizer_data_dir(std::filesystem::path directory) {
    const std::lock_guard lock(tokenizer_directory_lock);
    tokenizer_directory = std::move(directory);
}

std::size_t token_count(std::string_view input, std::string_view name) {
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
