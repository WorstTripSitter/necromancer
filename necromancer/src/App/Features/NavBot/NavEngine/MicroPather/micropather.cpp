/*
Copyright (c) 2000-2009 Lee Thomason (www.grinninglizard.com)

This software is provided 'as-is', without any express or implied 
warranty. In no event will the authors be held liable for any 
damages arising from the use of this software.

Permission is granted to anyone to use this software for any 
purpose, including commercial applications, and to alter it and 
redistribute it freely, subject to the following restrictions:

1. The origin of this software must not be misrepresented; you must 
not claim that you wrote the original software. If you use this 
software in a product, an acknowledgment in the product documentation 
would be appreciated but is not required.

2. Altered source versions must be plainly marked as such, and 
must not be misrepresented as being the original software.

3. This notice may not be removed or altered from any source 
distribution.
*/

#ifdef _MSC_VER
#pragma warning( disable : 4786 )
#pragma warning( disable : 4530 )
#endif

#include <memory.h>
#include <cstdio>
#include "micropather.h"

using namespace micropather;

class OpenQueue
{
public:
    explicit OpenQueue(Graph* _graph)
    {
#ifdef DEBUG
        graph = _graph;
#endif
        sentinel = reinterpret_cast<PathNode*>(sentinelMem);
        sentinel->InitSentinel();
#ifdef DEBUG
        sentinel->CheckList();
#endif
    }
    ~OpenQueue() = default;

    void Push(PathNode* pNode);
    PathNode* Pop();
    void Update(PathNode* pNode);

    bool Empty()
    {
        return sentinel->next == sentinel;
    }

private:
    PathNode* sentinel;
    int sentinelMem[(sizeof(PathNode) + sizeof(int)) / sizeof(int)]{};
#ifdef DEBUG
    Graph* graph;
#endif
};

void OpenQueue::Push(PathNode* pNode)
{
    MPASSERT(pNode->inOpen == 0)
    MPASSERT(pNode->inClosed == 0)

    MPASSERT(pNode->totalCost < FLT_MAX)
    PathNode* iter = sentinel->next;
    while (true)
    {
        if (pNode->totalCost < iter->totalCost)
        {
            iter->AddBefore(pNode);
            pNode->inOpen = true;
            break;
        }
        iter = iter->next;
    }
    MPASSERT(pNode->inOpen)
#ifdef DEBUG
    sentinel->CheckList();
#endif
}

PathNode* OpenQueue::Pop()
{
    MPASSERT(sentinel->next != sentinel)
    PathNode* pNode = sentinel->next;
    pNode->Unlink();
#ifdef DEBUG
    sentinel->CheckList();
#endif

    MPASSERT(pNode->inClosed == 0)
    MPASSERT(pNode->inOpen == 1)
    pNode->inOpen = false;

    return pNode;
}

void OpenQueue::Update(PathNode* pNode)
{
    MPASSERT(pNode->inOpen)

    if (pNode->prev != sentinel && pNode->totalCost < pNode->prev->totalCost)
    {
        pNode->Unlink();
        sentinel->next->AddBefore(pNode);
    }

    if (pNode->totalCost > pNode->next->totalCost)
    {
        PathNode* it = pNode->next;
        pNode->Unlink();

        while (pNode->totalCost > it->totalCost)
            it = it->next;

        it->AddBefore(pNode);
#ifdef DEBUG
        sentinel->CheckList();
#endif
    }
}

class ClosedSet
{
public:
    explicit ClosedSet(Graph* _graph)
    {
        this->graph = _graph;
    }

    ~ClosedSet() = default;

    void Add(PathNode* pNode)
    {
#ifdef DEBUG
        MPASSERT(pNode->inClosed == 0);
        MPASSERT(pNode->inOpen == 0);
#endif
        pNode->inClosed = true;
    }

    void Remove(PathNode* pNode)
    {
        MPASSERT(pNode->inClosed == 1);
        MPASSERT(pNode->inOpen == 0);
        pNode->inClosed = false;
    }

    ClosedSet(const ClosedSet&) = delete;
    void operator=(const ClosedSet&) = delete;
private:
    Graph* graph;
};

PathNodePool::PathNodePool(unsigned int _allocate, unsigned int _typicalAdjacent)
    : firstBlock(nullptr),
      blocks(nullptr),
      allocate(_allocate),
      nAllocated(0),
      nAvailable(0)
{
    freeMemSentinel.InitSentinel();

    cacheCap = allocate * _typicalAdjacent;
    cacheSize = 0;
    cache = static_cast<NodeCost*>(malloc(cacheCap * sizeof(NodeCost)));

    hashShift = 3;
    while (HashSize() < allocate)
        ++hashShift;
    hashTable = static_cast<PathNode**>(calloc(HashSize(), sizeof(PathNode*)));

    blocks = firstBlock = NewBlock();
    totalCollide = 0;
}

