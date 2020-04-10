#ifndef BVH_BINNED_SAH_BUILDER_HPP
#define BVH_BINNED_SAH_BUILDER_HPP

#include <optional>

#include "bvh/bvh.hpp"
#include "bvh/bounding_box.hpp"
#include "bvh/top_down_builder.hpp"

namespace bvh {

template <typename Bvh, size_t BinCount>
class BinnedSahBuildTask {
    using Scalar  = typename Bvh::ScalarType;
    using Builder = TopDownBuilder<Bvh, BinnedSahBuildTask>;

public:
    struct WorkItem {
        size_t node_index;
        size_t begin;
        size_t end;
        size_t depth;

        WorkItem() = default;
        WorkItem(size_t node_index, size_t begin, size_t end, size_t depth)
            : node_index(node_index), begin(begin), end(end), depth(depth)
        {}

        size_t work_size() const { return end - begin; }
    };

private:
    struct Bin {
        BoundingBox<Scalar> bbox;
        size_t primitive_count;
        Scalar right_cost;
    };

    static constexpr size_t bin_count = BinCount;
    std::array<Bin, bin_count> bins_per_axis[3];

    Builder& builder;
    const BoundingBox<Scalar>* bboxes;
    const Vector3<Scalar>* centers;

public:
    BinnedSahBuildTask(Builder& builder, const BoundingBox<Scalar>* bboxes, const Vector3<Scalar>* centers)
        : builder(builder), bboxes(bboxes), centers(centers)
    {}

    template <typename BinIndex>
    std::pair<Scalar, size_t> find_split(int axis, size_t begin, size_t end, BinIndex bin_index) {
        auto& bins = bins_per_axis[axis];
        auto& primitive_indices = builder.bvh.primitive_indices;

        // Setup bins
        for (auto& bin : bins) {
            bin.bbox = BoundingBox<Scalar>::empty();
            bin.primitive_count = 0;
        }

        // Fill bins with primitives
        for (size_t i = begin; i < end; ++i) {
            auto primitive_index = primitive_indices[i];
            Bin& bin = bins[bin_index(centers[primitive_index], axis)];
            bin.primitive_count++;
            bin.bbox.extend(bboxes[primitive_index]);
        }

        // Right sweep to compute partial SAH
        auto   current_bbox  = BoundingBox<Scalar>::empty();
        size_t current_count = 0;
        for (size_t i = bin_count - 1; i > 0; --i) {
            current_bbox.extend(bins[i].bbox);
            current_count += bins[i].primitive_count;
            bins[i].right_cost = current_bbox.half_area() * current_count;
        }

        // Left sweep to compute full cost and find minimum
        current_bbox  = BoundingBox<Scalar>::empty();
        current_count = 0;

        auto best_split = std::pair<Scalar, size_t>(std::numeric_limits<Scalar>::max(), bin_count);
        for (size_t i = 0; i < bin_count - 1; ++i) {
            current_bbox.extend(bins[i].bbox);
            current_count += bins[i].primitive_count;
            auto cost = current_bbox.half_area() * current_count + bins[i + 1].right_cost;
            if (cost < best_split.first)
                best_split = std::make_pair(cost, i + 1);
        }
        return best_split;
    }

