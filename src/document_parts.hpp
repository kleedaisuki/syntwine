#pragma once

#include "core.hpp"

namespace syntwine {

class DocumentParts {
public:
    DocumentParts() = default;

    explicit DocumentParts(std::vector<NodePtr> nodes) {
        for (auto& node : nodes)
            push(std::move(node));
    }

    DocumentParts(const DocumentParts& other)
        : roots_(other.roots_), size_(other.size_) {
        tail_.reserve(chunk_size);
        tail_.assign(other.tail_.begin(), other.tail_.end());
    }

    DocumentParts(DocumentParts&&) noexcept = default;

    std::size_t size() const noexcept { return size_; }

    void push(NodePtr node) {
        if (tail_.size() == chunk_size)
            seal_tail();
        tail_.push_back(std::move(node));
        ++size_;
    }

    template<class Fn>
    void visit(Fn&& fn) const {
        for (auto tree = roots_.rbegin(); tree != roots_.rend(); ++tree)
            if (*tree)
                visit_chunk(**tree, fn);
        for (const auto& node : tail_)
            fn(node);
    }

    void append_to(std::vector<NodePtr>& nodes) const {
        nodes.reserve(nodes.size() + size_);
        visit([&](const NodePtr& node) { nodes.push_back(node); });
    }

    std::vector<NodePtr> materialize() const {
        std::vector<NodePtr> nodes;
        append_to(nodes);
        return nodes;
    }

private:
    using Leaf = std::vector<NodePtr>;
    struct Chunk;
    using Tree = std::shared_ptr<const Chunk>;
    struct Branch { Tree left, right; };
    struct Chunk { std::variant<Leaf, Branch> data; };

    static constexpr std::size_t chunk_size = 32;

    void seal_tail() {
        auto tree = std::make_shared<const Chunk>(Chunk{std::move(tail_)});
        tail_.clear();
        tail_.reserve(chunk_size);
        // The occupied levels encode the number of full chunks in binary.
        // Joining equal-height trees also bounds traversal and destruction depth.
        for (auto& root : roots_) {
            if (!root) {
                root = std::move(tree);
                return;
            }
            tree = std::make_shared<const Chunk>(Chunk{Branch{std::move(root), std::move(tree)}});
        }
        roots_.push_back(std::move(tree));
    }

    template<class Fn>
    static void visit_chunk(const Chunk& chunk, Fn& fn) {
        if (const auto* split = std::get_if<Branch>(&chunk.data)) {
            visit_chunk(*split->left, fn);
            visit_chunk(*split->right, fn);
        } else {
            for (const auto& node : std::get<Leaf>(chunk.data))
                fn(node);
        }
    }

    std::vector<Tree> roots_;
    Leaf tail_;
    std::size_t size_ = 0;
};

} // namespace syntwine