PathNodePool::~PathNodePool()
{
    Clear();
    free(firstBlock);
    free(cache);
    free(hashTable);
}

bool PathNodePool::PushCache(const NodeCost* nodes, int nNodes, int* start)
{
    *start = -1;
    if (nNodes + cacheSize <= cacheCap)
    {
        for (int i = 0; i < nNodes; ++i)
            cache[i + cacheSize] = nodes[i];
        *start = cacheSize;
        cacheSize += nNodes;
        return true;
    }
    return false;
}

void PathNodePool::GetCache(int start, int nNodes, NodeCost* nodes)
{
    MPASSERT(start >= 0 && start < cacheCap)
    MPASSERT(nNodes > 0)
    MPASSERT(start + nNodes <= cacheCap)
    memcpy(nodes, &cache[start], sizeof(NodeCost) * nNodes);
}

void PathNodePool::Clear()
{
    Block* b = blocks;
    while (b)
    {
        Block* temp = b->nextBlock;
        if (b != firstBlock)
            free(b);
        b = temp;
    }
    blocks = firstBlock;

    if (nAllocated > 0)
    {
        freeMemSentinel.next = &freeMemSentinel;
        freeMemSentinel.prev = &freeMemSentinel;

        memset(hashTable, 0, sizeof(PathNode*) * HashSize());
        for (unsigned int i = 0; i < allocate; ++i)
            freeMemSentinel.AddBefore(&firstBlock->pathNode[i]);
    }
    nAvailable = allocate;
    nAllocated = 0;
    cacheSize = 0;
}

PathNodePool::Block* PathNodePool::NewBlock()
{
    auto block = static_cast<Block*>(calloc(1, sizeof(Block) + sizeof(PathNode) * (allocate - 1)));
    block->nextBlock = nullptr;

    nAvailable += allocate;
    for (unsigned int i = 0; i < allocate; ++i)
        freeMemSentinel.AddBefore(&block->pathNode[i]);
    return block;
}

unsigned int PathNodePool::Hash(void* voidval)
{
    auto h = reinterpret_cast<MP_UPTR>(voidval);
    return h & HashMask();
}

PathNode* PathNodePool::Alloc()
{
    if (freeMemSentinel.next == &freeMemSentinel)
    {
        MPASSERT(nAvailable == 0)
        Block* b = NewBlock();
        b->nextBlock = blocks;
        blocks = b;
        MPASSERT(freeMemSentinel.next != &freeMemSentinel)
    }
    PathNode* pathNode = freeMemSentinel.next;
    pathNode->Unlink();

    ++nAllocated;
    MPASSERT(nAvailable > 0)
    --nAvailable;
    return pathNode;
}

void PathNodePool::AddPathNode(unsigned key, PathNode* root)
{
    if (hashTable[key])
    {
        PathNode* p = hashTable[key];
        while (true)
        {
            int dir = (root->state < p->state) ? 0 : 1;
            if (p->child[dir])
                p = p->child[dir];
            else
            {
                p->child[dir] = root;
                break;
            }
        }
    }
    else
        hashTable[key] = root;
}

PathNode* PathNodePool::FetchPathNode(void* state)
{
    unsigned int key = Hash(state);
    PathNode* root = hashTable[key];
    while (root)
    {
        if (root->state == state)
            break;
        root = (state < root->state) ? root->child[0] : root->child[1];
    }
    MPASSERT(root)
    return root;
}

PathNode* PathNodePool::GetPathNode(unsigned int frame, void* _state, float _costFromStart, float _estToGoal, PathNode* _parent)
{
    unsigned key = Hash(_state);
    PathNode* root = hashTable[key];
    while (root)
    {
        if (root->state == _state)
        {
            if (root->frame == frame)
                break;
            root->Init(frame, _state, _costFromStart, _estToGoal, _parent);
            break;
        }
        root = _state < root->state ? root->child[0] : root->child[1];
    }
    if (!root)
    {
        root = Alloc();
        root->Clear();
        root->Init(frame, _state, _costFromStart, _estToGoal, _parent);
        AddPathNode(key, root);
    }
    return root;
}

