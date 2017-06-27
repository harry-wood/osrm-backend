#include "engine/routing_algorithms/many_to_many.hpp"
#include "engine/routing_algorithms/routing_base_ch.hpp"

#include <boost/assert.hpp>

#include <limits>
#include <memory>
#include <unordered_map>
#include <vector>

namespace osrm
{
namespace engine
{
namespace routing_algorithms
{

namespace
{
struct NodeBucket
{
    unsigned target_id; // essentially a row in the weight matrix
    EdgeWeight weight;
    EdgeWeight duration;
    NodeBucket(const unsigned target_id, const EdgeWeight weight, const EdgeWeight duration)
        : target_id(target_id), weight(weight), duration(duration)
    {
    }
};
}

// FIXME This should be replaced by an std::unordered_multimap, though this needs benchmarking
using SearchSpaceWithBuckets = std::unordered_map<NodeID, std::vector<NodeBucket>>;

namespace ch
{
template <bool DIRECTION>
void relaxOutgoingEdges(const datafacade::ContiguousInternalMemoryDataFacade<Algorithm> &facade,
                        const NodeID node,
                        const EdgeWeight weight,
                        const EdgeWeight duration,
                        typename SearchEngineData<Algorithm>::ManyToManyQueryHeap &query_heap)
{
    for (auto edge : facade.GetAdjacentEdgeRange(node))
    {
        const auto &data = facade.GetEdgeData(edge);
        if (DIRECTION == FORWARD_DIRECTION ? data.forward : data.backward)
        {
            const NodeID to = facade.GetTarget(edge);
            const EdgeWeight edge_weight = data.weight;
            const EdgeWeight edge_duration = data.duration;

            BOOST_ASSERT_MSG(edge_weight > 0, "edge_weight invalid");
            const EdgeWeight to_weight = weight + edge_weight;
            const EdgeWeight to_duration = duration + edge_duration;

            // New Node discovered -> Add to Heap + Node Info Storage
            if (!query_heap.WasInserted(to))
            {
                query_heap.Insert(to, to_weight, {node, to_duration});
            }
            // Found a shorter Path -> Update weight
            else if (to_weight < query_heap.GetKey(to))
            {
                // new parent
                query_heap.GetData(to) = {node, to_duration};
                query_heap.DecreaseKey(to, to_weight);
            }
        }
    }
}

void forwardRoutingStep(const datafacade::ContiguousInternalMemoryDataFacade<Algorithm> &facade,
                        const unsigned row_idx,
                        const unsigned number_of_targets,
                        typename SearchEngineData<Algorithm>::ManyToManyQueryHeap &query_heap,
                        const SearchSpaceWithBuckets &search_space_with_buckets,
                        std::vector<EdgeWeight> &weights_table,
                        std::vector<EdgeWeight> &durations_table)
{
    const NodeID node = query_heap.DeleteMin();
    const EdgeWeight source_weight = query_heap.GetKey(node);
    const EdgeWeight source_duration = query_heap.GetData(node).duration;

    // check if each encountered node has an entry
    const auto bucket_iterator = search_space_with_buckets.find(node);
    // iterate bucket if there exists one
    if (bucket_iterator != search_space_with_buckets.end())
    {
        const std::vector<NodeBucket> &bucket_list = bucket_iterator->second;
        for (const NodeBucket &current_bucket : bucket_list)
        {
            // get target id from bucket entry
            const unsigned column_idx = current_bucket.target_id;
            const EdgeWeight target_weight = current_bucket.weight;
            const EdgeWeight target_duration = current_bucket.duration;

            auto &current_weight = weights_table[row_idx * number_of_targets + column_idx];
            auto &current_duration = durations_table[row_idx * number_of_targets + column_idx];

            // check if new weight is better
            const EdgeWeight new_weight = source_weight + target_weight;
            if (new_weight < 0)
            {
                const EdgeWeight loop_weight = ch::getLoopWeight<false>(facade, node);
                const EdgeWeight new_weight_with_loop = new_weight + loop_weight;
                if (loop_weight != INVALID_EDGE_WEIGHT && new_weight_with_loop >= 0)
                {
                    current_weight = std::min(current_weight, new_weight_with_loop);
                    current_duration = std::min(current_duration,
                                                source_duration + target_duration +
                                                    ch::getLoopWeight<true>(facade, node));
                }
            }
            else if (new_weight < current_weight)
            {
                current_weight = new_weight;
                current_duration = source_duration + target_duration;
            }
        }
    }
    if (ch::stallAtNode<FORWARD_DIRECTION>(facade, node, source_weight, query_heap))
    {
        return;
    }

    relaxOutgoingEdges<FORWARD_DIRECTION>(facade, node, source_weight, source_duration, query_heap);
}

void backwardRoutingStep(const datafacade::ContiguousInternalMemoryDataFacade<Algorithm> &facade,
                         const unsigned column_idx,
                         typename SearchEngineData<Algorithm>::ManyToManyQueryHeap &query_heap,
                         SearchSpaceWithBuckets &search_space_with_buckets)
{
    const NodeID node = query_heap.DeleteMin();
    const EdgeWeight target_weight = query_heap.GetKey(node);
    const EdgeWeight target_duration = query_heap.GetData(node).duration;

    // store settled nodes in search space bucket
    search_space_with_buckets[node].emplace_back(column_idx, target_weight, target_duration);

    if (ch::stallAtNode<REVERSE_DIRECTION>(facade, node, target_weight, query_heap))
    {
        return;
    }

    relaxOutgoingEdges<REVERSE_DIRECTION>(facade, node, target_weight, target_duration, query_heap);
}

std::vector<EdgeWeight>
manyToManySearch(SearchEngineData<Algorithm> &engine_working_data,
                 const datafacade::ContiguousInternalMemoryDataFacade<Algorithm> &facade,
                 const std::vector<PhantomNode> &phantom_nodes,
                 const std::vector<std::size_t> &source_indices,
                 const std::vector<std::size_t> &target_indices)
{
    const auto number_of_sources =
        source_indices.empty() ? phantom_nodes.size() : source_indices.size();
    const auto number_of_targets =
        target_indices.empty() ? phantom_nodes.size() : target_indices.size();
    const auto number_of_entries = number_of_sources * number_of_targets;

    std::vector<EdgeWeight> weights_table(number_of_entries, INVALID_EDGE_WEIGHT);
    std::vector<EdgeWeight> durations_table(number_of_entries, MAXIMAL_EDGE_DURATION);

    engine_working_data.InitializeOrClearManyToManyThreadLocalStorage(facade.GetNumberOfNodes());

    auto &query_heap = *(engine_working_data.many_to_many_heap);

    SearchSpaceWithBuckets search_space_with_buckets;

    unsigned column_idx = 0;
    const auto search_target_phantom = [&](const PhantomNode &phantom) {
        // clear heap and insert target nodes
        query_heap.Clear();
        insertTargetInHeap(query_heap, phantom);

        // explore search space
        while (!query_heap.Empty())
        {
            backwardRoutingStep(facade, column_idx, query_heap, search_space_with_buckets);
        }
        ++column_idx;
    };

    // for each source do forward search
    unsigned row_idx = 0;
    const auto search_source_phantom = [&](const PhantomNode &phantom) {
        // clear heap and insert source nodes
        query_heap.Clear();
        insertSourceInHeap(query_heap, phantom);

        // explore search space
        while (!query_heap.Empty())
        {
            forwardRoutingStep(facade,
                               row_idx,
                               number_of_targets,
                               query_heap,
                               search_space_with_buckets,
                               weights_table,
                               durations_table);
        }
        ++row_idx;
    };

    if (target_indices.empty())
    {
        for (const auto &phantom : phantom_nodes)
        {
            search_target_phantom(phantom);
        }
    }
    else
    {
        for (const auto index : target_indices)
        {
            const auto &phantom = phantom_nodes[index];
            search_target_phantom(phantom);
        }
    }

    if (source_indices.empty())
    {
        for (const auto &phantom : phantom_nodes)
        {
            search_source_phantom(phantom);
        }
    }
    else
    {
        for (const auto index : source_indices)
        {
            const auto &phantom = phantom_nodes[index];
            search_source_phantom(phantom);
        }
    }

    return durations_table;
}

} // namespace ch

// TODO: generalize with CH version
namespace mld
{
template <bool DIRECTION>
void relaxOutgoingEdges(const datafacade::ContiguousInternalMemoryDataFacade<Algorithm> &facade,
                        const NodeID node,
                        const EdgeWeight weight,
                        const EdgeWeight duration,
                        typename SearchEngineData<Algorithm>::ManyToManyQueryHeap &query_heap,
                        const std::pair<LevelID, CellID> &parentCell)
{
    const auto &partition = facade.GetMultiLevelPartition();
    const auto &cells = facade.GetCellStorage();

    const auto &node_data = query_heap.GetData(node);
    const auto level =
        std::max(node_data.level, partition.GetHighestDifferentLevel(node_data.parent, node));

    if (level >= 1 && !node_data.from_clique_arc)
    {
        const auto &cell = cells.GetCell(level, partition.GetCell(level, node));
        if (DIRECTION == FORWARD_DIRECTION)
        {
            // Shortcuts in forward direction
            auto destination = cell.GetDestinationNodes().begin();
            auto shortcut_durations = cell.GetOutDuration(node);
            for (auto shortcut_weight : cell.GetOutWeight(node))
            {
                BOOST_ASSERT(destination != cell.GetDestinationNodes().end());
                BOOST_ASSERT(!shortcut_durations.empty());
                const NodeID to = *destination;
                if (shortcut_weight != INVALID_EDGE_WEIGHT && node != to)
                {
                    const auto to_weight = weight + shortcut_weight;
                    const auto to_duration = duration + shortcut_durations.front();
                    if (!query_heap.WasInserted(to))
                    {
                        query_heap.Insert(to, to_weight, {node, true, level, to_duration});
                    }
                    else if (to_weight < query_heap.GetKey(to))
                    {
                        query_heap.GetData(to) = {node, true, level, to_duration};
                        query_heap.DecreaseKey(to, to_weight);
                    }
                }
                ++destination;
                shortcut_durations.advance_begin(1);
            }
            BOOST_ASSERT(shortcut_durations.empty());
        }
        else
        {
            // Shortcuts in backward direction
            auto source = cell.GetSourceNodes().begin();
            auto shortcut_durations = cell.GetInDuration(node);
            for (auto shortcut_weight : cell.GetInWeight(node))
            {
                BOOST_ASSERT(source != cell.GetSourceNodes().end());
                BOOST_ASSERT(!shortcut_durations.empty());
                const NodeID to = *source;
                if (shortcut_weight != INVALID_EDGE_WEIGHT && node != to)
                {
                    const auto to_weight = weight + shortcut_weight;
                    const auto to_duration = duration + shortcut_durations.front();
                    if (!query_heap.WasInserted(to))
                    {
                        query_heap.Insert(to, to_weight, {node, true, level, to_duration});
                    }
                    else if (to_weight < query_heap.GetKey(to))
                    {
                        query_heap.GetData(to) = {node, true, level, to_duration};
                        query_heap.DecreaseKey(to, to_weight);
                    }
                }
                ++source;
                shortcut_durations.advance_begin(1);
            }
            BOOST_ASSERT(shortcut_durations.empty());
        }
    }

    for (const auto edge : facade.GetBorderEdgeRange(level, node))
    {
        const auto &data = facade.GetEdgeData(edge);
        if (DIRECTION == FORWARD_DIRECTION ? data.forward : data.backward)
        {
            const NodeID to = facade.GetTarget(edge);

            if (partition.GetCell(parentCell.first, to) == parentCell.second)
            {
                const EdgeWeight edge_weight = data.weight;
                const EdgeWeight edge_duration = data.duration;

                BOOST_ASSERT_MSG(edge_weight > 0, "edge_weight invalid");
                const EdgeWeight to_weight = weight + edge_weight;
                const EdgeWeight to_duration = duration + edge_duration;

                // New Node discovered -> Add to Heap + Node Info Storage
                if (!query_heap.WasInserted(to))
                {
                    query_heap.Insert(to, to_weight, {node, false, level, to_duration});
                }
                // Found a shorter Path -> Update weight
                else if (to_weight < query_heap.GetKey(to))
                {
                    // new parent
                    query_heap.GetData(to) = {node, false, level, to_duration};
                    query_heap.DecreaseKey(to, to_weight);
                }
            }
        }
    }
}

void forwardRoutingStep(const datafacade::ContiguousInternalMemoryDataFacade<Algorithm> &facade,
                        const unsigned row_idx,
                        const unsigned number_of_targets,
                        typename SearchEngineData<Algorithm>::ManyToManyQueryHeap &query_heap,
                        const SearchSpaceWithBuckets &search_space_with_buckets,
                        std::vector<EdgeWeight> &weights_table,
                        std::vector<EdgeWeight> &durations_table,
                        const std::pair<LevelID, CellID> &parentCell)
{
    const NodeID node = query_heap.DeleteMin();
    const EdgeWeight source_weight = query_heap.GetKey(node);
    const EdgeWeight source_duration = query_heap.GetData(node).duration;

    // check if each encountered node has an entry
    const auto bucket_iterator = search_space_with_buckets.find(node);
    // iterate bucket if there exists one
    if (bucket_iterator != search_space_with_buckets.end())
    {
        const std::vector<NodeBucket> &bucket_list = bucket_iterator->second;
        for (const NodeBucket &current_bucket : bucket_list)
        {
            // get target id from bucket entry
            const unsigned column_idx = current_bucket.target_id;
            const EdgeWeight target_weight = current_bucket.weight;
            const EdgeWeight target_duration = current_bucket.duration;

            auto &current_weight = weights_table[row_idx * number_of_targets + column_idx];
            auto &current_duration = durations_table[row_idx * number_of_targets + column_idx];

            // check if new weight is better
            const EdgeWeight new_weight = source_weight + target_weight;
            if (new_weight >= 0 && new_weight < current_weight)
            {
                current_weight = new_weight;
                current_duration = source_duration + target_duration;
            }
        }
    }

    relaxOutgoingEdges<FORWARD_DIRECTION>(
        facade, node, source_weight, source_duration, query_heap, parentCell);
}

void backwardRoutingStep(const datafacade::ContiguousInternalMemoryDataFacade<Algorithm> &facade,
                         const unsigned column_idx,
                         typename SearchEngineData<Algorithm>::ManyToManyQueryHeap &query_heap,
                         SearchSpaceWithBuckets &search_space_with_buckets,
                         const std::pair<LevelID, CellID> &parentCell)
{
    const NodeID node = query_heap.DeleteMin();
    const EdgeWeight target_weight = query_heap.GetKey(node);
    const EdgeWeight target_duration = query_heap.GetData(node).duration;

    // store settled nodes in search space bucket
    search_space_with_buckets[node].emplace_back(column_idx, target_weight, target_duration);

    relaxOutgoingEdges<REVERSE_DIRECTION>(
        facade, node, target_weight, target_duration, query_heap, parentCell);
}

std::pair<LevelID, CellID> getParentCellID(const partition::MultiLevelPartitionView partition,
                                           const PhantomNode &source,
                                           const std::vector<PhantomNode> &phantom_nodes,
                                           const std::vector<std::size_t> &phantom_indices)
{
    LevelID highest_different_level = 0;

    auto level = [&partition](const SegmentID &source, const SegmentID &target) {
        if (source.enabled && target.enabled)
            return partition.GetHighestDifferentLevel(source.id, target.id);
        return LevelID{0};
    };

    auto highest_level = [&source, &level](const auto &target) {
        return std::max(std::max(level(source.forward_segment_id, target.forward_segment_id),
                                 level(source.forward_segment_id, target.reverse_segment_id)),
                        std::max(level(source.reverse_segment_id, target.forward_segment_id),
                                 level(source.reverse_segment_id, target.reverse_segment_id)));
    };

    if (phantom_indices.empty())
    {
        for (const auto &phantom : phantom_nodes)
        {
            highest_different_level = std::max(highest_different_level, highest_level(phantom));
        }
    }
    else
    {
        for (const auto index : phantom_indices)
        {
            highest_different_level =
                std::max(highest_different_level, highest_level(phantom_nodes[index]));
        }
    }

    // All nodes must be in the same parent cell
    return std::make_pair(
        highest_different_level + 1,
        partition.GetCell(highest_different_level + 1, source.forward_segment_id.id));
}

std::vector<EdgeWeight>
manyToManySearch(SearchEngineData<Algorithm> &engine_working_data,
                 const datafacade::ContiguousInternalMemoryDataFacade<Algorithm> &facade,
                 const std::vector<PhantomNode> &phantom_nodes,
                 const std::vector<std::size_t> &source_indices,
                 const std::vector<std::size_t> &target_indices)
{
    const auto number_of_sources =
        source_indices.empty() ? phantom_nodes.size() : source_indices.size();
    const auto number_of_targets =
        target_indices.empty() ? phantom_nodes.size() : target_indices.size();
    const auto number_of_entries = number_of_sources * number_of_targets;

    std::vector<EdgeWeight> weights_table(number_of_entries, INVALID_EDGE_WEIGHT);
    std::vector<EdgeWeight> durations_table(number_of_entries, MAXIMAL_EDGE_DURATION);

    engine_working_data.InitializeOrClearManyToManyThreadLocalStorage(facade.GetNumberOfNodes());

    auto &query_heap = *(engine_working_data.many_to_many_heap);

    SearchSpaceWithBuckets search_space_with_buckets;

    unsigned column_idx = 0;
    const auto search_target_phantom = [&](const PhantomNode &phantom,
                                           const std::pair<LevelID, CellID> &parentCell) {
        // clear heap and insert target nodes
        query_heap.Clear();
        insertTargetInHeap(query_heap, phantom);

        // explore search space
        while (!query_heap.Empty())
        {
            backwardRoutingStep(
                facade, column_idx, query_heap, search_space_with_buckets, parentCell);
        }
        ++column_idx;
    };

    // for each source do forward search
    unsigned row_idx = 0;
    const auto search_source_phantom = [&](const PhantomNode &phantom,
                                           const std::pair<LevelID, CellID> &parentCell) {
        // clear heap and insert source nodes
        query_heap.Clear();
        insertSourceInHeap(query_heap, phantom);

        // explore search space
        while (!query_heap.Empty())
        {
            forwardRoutingStep(facade,
                               row_idx,
                               number_of_targets,
                               query_heap,
                               search_space_with_buckets,
                               weights_table,
                               durations_table,
                               parentCell);
        }
        ++row_idx;
    };

    if (target_indices.empty())
    {
        for (const auto &phantom : phantom_nodes)
        {
            const auto parentCell = getParentCellID(
                facade.GetMultiLevelPartition(), phantom, phantom_nodes, source_indices);
            search_target_phantom(phantom, parentCell);
        }
    }
    else
    {
        for (const auto index : target_indices)
        {
            const auto &phantom = phantom_nodes[index];
            const auto parentCell = getParentCellID(
                facade.GetMultiLevelPartition(), phantom, phantom_nodes, source_indices);
            search_target_phantom(phantom, parentCell);
        }
    }

    if (source_indices.empty())
    {
        for (const auto &phantom : phantom_nodes)
        {
            const auto parentCell = getParentCellID(
                facade.GetMultiLevelPartition(), phantom, phantom_nodes, target_indices);
            search_source_phantom(phantom, parentCell);
        }
    }
    else
    {
        for (const auto index : source_indices)
        {
            const auto &phantom = phantom_nodes[index];
            const auto parentCell = getParentCellID(
                facade.GetMultiLevelPartition(), phantom, phantom_nodes, target_indices);
            search_source_phantom(phantom, parentCell);
        }
    }

    return durations_table;
}

} // namespace mld

} // namespace routing_algorithms
} // namespace engine
} // namespace osrm
