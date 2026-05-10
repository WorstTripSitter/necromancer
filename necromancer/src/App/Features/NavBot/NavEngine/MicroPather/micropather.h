/*
Copyright (c) 2000-2013 Lee Thomason (www.grinninglizard.com)
Micropather

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

#pragma once

#include <vector>
#include <cfloat>

#define MP_VECTOR std::vector

#ifdef _DEBUG
#ifndef DEBUG
#define DEBUG
#endif
#endif

#ifdef DEBUG
#ifdef _MSC_VER
#define MPASSERT(x)           if (!((void)0,(x))) { __debugbreak(); }
#else
#include <cassert>
#define MPASSERT              assert
#endif
#else
#define MPASSERT(x)           {}
#endif

#if defined(_MSC_VER) && (_MSC_VER >= 1400)
#include <cstdlib>
typedef uintptr_t		MP_UPTR;
#elif defined (__GNUC__) && (__GNUC__ >= 3)
#include <cstdint>
#include <cstdlib>
typedef uintptr_t MP_UPTR;
#else
typedef unsigned int MP_UPTR;
#endif

namespace micropather
{
	struct StateCost
	{
		void* state;
		float cost;
	};

	class Graph
	{
	public:
		virtual ~Graph() = default;
		virtual float LeastCostEstimate(void* stateStart, void* stateEnd) = 0;
		virtual void AdjacentCost(void* state, MP_VECTOR<micropather::StateCost>* adjacent) = 0;
		virtual void PrintStateInfo(void* state) = 0;
	};

	class PathNode;

	struct NodeCost
	{
		PathNode* node;
		float cost;
	};

	class PathNode
	{
	public:
		void Init(unsigned int _frame, void* _state, float _costFromStart, float _estToGoal, PathNode* _parent);
		void Clear();
		void InitSentinel()
		{
			Clear();
			Init(0, nullptr, FLT_MAX, FLT_MAX, nullptr);
			prev = next = this;
		}

		void* state;
		float costFromStart;
		float estToGoal;
		float totalCost;
		PathNode* parent;
		unsigned int frame;

		int numAdjacent;
		int cacheIndex;

		PathNode* child[2];
		PathNode* next, * prev;

		bool inOpen;
		bool inClosed;

		void Unlink()
		{
			next->prev = prev;
			prev->next = next;
			next = prev = nullptr;
		}

		void AddBefore(PathNode* addThis)
		{
			addThis->next = this;
			addThis->prev = prev;
			prev->next = addThis;
			prev = addThis;
		}

#ifdef DEBUG
		void CheckList()
		{
			MPASSERT(totalCost == FLT_MAX)
			for (PathNode* it = next; it != this; it = it->next)
			{
				MPASSERT(it->prev == this || it->totalCost >= it->prev->totalCost)
				MPASSERT(it->totalCost <= it->next->totalCost)
			}
		}
#endif

		void CalcTotalCost()
		{
			if (costFromStart < FLT_MAX && estToGoal < FLT_MAX)
				totalCost = costFromStart + estToGoal;
			else
				totalCost = FLT_MAX;
		}

	private:
		void operator=(const PathNode&);
	};

	class PathNodePool
	{
	public:
		PathNodePool(unsigned int allocate, unsigned int typicalAdjacent);
		~PathNodePool();

		void Clear();
		PathNode* GetPathNode(unsigned int frame, void* _state, float _costFromStart, float _estToGoal, PathNode* _parent);
		PathNode* FetchPathNode(void* state);
		bool PushCache(const NodeCost* nodes, int nNodes, int* start);
		void GetCache(int start, int nNodes, NodeCost* nodes);
		void AllStates(unsigned int frame, MP_VECTOR<void*>* stateVec);

	private:
		struct Block
		{
			Block* nextBlock;
			PathNode pathNode[1];
		};

		unsigned int Hash(void* voidval);
		unsigned int HashSize() const { return 1 << hashShift; }
		unsigned int HashMask() const { return (1 << hashShift) - 1; }
		void AddPathNode(unsigned int key, PathNode* p);
		Block* NewBlock();
		PathNode* Alloc();

		PathNode** hashTable;
		Block* firstBlock;
		Block* blocks;

		NodeCost* cache;
		int cacheCap;
		int cacheSize;

		PathNode freeMemSentinel{};
		unsigned int allocate;
		unsigned int nAllocated;
		unsigned int nAvailable;

		unsigned int hashShift;
		unsigned int totalCollide;
	};

	class PathCache
	{
	public:
		struct Item
		{
			void* start;
			void* end;

			bool KeyEqual(const Item& item) const { return start == item.start && end == item.end; }
			bool Empty() const { return start == nullptr && end == nullptr; }

			void* next;
			float cost;

			unsigned int Hash() const
			{
				const auto* p = reinterpret_cast<const unsigned char*>(&start);
				unsigned int h = 2166136261U;
				for (unsigned int i = 0; i < sizeof(void*) * 2; ++i, ++p)
				{
					h ^= *p;
					h *= 16777619;
				}
				return h;
			}
		};

		explicit PathCache(int itemsToAllocate);
		~PathCache();

		void Reset();
		void Add(const MP_VECTOR<void*>& path, const MP_VECTOR<float>& cost);
		void AddNoSolution(void* end, void* states[], int count);
		int Solve(void* startState, void* endState, MP_VECTOR<void*>* path, float* totalCost);

		int AllocatedBytes() const { return allocated * sizeof(Item); }
		int UsedBytes() const { return nItems * sizeof(Item); }

		int hit;
		int miss;

	private:
		void AddItem(const Item& item);
		const Item* Find(void* start, void* end);

		Item* mem;
		unsigned int allocated;
		unsigned int nItems;
	};

	struct CacheData
	{
		CacheData() : nBytesAllocated(0), nBytesUsed(0), memoryFraction(0), hit(0), miss(0), hitFraction(0) {}
		int nBytesAllocated;
		int nBytesUsed;
		float memoryFraction;
		int hit;
		int miss;
		float hitFraction;
	};

	class MicroPather
	{
		friend class micropather::PathNode;

	public:
		enum
		{
			SOLVED,
			NO_SOLUTION,
			START_END_SAME,
			NOT_CACHED
		};

		explicit MicroPather(Graph* graph, unsigned int allocate = 250, unsigned int typicalAdjacent = 6, bool cache = true);
		~MicroPather();

		int Solve(void* startState, void* endState, MP_VECTOR<void*>* path, float* totalCost);
		int SolveForNearStates(void* startState, MP_VECTOR<StateCost>* near, float maxCost);
		void Reset();
		void StatesInPool(MP_VECTOR<void*>* stateVec);
		void GetCacheData(CacheData* data);

	private:
		void GoalReached(PathNode* node, void* start, void* end, MP_VECTOR<void*>* path);
		void GetNodeNeighbors(PathNode* node, MP_VECTOR<NodeCost>* pNodeCost);

		PathNodePool pathNodePool;
		MP_VECTOR<StateCost> stateCostVec;
		MP_VECTOR<NodeCost> nodeCostVec;
		MP_VECTOR<float> costVec;

		Graph* graph;
		unsigned int frame;
		PathCache* pathCache;
	};
}