void PathNode::Init(unsigned _frame, void* _state, float _costFromStart, float _estToGoal, PathNode* _parent)
{
    state = _state;
    costFromStart = _costFromStart;
    estToGoal = _estToGoal;
    CalcTotalCost();
    parent = _parent;
    frame = _frame;
    inOpen = false;
    inClosed = false;
}

void PathNode::Clear()
{
    memset(this, 0, sizeof(PathNode));
    numAdjacent = -1;
    cacheIndex = -1;
}

MicroPather::MicroPather(Graph* _graph, unsigned allocate, unsigned typicalAdjacent, bool cache)
    : pathNodePool(allocate, typicalAdjacent),
      graph(_graph),
      frame(0)
{
    MPASSERT(allocate);
    MPASSERT(typicalAdjacent);
    pathCache = nullptr;
    if (cache)
        pathCache = new PathCache(allocate * 4);
}

MicroPather::~MicroPather()
{
    delete pathCache;
}

void MicroPather::Reset()
{
    pathNodePool.Clear();
    if (pathCache)
        pathCache->Reset();
    frame = 0;
}

void MicroPather::GoalReached(PathNode* node, void* start, void* end, MP_VECTOR<void*>* _path)
{
    MP_VECTOR<void*>& path = *_path;
    path.clear();

    int count = 1;
    PathNode* it = node;
    while (it->parent)
    {
        ++count;
        it = it->parent;
    }

    if (count < 3)
    {
        path.resize(2);
        path[0] = start;
        path[1] = end;
    }
    else
    {
        path.resize(count);

        path[0] = start;
        path[count - 1] = end;
        count -= 2;
        it = node->parent;

        while (it->parent)
        {
            path[count] = it->state;
            it = it->parent;
            --count;
        }
    }

    if (pathCache)
    {
        costVec.clear();

        PathNode* pn0 = pathNodePool.FetchPathNode(path[0]);
        PathNode* pn1;
        for (unsigned int i = 0; i < path.size() - 1; ++i)
        {
            pn1 = pathNodePool.FetchPathNode(path[i + 1]);
            nodeCostVec.clear();
            GetNodeNeighbors(pn0, &nodeCostVec);
            for (auto& j : nodeCostVec)
            {
                if (j.node == pn1)
                {
                    costVec.push_back(j.cost);
                    break;
                }
            }
            MPASSERT(costVec.size() == i + 1)
            pn0 = pn1;
        }
        pathCache->Add(path, costVec);
    }
}

void MicroPather::GetNodeNeighbors(PathNode* node, MP_VECTOR<NodeCost>* pNodeCost)
{
    if (node->numAdjacent == 0)
    {
        pNodeCost->resize(0);
    }
    else if (node->cacheIndex < 0)
    {
        stateCostVec.resize(0);
        graph->AdjacentCost(node->state, &stateCostVec);

#ifdef DEBUG
        {
            for (unsigned int i = 0; i < stateCostVec.size(); ++i)
            MPASSERT(stateCostVec[i].state != node->state);
        }
#endif

        pNodeCost->resize(stateCostVec.size());
		node->numAdjacent = static_cast< int >( stateCostVec.size( ) );

        if (node->numAdjacent > 0)
        {
			const unsigned int stateCostVecSize = static_cast< unsigned int >( stateCostVec.size( ) );
            const StateCost* stateCostVecPtr = &stateCostVec[0];
            NodeCost* pNodeCostPtr = &(*pNodeCost)[0];

            for (unsigned int i = 0; i < stateCostVecSize; ++i)
            {
                void* state = stateCostVecPtr[i].state;
                pNodeCostPtr[i].cost = stateCostVecPtr[i].cost;
                pNodeCostPtr[i].node = pathNodePool.GetPathNode(frame, state, FLT_MAX, FLT_MAX, nullptr);
            }

            int start = 0;
			if ( !pNodeCost->empty( ) && pathNodePool.PushCache( pNodeCostPtr, static_cast< int >( pNodeCost->size( ) ), &start ) )
                node->cacheIndex = start;
        }
    }
    else
    {
        pNodeCost->resize(node->numAdjacent);
        NodeCost* pNodeCostPtr = &(*pNodeCost)[0];
        pathNodePool.GetCache(node->cacheIndex, node->numAdjacent, pNodeCostPtr);

        for (int i = 0; i < node->numAdjacent; ++i)
        {
            PathNode* pNode = pNodeCostPtr[i].node;
            if (pNode->frame != frame)
                pNode->Init(frame, pNode->state, FLT_MAX, FLT_MAX, nullptr);
        }
    }
}