    std::optional<std::pair<WorkItem, WorkItem>> build(const WorkItem& item) {
        auto& bvh  = builder.bvh;
        auto& node = bvh.nodes[item.node_index];

        auto make_leaf = [] (typename Bvh::Node& node, size_t begin, size_t end) {
            node.first_child_or_primitive = begin;
            node.primitive_count          = end - begin;
            node.is_leaf                  = true;
        };

        if (item.work_size() <= 1 || item.depth >= bvh.max_depth) {
            make_leaf(node, item.begin, item.end);
            return std::nullopt;
        }

        auto primitive_indices = bvh.primitive_indices.get();

        // Compute the bounding box of the centers of the primitives in this node
        auto center_bbox = BoundingBox<Scalar>::empty();
        for (size_t i = item.begin; i < item.end; ++i)
            center_bbox.extend(centers[primitive_indices[i]]);

        std::pair<Scalar, size_t> best_splits[3];

        auto inverse = center_bbox.diagonal().inverse() * Scalar(bin_count);
        auto base    = -center_bbox.min * inverse;
        auto bin_index = [=] (const Vector3<Scalar>& center, int axis) {
            return std::min(size_t(multiply_add(center[axis], inverse[axis], base[axis])), size_t(bin_count - 1));
        };

        #pragma omp taskloop if (item.work_size() > builder.parallel_threshold) grainsize(1) default(shared)
        for (int axis = 0; axis < 3; ++axis)
            best_splits[axis] = find_split(axis, item.begin, item.end, bin_index);

        int best_axis = 0;
        if (best_splits[0].first > best_splits[1].first)
            best_axis = 1;
        if (best_splits[best_axis].first > best_splits[2].first)
            best_axis = 2;

        // Make sure the cost of splitting does not exceed the cost of not splitting
        if (best_splits[best_axis].second == bin_count ||
            best_splits[best_axis].first >= node.bounding_box_proxy().half_area() * (item.work_size() - bvh.traversal_cost)) {
            make_leaf(node, item.begin, item.end);
            return std::nullopt;
        }

        // Split primitives according to split position
        size_t begin_right = std::partition(primitive_indices + item.begin, primitive_indices + item.end, [&] (size_t i) {
            return bin_index(centers[i], best_axis) < best_splits[best_axis].second;
        }) - primitive_indices;

        // Check that the split does not leave one side empty
        if (begin_right > item.begin && begin_right < item.end) {
            // Allocate two nodes
            size_t left_index;
            #pragma omp atomic capture
            { left_index = bvh.node_count; bvh.node_count += 2; }
            auto& left  = bvh.nodes[left_index + 0];
            auto& right = bvh.nodes[left_index + 1];
            node.first_child_or_primitive = left_index;
            node.primitive_count          = 0;
            node.is_leaf                  = false;

            // Compute the bounding boxes of each node
            auto& bins = bins_per_axis[best_axis];
            auto left_bbox  = BoundingBox<Scalar>::empty();
            auto right_bbox = BoundingBox<Scalar>::empty();
            for (size_t i = 0; i < best_splits[best_axis].second; ++i)
                left_bbox.extend(bins[i].bbox);
            for (size_t i = best_splits[best_axis].second; i < bin_count; ++i)
                right_bbox.extend(bins[i].bbox);
            left.bounding_box_proxy()  = left_bbox;
            right.bounding_box_proxy() = right_bbox;

            // Return new work items
            WorkItem first_item (left_index + 0, item.begin, begin_right, item.depth + 1);
            WorkItem second_item(left_index + 1, begin_right, item.end,   item.depth + 1);
            return std::make_optional(std::make_pair(first_item, second_item));
        }

        make_leaf(node, item.begin, item.end);
        return std::nullopt;
    }
};

template <typename Bvh, size_t BinCount>
class BinnedSahBuilder : public TopDownBuilder<Bvh, BinnedSahBuildTask<Bvh, BinCount>> {
    using Scalar = typename Bvh::ScalarType;

    using ParentBuilder = TopDownBuilder<Bvh, BinnedSahBuildTask<Bvh, BinCount>>;
    using ParentBuilder::bvh;
    using ParentBuilder::run;

public:
    BinnedSahBuilder(Bvh& bvh)
        : ParentBuilder(bvh)
    {}

    void build(const BoundingBox<Scalar>* bboxes, const Vector3<Scalar>* centers, size_t primitive_count) {
        // Allocate buffers
        bvh.nodes.reset(new typename Bvh::Node[2 * primitive_count + 1]);
        bvh.primitive_indices.reset(new size_t[primitive_count]);

        // Initialize root node
        auto root_bbox = BoundingBox<Scalar>::empty();
        bvh.node_count = 1;

        #pragma omp parallel
        {
            #pragma omp declare reduction \
                (bbox_extend:BoundingBox<Scalar>:omp_out.extend(omp_in)) \
                initializer(omp_priv = BoundingBox<Scalar>::empty())

            #pragma omp for reduction(bbox_extend: root_bbox)
            for (size_t i = 0; i < primitive_count; ++i) {
                root_bbox.extend(bboxes[i]);
                bvh.primitive_indices[i] = i;
            }

            #pragma omp single
            {
                bvh.nodes[0].bounding_box_proxy() = root_bbox;
                BinnedSahBuildTask first_task(*this, bboxes, centers);
                run(first_task, 0, 0, primitive_count, 0);
            }
        }
    }
};

} // namespace bvh

#endif