void MicroPather::StatesInPool(MP_VECTOR<void*>* stateVec)
{
    stateVec->clear();
    pathNodePool.AllStates(frame, stateVec);
}

void PathNodePool::AllStates(unsigned int frame, MP_VECTOR<void*>* stateVec)
{
    for (Block* b = blocks; b; b = b->nextBlock)
        for (unsigned int i = 0; i < allocate; ++i)
            if (b->pathNode[i].frame == frame)
                stateVec->push_back(b->pathNode[i].state);
}

PathCache::PathCache(int _allocated)
{
    mem = new Item[_allocated];
    memset(mem, 0, sizeof(*mem) * _allocated);
    allocated = _allocated;
    nItems = 0;
    hit = 0;
    miss = 0;
}

PathCache::~PathCache()
{
    delete[] mem;
}

void PathCache::Reset()
{
    if (nItems)
    {
        memset(mem, 0, sizeof(*mem) * allocated);
        nItems = 0;
        hit = 0;
        miss = 0;
    }
}

void PathCache::Add(const MP_VECTOR<void*>& path, const MP_VECTOR<float>& cost)
{
    if (nItems + static_cast<int>(path.size()) > allocated * 3 / 4)
        return;

    for (unsigned int i = 0; i < path.size() - 1; ++i)
    {
        void* end = path[path.size() - 1];
        Item item = { path[i], end, path[i + 1], cost[i] };
        AddItem(item);
    }
}

void PathCache::AddNoSolution(void* end, void* states[], int count)
{
    if (count + nItems > allocated * 3 / 4)
        return;

    for (int i = 0; i < count; ++i)
    {
        Item item = { states[i], end, nullptr, FLT_MAX };
        AddItem(item);
    }
}

int PathCache::Solve(void* start, void* end, MP_VECTOR<void*>* path, float* totalCost)
{
    const Item* item = Find(start, end);
    if (item)
    {
        if (item->cost == FLT_MAX)
        {
            ++hit;
            return MicroPather::NO_SOLUTION;
        }

        path->clear();
        path->push_back(start);
        *totalCost = 0;

        for (; start != end; start = item->next, item = Find(start, end))
        {
            MPASSERT(item)
            *totalCost += item->cost;
            path->push_back(item->next);
        }
        ++hit;
        return MicroPather::SOLVED;
    }
    ++miss;
    return MicroPather::NOT_CACHED;
}

void PathCache::AddItem(const Item& item)
{
    MPASSERT(allocated)
    unsigned int index = item.Hash() % allocated;
    while (true)
    {
        if (mem[index].Empty())
        {
            mem[index] = item;
            ++nItems;
            break;
        }
        else if (mem[index].KeyEqual(item))
        {
            MPASSERT((mem[index].next && item.next) || (mem[index].next == 0 && item.next == 0))
            break;
        }
        ++index;
        if (index == allocated)
            index = 0;
    }
}

const PathCache::Item* PathCache::Find(void* start, void* end)
{
    MPASSERT(allocated)
    Item fake = { start, end, nullptr, 0 };
    unsigned int index = fake.Hash() % allocated;
    while (true)
    {
        if (mem[index].Empty())
            return nullptr;
        if (mem[index].KeyEqual(fake))
            return mem + index;
        ++index;
        if (index == allocated)
            index = 0;
    }
}

void MicroPather::GetCacheData(CacheData* data)
{
    *data = {};

    if (pathCache)
    {
        data->nBytesAllocated = pathCache->AllocatedBytes();
        data->nBytesUsed = pathCache->UsedBytes();
        data->memoryFraction =
            static_cast<float>(static_cast<double>(data->nBytesUsed) / static_cast<double>(data->nBytesAllocated));

        data->hit = pathCache->hit;
        data->miss = pathCache->miss;
        if (data->hit + data->miss)
            data->hitFraction =
                static_cast<float>(static_cast<double>(data->hit) / static_cast<double>(data->hit + data->miss));
        else
            data->hitFraction = 0;
    }
}

int MicroPather::Solve(void* startNode, void* endNode, MP_VECTOR<void*>* path, float* cost)
{
    path->clear();

    *cost = 0.0f;

    if (startNode == endNode)
        return START_END_SAME;

    if (pathCache)
    {
        int cacheResult = pathCache->Solve(startNode, endNode, path, cost);
        if (cacheResult == SOLVED || cacheResult == NO_SOLUTION)
        {
            return cacheResult;
        }
    }

    ++frame;

    OpenQueue open(graph);
    ClosedSet closed(graph);

    PathNode* newPathNode =
        pathNodePool.GetPathNode(frame, startNode, 0, graph->LeastCostEstimate(startNode, endNode), nullptr);

    open.Push(newPathNode);
    stateCostVec.resize(0);
    nodeCostVec.resize(0);

    while (!open.Empty())
    {
        PathNode* node = open.Pop();

        if (node->state == endNode)
        {
            GoalReached(node, startNode, endNode, path);
            *cost = node->costFromStart;
            return SOLVED;
        }
        else
        {
            closed.Add(node);

            GetNodeNeighbors(node, &nodeCostVec);

            for (int i = 0; i < node->numAdjacent; ++i)
            {
                if (nodeCostVec[i].cost == FLT_MAX)
                    continue;
                PathNode* child = nodeCostVec[i].node;
                float newCost = node->costFromStart + nodeCostVec[i].cost;

                PathNode* inOpen = child->inOpen ? child : nullptr;
                PathNode* inClosed = child->inClosed ? child : nullptr;
                auto inEither = reinterpret_cast<PathNode*>(reinterpret_cast<MP_UPTR>(inOpen)
                    | reinterpret_cast<MP_UPTR>(inClosed));

                MPASSERT(inEither != node)
                MPASSERT(!(inOpen && inClosed))

                if (inEither)
                {
                    if (newCost < child->costFromStart)
                    {
                        child->parent = node;
                        child->costFromStart = newCost;
                        child->estToGoal = graph->LeastCostEstimate(child->state, endNode);
                        child->CalcTotalCost();
                        if (inOpen)
                            open.Update(child);
                    }
                }
                else
                {
                    child->parent = node;
                    child->costFromStart = newCost;
                    child->estToGoal = graph->LeastCostEstimate(child->state, endNode),
                        child->CalcTotalCost();

                    MPASSERT(!child->inOpen && !child->inClosed)
                    open.Push(child);
                }
            }
        }
    }
    if (pathCache)
    {
        pathCache->AddNoSolution(endNode, &startNode, 1);
    }
    return NO_SOLUTION;
}

int MicroPather::SolveForNearStates(void* startState, MP_VECTOR<StateCost>* near, float maxCost)
{
    ++frame;

    OpenQueue open(graph);
    ClosedSet closed(graph);

    nodeCostVec.resize(0);
    stateCostVec.resize(0);

    PathNode closedSentinel{};
    closedSentinel.Clear();
    closedSentinel.Init(frame, nullptr, FLT_MAX, FLT_MAX, nullptr);
    closedSentinel.next = closedSentinel.prev = &closedSentinel;

    PathNode* newPathNode = pathNodePool.GetPathNode(frame, startState, 0, 0, nullptr);
    open.Push(newPathNode);

    while (!open.Empty())
    {
        PathNode* node = open.Pop();
        closed.Add(node);
        closedSentinel.AddBefore(node);

        if (node->totalCost > maxCost)
            continue;

        GetNodeNeighbors(node, &nodeCostVec);

        for (int i = 0; i < node->numAdjacent; ++i)
        {
            MPASSERT(node->costFromStart < FLT_MAX)
            float newCost = node->costFromStart + nodeCostVec[i].cost;

            PathNode* inOpen = nodeCostVec[i].node->inOpen ? nodeCostVec[i].node : nullptr;
            PathNode* inClosed = nodeCostVec[i].node->inClosed ? nodeCostVec[i].node : nullptr;
            MPASSERT(!(inOpen && inClosed))
            PathNode* inEither = inOpen ? inOpen : inClosed;
            MPASSERT(inEither != node)

            if (inEither && inEither->costFromStart <= newCost)
                continue;
            PathNode* child = nodeCostVec[i].node;
            MPASSERT(child->state != newPathNode->state)

            child->parent = node;
            child->costFromStart = newCost;
            child->estToGoal = 0;
            child->totalCost = child->costFromStart;

            if (inOpen)
                open.Update(inOpen);
            else if (!inClosed)
                open.Push(child);
        }
    }
    near->clear();

    for (PathNode* pNode = closedSentinel.next; pNode != &closedSentinel; pNode = pNode->next)
    {
        if (pNode->totalCost <= maxCost)
        {
            StateCost sc{};
            sc.cost = pNode->totalCost;
            sc.state = pNode->state;

            near->push_back(sc);
        }
    }
#ifdef DEBUG
    for (unsigned int i = 0; i < near->size(); ++i)
        for (unsigned int k = i + 1; k < near->size(); ++k)
            MPASSERT((*near)[i].state != (*near)[k].state)
#endif

    return SOLVED;
}
