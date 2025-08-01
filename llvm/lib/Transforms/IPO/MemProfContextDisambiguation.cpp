//==-- MemProfContextDisambiguation.cpp - Disambiguate contexts -------------=//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file implements support for context disambiguation of allocation
// calls for profile guided heap optimization. Specifically, it uses Memprof
// profiles which indicate context specific allocation behavior (currently
// distinguishing cold vs hot memory allocations). Cloning is performed to
// expose the cold allocation call contexts, and the allocation calls are
// subsequently annotated with an attribute for later transformation.
//
// The transformations can be performed either directly on IR (regular LTO), or
// on a ThinLTO index (and later applied to the IR during the ThinLTO backend).
// Both types of LTO operate on a the same base graph representation, which
// uses CRTP to support either IR or Index formats.
//
//===----------------------------------------------------------------------===//

#include "llvm/Transforms/IPO/MemProfContextDisambiguation.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/MapVector.h"
#include "llvm/ADT/SetOperations.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/Analysis/MemoryProfileInfo.h"
#include "llvm/Analysis/ModuleSummaryAnalysis.h"
#include "llvm/Analysis/OptimizationRemarkEmitter.h"
#include "llvm/Bitcode/BitcodeReader.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/ModuleSummaryIndex.h"
#include "llvm/Pass.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/GraphWriter.h"
#include "llvm/Support/InterleavedRange.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/IPO.h"
#include "llvm/Transforms/Utils/CallPromotionUtils.h"
#include "llvm/Transforms/Utils/Cloning.h"
#include "llvm/Transforms/Utils/Instrumentation.h"
#include <deque>
#include <sstream>
#include <unordered_map>
#include <vector>
using namespace llvm;
using namespace llvm::memprof;

#define DEBUG_TYPE "memprof-context-disambiguation"

STATISTIC(FunctionClonesAnalysis,
          "Number of function clones created during whole program analysis");
STATISTIC(FunctionClonesThinBackend,
          "Number of function clones created during ThinLTO backend");
STATISTIC(FunctionsClonedThinBackend,
          "Number of functions that had clones created during ThinLTO backend");
STATISTIC(AllocTypeNotCold, "Number of not cold static allocations (possibly "
                            "cloned) during whole program analysis");
STATISTIC(AllocTypeCold, "Number of cold static allocations (possibly cloned) "
                         "during whole program analysis");
STATISTIC(AllocTypeNotColdThinBackend,
          "Number of not cold static allocations (possibly cloned) during "
          "ThinLTO backend");
STATISTIC(AllocTypeColdThinBackend, "Number of cold static allocations "
                                    "(possibly cloned) during ThinLTO backend");
STATISTIC(OrigAllocsThinBackend,
          "Number of original (not cloned) allocations with memprof profiles "
          "during ThinLTO backend");
STATISTIC(
    AllocVersionsThinBackend,
    "Number of allocation versions (including clones) during ThinLTO backend");
STATISTIC(MaxAllocVersionsThinBackend,
          "Maximum number of allocation versions created for an original "
          "allocation during ThinLTO backend");
STATISTIC(UnclonableAllocsThinBackend,
          "Number of unclonable ambigous allocations during ThinLTO backend");
STATISTIC(RemovedEdgesWithMismatchedCallees,
          "Number of edges removed due to mismatched callees (profiled vs IR)");
STATISTIC(FoundProfiledCalleeCount,
          "Number of profiled callees found via tail calls");
STATISTIC(FoundProfiledCalleeDepth,
          "Aggregate depth of profiled callees found via tail calls");
STATISTIC(FoundProfiledCalleeMaxDepth,
          "Maximum depth of profiled callees found via tail calls");
STATISTIC(FoundProfiledCalleeNonUniquelyCount,
          "Number of profiled callees found via multiple tail call chains");
STATISTIC(DeferredBackedges, "Number of backedges with deferred cloning");
STATISTIC(NewMergedNodes, "Number of new nodes created during merging");
STATISTIC(NonNewMergedNodes, "Number of non new nodes used during merging");
STATISTIC(MissingAllocForContextId,
          "Number of missing alloc nodes for context ids");
STATISTIC(SkippedCallsCloning,
          "Number of calls skipped during cloning due to unexpected operand");
STATISTIC(MismatchedCloneAssignments,
          "Number of callsites assigned to call multiple non-matching clones");

static cl::opt<std::string> DotFilePathPrefix(
    "memprof-dot-file-path-prefix", cl::init(""), cl::Hidden,
    cl::value_desc("filename"),
    cl::desc("Specify the path prefix of the MemProf dot files."));

static cl::opt<bool> ExportToDot("memprof-export-to-dot", cl::init(false),
                                 cl::Hidden,
                                 cl::desc("Export graph to dot files."));

// How much of the graph to export to dot.
enum DotScope {
  All,     // The full CCG graph.
  Alloc,   // Only contexts for the specified allocation.
  Context, // Only the specified context.
};

static cl::opt<DotScope> DotGraphScope(
    "memprof-dot-scope", cl::desc("Scope of graph to export to dot"),
    cl::Hidden, cl::init(DotScope::All),
    cl::values(
        clEnumValN(DotScope::All, "all", "Export full callsite graph"),
        clEnumValN(DotScope::Alloc, "alloc",
                   "Export only nodes with contexts feeding given "
                   "-memprof-dot-alloc-id"),
        clEnumValN(DotScope::Context, "context",
                   "Export only nodes with given -memprof-dot-context-id")));

static cl::opt<unsigned>
    AllocIdForDot("memprof-dot-alloc-id", cl::init(0), cl::Hidden,
                  cl::desc("Id of alloc to export if -memprof-dot-scope=alloc "
                           "or to highlight if -memprof-dot-scope=all"));

static cl::opt<unsigned> ContextIdForDot(
    "memprof-dot-context-id", cl::init(0), cl::Hidden,
    cl::desc("Id of context to export if -memprof-dot-scope=context or to "
             "highlight otherwise"));

static cl::opt<bool>
    DumpCCG("memprof-dump-ccg", cl::init(false), cl::Hidden,
            cl::desc("Dump CallingContextGraph to stdout after each stage."));

static cl::opt<bool>
    VerifyCCG("memprof-verify-ccg", cl::init(false), cl::Hidden,
              cl::desc("Perform verification checks on CallingContextGraph."));

static cl::opt<bool>
    VerifyNodes("memprof-verify-nodes", cl::init(false), cl::Hidden,
                cl::desc("Perform frequent verification checks on nodes."));

static cl::opt<std::string> MemProfImportSummary(
    "memprof-import-summary",
    cl::desc("Import summary to use for testing the ThinLTO backend via opt"),
    cl::Hidden);

static cl::opt<unsigned>
    TailCallSearchDepth("memprof-tail-call-search-depth", cl::init(5),
                        cl::Hidden,
                        cl::desc("Max depth to recursively search for missing "
                                 "frames through tail calls."));

// Optionally enable cloning of callsites involved with recursive cycles
static cl::opt<bool> AllowRecursiveCallsites(
    "memprof-allow-recursive-callsites", cl::init(true), cl::Hidden,
    cl::desc("Allow cloning of callsites involved in recursive cycles"));

static cl::opt<bool> CloneRecursiveContexts(
    "memprof-clone-recursive-contexts", cl::init(true), cl::Hidden,
    cl::desc("Allow cloning of contexts through recursive cycles"));

// Generally this is needed for correct assignment of allocation clones to
// function clones, however, allow it to be disabled for debugging while the
// functionality is new and being tested more widely.
static cl::opt<bool>
    MergeClones("memprof-merge-clones", cl::init(true), cl::Hidden,
                cl::desc("Merge clones before assigning functions"));

// When disabled, try to detect and prevent cloning of recursive contexts.
// This is only necessary until we support cloning through recursive cycles.
// Leave on by default for now, as disabling requires a little bit of compile
// time overhead and doesn't affect correctness, it will just inflate the cold
// hinted bytes reporting a bit when -memprof-report-hinted-sizes is enabled.
static cl::opt<bool> AllowRecursiveContexts(
    "memprof-allow-recursive-contexts", cl::init(true), cl::Hidden,
    cl::desc("Allow cloning of contexts having recursive cycles"));

// Set the minimum absolute count threshold for allowing inlining of indirect
// calls promoted during cloning.
static cl::opt<unsigned> MemProfICPNoInlineThreshold(
    "memprof-icp-noinline-threshold", cl::init(2), cl::Hidden,
    cl::desc("Minimum absolute count for promoted target to be inlinable"));

namespace llvm {
cl::opt<bool> EnableMemProfContextDisambiguation(
    "enable-memprof-context-disambiguation", cl::init(false), cl::Hidden,
    cl::ZeroOrMore, cl::desc("Enable MemProf context disambiguation"));

// Indicate we are linking with an allocator that supports hot/cold operator
// new interfaces.
cl::opt<bool> SupportsHotColdNew(
    "supports-hot-cold-new", cl::init(false), cl::Hidden,
    cl::desc("Linking with hot/cold operator new interfaces"));

static cl::opt<bool> MemProfRequireDefinitionForPromotion(
    "memprof-require-definition-for-promotion", cl::init(false), cl::Hidden,
    cl::desc(
        "Require target function definition when promoting indirect calls"));
} // namespace llvm

extern cl::opt<bool> MemProfReportHintedSizes;
extern cl::opt<unsigned> MinClonedColdBytePercent;

namespace {
/// CRTP base for graphs built from either IR or ThinLTO summary index.
///
/// The graph represents the call contexts in all memprof metadata on allocation
/// calls, with nodes for the allocations themselves, as well as for the calls
/// in each context. The graph is initially built from the allocation memprof
/// metadata (or summary) MIBs. It is then updated to match calls with callsite
/// metadata onto the nodes, updating it to reflect any inlining performed on
/// those calls.
///
/// Each MIB (representing an allocation's call context with allocation
/// behavior) is assigned a unique context id during the graph build. The edges
/// and nodes in the graph are decorated with the context ids they carry. This
/// is used to correctly update the graph when cloning is performed so that we
/// can uniquify the context for a single (possibly cloned) allocation.
template <typename DerivedCCG, typename FuncTy, typename CallTy>
class CallsiteContextGraph {
public:
  CallsiteContextGraph() = default;
  CallsiteContextGraph(const CallsiteContextGraph &) = default;
  CallsiteContextGraph(CallsiteContextGraph &&) = default;

  /// Main entry point to perform analysis and transformations on graph.
  bool process();

  /// Perform cloning on the graph necessary to uniquely identify the allocation
  /// behavior of an allocation based on its context.
  void identifyClones();

  /// Assign callsite clones to functions, cloning functions as needed to
  /// accommodate the combinations of their callsite clones reached by callers.
  /// For regular LTO this clones functions and callsites in the IR, but for
  /// ThinLTO the cloning decisions are noted in the summaries and later applied
  /// in applyImport.
  bool assignFunctions();

  void dump() const;
  void print(raw_ostream &OS) const;
  void printTotalSizes(raw_ostream &OS) const;

  friend raw_ostream &operator<<(raw_ostream &OS,
                                 const CallsiteContextGraph &CCG) {
    CCG.print(OS);
    return OS;
  }

  friend struct GraphTraits<
      const CallsiteContextGraph<DerivedCCG, FuncTy, CallTy> *>;
  friend struct DOTGraphTraits<
      const CallsiteContextGraph<DerivedCCG, FuncTy, CallTy> *>;

  void exportToDot(std::string Label) const;

  /// Represents a function clone via FuncTy pointer and clone number pair.
  struct FuncInfo final
      : public std::pair<FuncTy *, unsigned /*Clone number*/> {
    using Base = std::pair<FuncTy *, unsigned>;
    FuncInfo(const Base &B) : Base(B) {}
    FuncInfo(FuncTy *F = nullptr, unsigned CloneNo = 0) : Base(F, CloneNo) {}
    explicit operator bool() const { return this->first != nullptr; }
    FuncTy *func() const { return this->first; }
    unsigned cloneNo() const { return this->second; }
  };

  /// Represents a callsite clone via CallTy and clone number pair.
  struct CallInfo final : public std::pair<CallTy, unsigned /*Clone number*/> {
    using Base = std::pair<CallTy, unsigned>;
    CallInfo(const Base &B) : Base(B) {}
    CallInfo(CallTy Call = nullptr, unsigned CloneNo = 0)
        : Base(Call, CloneNo) {}
    explicit operator bool() const { return (bool)this->first; }
    CallTy call() const { return this->first; }
    unsigned cloneNo() const { return this->second; }
    void setCloneNo(unsigned N) { this->second = N; }
    void print(raw_ostream &OS) const {
      if (!operator bool()) {
        assert(!cloneNo());
        OS << "null Call";
        return;
      }
      call()->print(OS);
      OS << "\t(clone " << cloneNo() << ")";
    }
    void dump() const {
      print(dbgs());
      dbgs() << "\n";
    }
    friend raw_ostream &operator<<(raw_ostream &OS, const CallInfo &Call) {
      Call.print(OS);
      return OS;
    }
  };

  struct ContextEdge;

  /// Node in the Callsite Context Graph
  struct ContextNode {
    // Keep this for now since in the IR case where we have an Instruction* it
    // is not as immediately discoverable. Used for printing richer information
    // when dumping graph.
    bool IsAllocation;

    // Keeps track of when the Call was reset to null because there was
    // recursion.
    bool Recursive = false;

    // This will be formed by ORing together the AllocationType enum values
    // for contexts including this node.
    uint8_t AllocTypes = 0;

    // The corresponding allocation or interior call. This is the primary call
    // for which we have created this node.
    CallInfo Call;

    // List of other calls that can be treated the same as the primary call
    // through cloning. I.e. located in the same function and have the same
    // (possibly pruned) stack ids. They will be updated the same way as the
    // primary call when assigning to function clones.
    SmallVector<CallInfo, 0> MatchingCalls;

    // For alloc nodes this is a unique id assigned when constructed, and for
    // callsite stack nodes it is the original stack id when the node is
    // constructed from the memprof MIB metadata on the alloc nodes. Note that
    // this is only used when matching callsite metadata onto the stack nodes
    // created when processing the allocation memprof MIBs, and for labeling
    // nodes in the dot graph. Therefore we don't bother to assign a value for
    // clones.
    uint64_t OrigStackOrAllocId = 0;

    // Edges to all callees in the profiled call stacks.
    // TODO: Should this be a map (from Callee node) for more efficient lookup?
    std::vector<std::shared_ptr<ContextEdge>> CalleeEdges;

    // Edges to all callers in the profiled call stacks.
    // TODO: Should this be a map (from Caller node) for more efficient lookup?
    std::vector<std::shared_ptr<ContextEdge>> CallerEdges;

    // Returns true if we need to look at the callee edges for determining the
    // node context ids and allocation type.
    bool useCallerEdgesForContextInfo() const {
      // Typically if the callee edges are empty either the caller edges are
      // also empty, or this is an allocation (leaf node). However, if we are
      // allowing recursive callsites and contexts this will be violated for
      // incompletely cloned recursive cycles.
      assert(!CalleeEdges.empty() || CallerEdges.empty() || IsAllocation ||
             (AllowRecursiveCallsites && AllowRecursiveContexts));
      // When cloning for a recursive context, during cloning we might be in the
      // midst of cloning for a recurrence and have moved context ids off of a
      // caller edge onto the clone but not yet off of the incoming caller
      // (back) edge. If we don't look at those we miss the fact that this node
      // still has context ids of interest.
      return IsAllocation || CloneRecursiveContexts;
    }

    // Compute the context ids for this node from the union of its edge context
    // ids.
    DenseSet<uint32_t> getContextIds() const {
      unsigned Count = 0;
      // Compute the number of ids for reserve below. In general we only need to
      // look at one set of edges, typically the callee edges, since other than
      // allocations and in some cases during recursion cloning, all the context
      // ids on the callers should also flow out via callee edges.
      for (auto &Edge : CalleeEdges.empty() ? CallerEdges : CalleeEdges)
        Count += Edge->getContextIds().size();
      DenseSet<uint32_t> ContextIds;
      ContextIds.reserve(Count);
      auto Edges = llvm::concat<const std::shared_ptr<ContextEdge>>(
          CalleeEdges, useCallerEdgesForContextInfo()
                           ? CallerEdges
                           : std::vector<std::shared_ptr<ContextEdge>>());
      for (const auto &Edge : Edges)
        ContextIds.insert_range(Edge->getContextIds());
      return ContextIds;
    }

    // Compute the allocation type for this node from the OR of its edge
    // allocation types.
    uint8_t computeAllocType() const {
      uint8_t BothTypes =
          (uint8_t)AllocationType::Cold | (uint8_t)AllocationType::NotCold;
      uint8_t AllocType = (uint8_t)AllocationType::None;
      auto Edges = llvm::concat<const std::shared_ptr<ContextEdge>>(
          CalleeEdges, useCallerEdgesForContextInfo()
                           ? CallerEdges
                           : std::vector<std::shared_ptr<ContextEdge>>());
      for (const auto &Edge : Edges) {
        AllocType |= Edge->AllocTypes;
        // Bail early if alloc type reached both, no further refinement.
        if (AllocType == BothTypes)
          return AllocType;
      }
      return AllocType;
    }

    // The context ids set for this node is empty if its edge context ids are
    // also all empty.
    bool emptyContextIds() const {
      auto Edges = llvm::concat<const std::shared_ptr<ContextEdge>>(
          CalleeEdges, useCallerEdgesForContextInfo()
                           ? CallerEdges
                           : std::vector<std::shared_ptr<ContextEdge>>());
      for (const auto &Edge : Edges) {
        if (!Edge->getContextIds().empty())
          return false;
      }
      return true;
    }

    // List of clones of this ContextNode, initially empty.
    std::vector<ContextNode *> Clones;

    // If a clone, points to the original uncloned node.
    ContextNode *CloneOf = nullptr;

    ContextNode(bool IsAllocation) : IsAllocation(IsAllocation), Call() {}

    ContextNode(bool IsAllocation, CallInfo C)
        : IsAllocation(IsAllocation), Call(C) {}

    void addClone(ContextNode *Clone) {
      if (CloneOf) {
        CloneOf->Clones.push_back(Clone);
        Clone->CloneOf = CloneOf;
      } else {
        Clones.push_back(Clone);
        assert(!Clone->CloneOf);
        Clone->CloneOf = this;
      }
    }

    ContextNode *getOrigNode() {
      if (!CloneOf)
        return this;
      return CloneOf;
    }

    void addOrUpdateCallerEdge(ContextNode *Caller, AllocationType AllocType,
                               unsigned int ContextId);

    ContextEdge *findEdgeFromCallee(const ContextNode *Callee);
    ContextEdge *findEdgeFromCaller(const ContextNode *Caller);
    void eraseCalleeEdge(const ContextEdge *Edge);
    void eraseCallerEdge(const ContextEdge *Edge);

    void setCall(CallInfo C) { Call = C; }

    bool hasCall() const { return (bool)Call.call(); }

    void printCall(raw_ostream &OS) const { Call.print(OS); }

    // True if this node was effectively removed from the graph, in which case
    // it should have an allocation type of None and empty context ids.
    bool isRemoved() const {
      // Typically if the callee edges are empty either the caller edges are
      // also empty, or this is an allocation (leaf node). However, if we are
      // allowing recursive callsites and contexts this will be violated for
      // incompletely cloned recursive cycles.
      assert((AllowRecursiveCallsites && AllowRecursiveContexts) ||
             (AllocTypes == (uint8_t)AllocationType::None) ==
                 emptyContextIds());
      return AllocTypes == (uint8_t)AllocationType::None;
    }

    void dump() const;
    void print(raw_ostream &OS) const;

    friend raw_ostream &operator<<(raw_ostream &OS, const ContextNode &Node) {
      Node.print(OS);
      return OS;
    }
  };

  /// Edge in the Callsite Context Graph from a ContextNode N to a caller or
  /// callee.
  struct ContextEdge {
    ContextNode *Callee;
    ContextNode *Caller;

    // This will be formed by ORing together the AllocationType enum values
    // for contexts including this edge.
    uint8_t AllocTypes = 0;

    // Set just before initiating cloning when cloning of recursive contexts is
    // enabled. Used to defer cloning of backedges until we have done cloning of
    // the callee node for non-backedge caller edges. This exposes cloning
    // opportunities through the backedge of the cycle.
    // TODO: Note that this is not updated during cloning, and it is unclear
    // whether that would be needed.
    bool IsBackedge = false;

    // The set of IDs for contexts including this edge.
    DenseSet<uint32_t> ContextIds;

    ContextEdge(ContextNode *Callee, ContextNode *Caller, uint8_t AllocType,
                DenseSet<uint32_t> ContextIds)
        : Callee(Callee), Caller(Caller), AllocTypes(AllocType),
          ContextIds(std::move(ContextIds)) {}

    DenseSet<uint32_t> &getContextIds() { return ContextIds; }

    // Helper to clear the fields of this edge when we are removing it from the
    // graph.
    inline void clear() {
      ContextIds.clear();
      AllocTypes = (uint8_t)AllocationType::None;
      Caller = nullptr;
      Callee = nullptr;
    }

    // Check if edge was removed from the graph. This is useful while iterating
    // over a copy of edge lists when performing operations that mutate the
    // graph in ways that might remove one of the edges.
    inline bool isRemoved() const {
      if (Callee || Caller)
        return false;
      // Any edges that have been removed from the graph but are still in a
      // shared_ptr somewhere should have all fields null'ed out by clear()
      // above.
      assert(AllocTypes == (uint8_t)AllocationType::None);
      assert(ContextIds.empty());
      return true;
    }

    void dump() const;
    void print(raw_ostream &OS) const;

    friend raw_ostream &operator<<(raw_ostream &OS, const ContextEdge &Edge) {
      Edge.print(OS);
      return OS;
    }
  };

  /// Helpers to remove edges that have allocation type None (due to not
  /// carrying any context ids) after transformations.
  void removeNoneTypeCalleeEdges(ContextNode *Node);
  void removeNoneTypeCallerEdges(ContextNode *Node);
  void
  recursivelyRemoveNoneTypeCalleeEdges(ContextNode *Node,
                                       DenseSet<const ContextNode *> &Visited);

protected:
  /// Get a list of nodes corresponding to the stack ids in the given callsite
  /// context.
  template <class NodeT, class IteratorT>
  std::vector<uint64_t>
  getStackIdsWithContextNodes(CallStack<NodeT, IteratorT> &CallsiteContext);

  /// Adds nodes for the given allocation and any stack ids on its memprof MIB
  /// metadata (or summary).
  ContextNode *addAllocNode(CallInfo Call, const FuncTy *F);

  /// Adds nodes for the given MIB stack ids.
  template <class NodeT, class IteratorT>
  void addStackNodesForMIB(ContextNode *AllocNode,
                           CallStack<NodeT, IteratorT> &StackContext,
                           CallStack<NodeT, IteratorT> &CallsiteContext,
                           AllocationType AllocType,
                           ArrayRef<ContextTotalSize> ContextSizeInfo);

  /// Matches all callsite metadata (or summary) to the nodes created for
  /// allocation memprof MIB metadata, synthesizing new nodes to reflect any
  /// inlining performed on those callsite instructions.
  void updateStackNodes();

  /// Update graph to conservatively handle any callsite stack nodes that target
  /// multiple different callee target functions.
  void handleCallsitesWithMultipleTargets();

  /// Mark backedges via the standard DFS based backedge algorithm.
  void markBackedges();

  /// Merge clones generated during cloning for different allocations but that
  /// are called by the same caller node, to ensure proper function assignment.
  void mergeClones();

  // Try to partition calls on the given node (already placed into the AllCalls
  // array) by callee function, creating new copies of Node as needed to hold
  // calls with different callees, and moving the callee edges appropriately.
  // Returns true if partitioning was successful.
  bool partitionCallsByCallee(
      ContextNode *Node, ArrayRef<CallInfo> AllCalls,
      std::vector<std::pair<CallInfo, ContextNode *>> &NewCallToNode);

  /// Save lists of calls with MemProf metadata in each function, for faster
  /// iteration.
  MapVector<FuncTy *, std::vector<CallInfo>> FuncToCallsWithMetadata;

  /// Map from callsite node to the enclosing caller function.
  std::map<const ContextNode *, const FuncTy *> NodeToCallingFunc;

  // When exporting to dot, and an allocation id is specified, contains the
  // context ids on that allocation.
  DenseSet<uint32_t> DotAllocContextIds;

private:
  using EdgeIter = typename std::vector<std::shared_ptr<ContextEdge>>::iterator;

  // Structure to keep track of information for each call as we are matching
  // non-allocation callsites onto context nodes created from the allocation
  // call metadata / summary contexts.
  struct CallContextInfo {
    // The callsite we're trying to match.
    CallTy Call;
    // The callsites stack ids that have a context node in the graph.
    std::vector<uint64_t> StackIds;
    // The function containing this callsite.
    const FuncTy *Func;
    // Initially empty, if needed this will be updated to contain the context
    // ids for use in a new context node created for this callsite.
    DenseSet<uint32_t> ContextIds;
  };

  /// Helper to remove edge from graph, updating edge iterator if it is provided
  /// (in which case CalleeIter indicates which edge list is being iterated).
  /// This will also perform the necessary clearing of the ContextEdge members
  /// to enable later checking if the edge has been removed (since we may have
  /// other copies of the shared_ptr in existence, and in fact rely on this to
  /// enable removal while iterating over a copy of a node's edge list).
  void removeEdgeFromGraph(ContextEdge *Edge, EdgeIter *EI = nullptr,
                           bool CalleeIter = true);

  /// Assigns the given Node to calls at or inlined into the location with
  /// the Node's stack id, after post order traversing and processing its
  /// caller nodes. Uses the call information recorded in the given
  /// StackIdToMatchingCalls map, and creates new nodes for inlined sequences
  /// as needed. Called by updateStackNodes which sets up the given
  /// StackIdToMatchingCalls map.
  void assignStackNodesPostOrder(
      ContextNode *Node, DenseSet<const ContextNode *> &Visited,
      DenseMap<uint64_t, std::vector<CallContextInfo>> &StackIdToMatchingCalls,
      DenseMap<CallInfo, CallInfo> &CallToMatchingCall);

  /// Duplicates the given set of context ids, updating the provided
  /// map from each original id with the newly generated context ids,
  /// and returning the new duplicated id set.
  DenseSet<uint32_t> duplicateContextIds(
      const DenseSet<uint32_t> &StackSequenceContextIds,
      DenseMap<uint32_t, DenseSet<uint32_t>> &OldToNewContextIds);

  /// Propagates all duplicated context ids across the graph.
  void propagateDuplicateContextIds(
      const DenseMap<uint32_t, DenseSet<uint32_t>> &OldToNewContextIds);

  /// Connect the NewNode to OrigNode's callees if TowardsCallee is true,
  /// else to its callers. Also updates OrigNode's edges to remove any context
  /// ids moved to the newly created edge.
  void connectNewNode(ContextNode *NewNode, ContextNode *OrigNode,
                      bool TowardsCallee,
                      DenseSet<uint32_t> RemainingContextIds);

  /// Get the stack id corresponding to the given Id or Index (for IR this will
  /// return itself, for a summary index this will return the id recorded in the
  /// index for that stack id index value).
  uint64_t getStackId(uint64_t IdOrIndex) const {
    return static_cast<const DerivedCCG *>(this)->getStackId(IdOrIndex);
  }

  /// Returns true if the given call targets the callee of the given edge, or if
  /// we were able to identify the call chain through intermediate tail calls.
  /// In the latter case new context nodes are added to the graph for the
  /// identified tail calls, and their synthesized nodes are added to
  /// TailCallToContextNodeMap. The EdgeIter is updated in the latter case for
  /// the updated edges and to prepare it for an increment in the caller.
  bool
  calleesMatch(CallTy Call, EdgeIter &EI,
               MapVector<CallInfo, ContextNode *> &TailCallToContextNodeMap);

  // Return the callee function of the given call, or nullptr if it can't be
  // determined
  const FuncTy *getCalleeFunc(CallTy Call) {
    return static_cast<DerivedCCG *>(this)->getCalleeFunc(Call);
  }

  /// Returns true if the given call targets the given function, or if we were
  /// able to identify the call chain through intermediate tail calls (in which
  /// case FoundCalleeChain will be populated).
  bool calleeMatchesFunc(
      CallTy Call, const FuncTy *Func, const FuncTy *CallerFunc,
      std::vector<std::pair<CallTy, FuncTy *>> &FoundCalleeChain) {
    return static_cast<DerivedCCG *>(this)->calleeMatchesFunc(
        Call, Func, CallerFunc, FoundCalleeChain);
  }

  /// Returns true if both call instructions have the same callee.
  bool sameCallee(CallTy Call1, CallTy Call2) {
    return static_cast<DerivedCCG *>(this)->sameCallee(Call1, Call2);
  }

  /// Get a list of nodes corresponding to the stack ids in the given
  /// callsite's context.
  std::vector<uint64_t> getStackIdsWithContextNodesForCall(CallTy Call) {
    return static_cast<DerivedCCG *>(this)->getStackIdsWithContextNodesForCall(
        Call);
  }

  /// Get the last stack id in the context for callsite.
  uint64_t getLastStackId(CallTy Call) {
    return static_cast<DerivedCCG *>(this)->getLastStackId(Call);
  }

  /// Update the allocation call to record type of allocated memory.
  void updateAllocationCall(CallInfo &Call, AllocationType AllocType) {
    AllocType == AllocationType::Cold ? AllocTypeCold++ : AllocTypeNotCold++;
    static_cast<DerivedCCG *>(this)->updateAllocationCall(Call, AllocType);
  }

  /// Get the AllocationType assigned to the given allocation instruction clone.
  AllocationType getAllocationCallType(const CallInfo &Call) const {
    return static_cast<const DerivedCCG *>(this)->getAllocationCallType(Call);
  }

  /// Update non-allocation call to invoke (possibly cloned) function
  /// CalleeFunc.
  void updateCall(CallInfo &CallerCall, FuncInfo CalleeFunc) {
    static_cast<DerivedCCG *>(this)->updateCall(CallerCall, CalleeFunc);
  }

  /// Clone the given function for the given callsite, recording mapping of all
  /// of the functions tracked calls to their new versions in the CallMap.
  /// Assigns new clones to clone number CloneNo.
  FuncInfo cloneFunctionForCallsite(
      FuncInfo &Func, CallInfo &Call, std::map<CallInfo, CallInfo> &CallMap,
      std::vector<CallInfo> &CallsWithMetadataInFunc, unsigned CloneNo) {
    return static_cast<DerivedCCG *>(this)->cloneFunctionForCallsite(
        Func, Call, CallMap, CallsWithMetadataInFunc, CloneNo);
  }

  /// Gets a label to use in the dot graph for the given call clone in the given
  /// function.
  std::string getLabel(const FuncTy *Func, const CallTy Call,
                       unsigned CloneNo) const {
    return static_cast<const DerivedCCG *>(this)->getLabel(Func, Call, CloneNo);
  }

  // Create and return a new ContextNode.
  ContextNode *createNewNode(bool IsAllocation, const FuncTy *F = nullptr,
                             CallInfo C = CallInfo()) {
    NodeOwner.push_back(std::make_unique<ContextNode>(IsAllocation, C));
    auto *NewNode = NodeOwner.back().get();
    if (F)
      NodeToCallingFunc[NewNode] = F;
    return NewNode;
  }

  /// Helpers to find the node corresponding to the given call or stackid.
  ContextNode *getNodeForInst(const CallInfo &C);
  ContextNode *getNodeForAlloc(const CallInfo &C);
  ContextNode *getNodeForStackId(uint64_t StackId);

  /// Computes the alloc type corresponding to the given context ids, by
  /// unioning their recorded alloc types.
  uint8_t computeAllocType(DenseSet<uint32_t> &ContextIds) const;

  /// Returns the allocation type of the intersection of the contexts of two
  /// nodes (based on their provided context id sets), optimized for the case
  /// when Node1Ids is smaller than Node2Ids.
  uint8_t intersectAllocTypesImpl(const DenseSet<uint32_t> &Node1Ids,
                                  const DenseSet<uint32_t> &Node2Ids) const;

  /// Returns the allocation type of the intersection of the contexts of two
  /// nodes (based on their provided context id sets).
  uint8_t intersectAllocTypes(const DenseSet<uint32_t> &Node1Ids,
                              const DenseSet<uint32_t> &Node2Ids) const;

  /// Create a clone of Edge's callee and move Edge to that new callee node,
  /// performing the necessary context id and allocation type updates.
  /// If ContextIdsToMove is non-empty, only that subset of Edge's ids are
  /// moved to an edge to the new callee.
  ContextNode *
  moveEdgeToNewCalleeClone(const std::shared_ptr<ContextEdge> &Edge,
                           DenseSet<uint32_t> ContextIdsToMove = {});

  /// Change the callee of Edge to existing callee clone NewCallee, performing
  /// the necessary context id and allocation type updates.
  /// If ContextIdsToMove is non-empty, only that subset of Edge's ids are
  /// moved to an edge to the new callee.
  void moveEdgeToExistingCalleeClone(const std::shared_ptr<ContextEdge> &Edge,
                                     ContextNode *NewCallee,
                                     bool NewClone = false,
                                     DenseSet<uint32_t> ContextIdsToMove = {});

  /// Change the caller of the edge at the given callee edge iterator to be
  /// NewCaller, performing the necessary context id and allocation type
  /// updates. This is similar to the above moveEdgeToExistingCalleeClone, but
  /// a simplified version of it as we always move the given edge and all of its
  /// context ids.
  void moveCalleeEdgeToNewCaller(const std::shared_ptr<ContextEdge> &Edge,
                                 ContextNode *NewCaller);

  /// Recursive helper for marking backedges via DFS.
  void markBackedges(ContextNode *Node, DenseSet<const ContextNode *> &Visited,
                     DenseSet<const ContextNode *> &CurrentStack);

  /// Recursive helper for merging clones.
  void
  mergeClones(ContextNode *Node, DenseSet<const ContextNode *> &Visited,
              DenseMap<uint32_t, ContextNode *> &ContextIdToAllocationNode);
  /// Main worker for merging callee clones for a given node.
  void mergeNodeCalleeClones(
      ContextNode *Node, DenseSet<const ContextNode *> &Visited,
      DenseMap<uint32_t, ContextNode *> &ContextIdToAllocationNode);
  /// Helper to find other callers of the given set of callee edges that can
  /// share the same callee merge node.
  void findOtherCallersToShareMerge(
      ContextNode *Node, std::vector<std::shared_ptr<ContextEdge>> &CalleeEdges,
      DenseMap<uint32_t, ContextNode *> &ContextIdToAllocationNode,
      DenseSet<ContextNode *> &OtherCallersToShareMerge);

  /// Recursively perform cloning on the graph for the given Node and its
  /// callers, in order to uniquely identify the allocation behavior of an
  /// allocation given its context. The context ids of the allocation being
  /// processed are given in AllocContextIds.
  void identifyClones(ContextNode *Node, DenseSet<const ContextNode *> &Visited,
                      const DenseSet<uint32_t> &AllocContextIds);

  /// Map from each context ID to the AllocationType assigned to that context.
  DenseMap<uint32_t, AllocationType> ContextIdToAllocationType;

  /// Map from each contextID to the profiled full contexts and their total
  /// sizes (there may be more than one due to context trimming),
  /// optionally populated when requested (via MemProfReportHintedSizes or
  /// MinClonedColdBytePercent).
  DenseMap<uint32_t, std::vector<ContextTotalSize>> ContextIdToContextSizeInfos;

  /// Identifies the context node created for a stack id when adding the MIB
  /// contexts to the graph. This is used to locate the context nodes when
  /// trying to assign the corresponding callsites with those stack ids to these
  /// nodes.
  DenseMap<uint64_t, ContextNode *> StackEntryIdToContextNodeMap;

  /// Maps to track the calls to their corresponding nodes in the graph.
  MapVector<CallInfo, ContextNode *> AllocationCallToContextNodeMap;
  MapVector<CallInfo, ContextNode *> NonAllocationCallToContextNodeMap;

  /// Owner of all ContextNode unique_ptrs.
  std::vector<std::unique_ptr<ContextNode>> NodeOwner;

  /// Perform sanity checks on graph when requested.
  void check() const;

  /// Keeps track of the last unique context id assigned.
  unsigned int LastContextId = 0;
};

template <typename DerivedCCG, typename FuncTy, typename CallTy>
using ContextNode =
    typename CallsiteContextGraph<DerivedCCG, FuncTy, CallTy>::ContextNode;
template <typename DerivedCCG, typename FuncTy, typename CallTy>
using ContextEdge =
    typename CallsiteContextGraph<DerivedCCG, FuncTy, CallTy>::ContextEdge;
template <typename DerivedCCG, typename FuncTy, typename CallTy>
using FuncInfo =
    typename CallsiteContextGraph<DerivedCCG, FuncTy, CallTy>::FuncInfo;
template <typename DerivedCCG, typename FuncTy, typename CallTy>
using CallInfo =
    typename CallsiteContextGraph<DerivedCCG, FuncTy, CallTy>::CallInfo;

/// CRTP derived class for graphs built from IR (regular LTO).
class ModuleCallsiteContextGraph
    : public CallsiteContextGraph<ModuleCallsiteContextGraph, Function,
                                  Instruction *> {
public:
  ModuleCallsiteContextGraph(
      Module &M,
      llvm::function_ref<OptimizationRemarkEmitter &(Function *)> OREGetter);

private:
  friend CallsiteContextGraph<ModuleCallsiteContextGraph, Function,
                              Instruction *>;

  uint64_t getStackId(uint64_t IdOrIndex) const;
  const Function *getCalleeFunc(Instruction *Call);
  bool calleeMatchesFunc(
      Instruction *Call, const Function *Func, const Function *CallerFunc,
      std::vector<std::pair<Instruction *, Function *>> &FoundCalleeChain);
  bool sameCallee(Instruction *Call1, Instruction *Call2);
  bool findProfiledCalleeThroughTailCalls(
      const Function *ProfiledCallee, Value *CurCallee, unsigned Depth,
      std::vector<std::pair<Instruction *, Function *>> &FoundCalleeChain,
      bool &FoundMultipleCalleeChains);
  uint64_t getLastStackId(Instruction *Call);
  std::vector<uint64_t> getStackIdsWithContextNodesForCall(Instruction *Call);
  void updateAllocationCall(CallInfo &Call, AllocationType AllocType);
  AllocationType getAllocationCallType(const CallInfo &Call) const;
  void updateCall(CallInfo &CallerCall, FuncInfo CalleeFunc);
  CallsiteContextGraph<ModuleCallsiteContextGraph, Function,
                       Instruction *>::FuncInfo
  cloneFunctionForCallsite(FuncInfo &Func, CallInfo &Call,
                           std::map<CallInfo, CallInfo> &CallMap,
                           std::vector<CallInfo> &CallsWithMetadataInFunc,
                           unsigned CloneNo);
  std::string getLabel(const Function *Func, const Instruction *Call,
                       unsigned CloneNo) const;

  const Module &Mod;
  llvm::function_ref<OptimizationRemarkEmitter &(Function *)> OREGetter;
};

/// Represents a call in the summary index graph, which can either be an
/// allocation or an interior callsite node in an allocation's context.
/// Holds a pointer to the corresponding data structure in the index.
struct IndexCall : public PointerUnion<CallsiteInfo *, AllocInfo *> {
  IndexCall() : PointerUnion() {}
  IndexCall(std::nullptr_t) : IndexCall() {}
  IndexCall(CallsiteInfo *StackNode) : PointerUnion(StackNode) {}
  IndexCall(AllocInfo *AllocNode) : PointerUnion(AllocNode) {}
  IndexCall(PointerUnion PT) : PointerUnion(PT) {}

  IndexCall *operator->() { return this; }

  void print(raw_ostream &OS) const {
    PointerUnion<CallsiteInfo *, AllocInfo *> Base = *this;
    if (auto *AI = llvm::dyn_cast_if_present<AllocInfo *>(Base)) {
      OS << *AI;
    } else {
      auto *CI = llvm::dyn_cast_if_present<CallsiteInfo *>(Base);
      assert(CI);
      OS << *CI;
    }
  }
};
} // namespace

namespace llvm {
template <> struct simplify_type<IndexCall> {
  using SimpleType = PointerUnion<CallsiteInfo *, AllocInfo *>;
  static SimpleType getSimplifiedValue(IndexCall &Val) { return Val; }
};
template <> struct simplify_type<const IndexCall> {
  using SimpleType = const PointerUnion<CallsiteInfo *, AllocInfo *>;
  static SimpleType getSimplifiedValue(const IndexCall &Val) { return Val; }
};
} // namespace llvm

namespace {
/// CRTP derived class for graphs built from summary index (ThinLTO).
class IndexCallsiteContextGraph
    : public CallsiteContextGraph<IndexCallsiteContextGraph, FunctionSummary,
                                  IndexCall> {
public:
  IndexCallsiteContextGraph(
      ModuleSummaryIndex &Index,
      llvm::function_ref<bool(GlobalValue::GUID, const GlobalValueSummary *)>
          isPrevailing);

  ~IndexCallsiteContextGraph() {
    // Now that we are done with the graph it is safe to add the new
    // CallsiteInfo structs to the function summary vectors. The graph nodes
    // point into locations within these vectors, so we don't want to add them
    // any earlier.
    for (auto &I : FunctionCalleesToSynthesizedCallsiteInfos) {
      auto *FS = I.first;
      for (auto &Callsite : I.second)
        FS->addCallsite(*Callsite.second);
    }
  }

private:
  friend CallsiteContextGraph<IndexCallsiteContextGraph, FunctionSummary,
                              IndexCall>;

  uint64_t getStackId(uint64_t IdOrIndex) const;
  const FunctionSummary *getCalleeFunc(IndexCall &Call);
  bool calleeMatchesFunc(
      IndexCall &Call, const FunctionSummary *Func,
      const FunctionSummary *CallerFunc,
      std::vector<std::pair<IndexCall, FunctionSummary *>> &FoundCalleeChain);
  bool sameCallee(IndexCall &Call1, IndexCall &Call2);
  bool findProfiledCalleeThroughTailCalls(
      ValueInfo ProfiledCallee, ValueInfo CurCallee, unsigned Depth,
      std::vector<std::pair<IndexCall, FunctionSummary *>> &FoundCalleeChain,
      bool &FoundMultipleCalleeChains);
  uint64_t getLastStackId(IndexCall &Call);
  std::vector<uint64_t> getStackIdsWithContextNodesForCall(IndexCall &Call);
  void updateAllocationCall(CallInfo &Call, AllocationType AllocType);
  AllocationType getAllocationCallType(const CallInfo &Call) const;
  void updateCall(CallInfo &CallerCall, FuncInfo CalleeFunc);
  CallsiteContextGraph<IndexCallsiteContextGraph, FunctionSummary,
                       IndexCall>::FuncInfo
  cloneFunctionForCallsite(FuncInfo &Func, CallInfo &Call,
                           std::map<CallInfo, CallInfo> &CallMap,
                           std::vector<CallInfo> &CallsWithMetadataInFunc,
                           unsigned CloneNo);
  std::string getLabel(const FunctionSummary *Func, const IndexCall &Call,
                       unsigned CloneNo) const;

  // Saves mapping from function summaries containing memprof records back to
  // its VI, for use in checking and debugging.
  std::map<const FunctionSummary *, ValueInfo> FSToVIMap;

  const ModuleSummaryIndex &Index;
  llvm::function_ref<bool(GlobalValue::GUID, const GlobalValueSummary *)>
      isPrevailing;

  // Saves/owns the callsite info structures synthesized for missing tail call
  // frames that we discover while building the graph.
  // It maps from the summary of the function making the tail call, to a map
  // of callee ValueInfo to corresponding synthesized callsite info.
  std::unordered_map<FunctionSummary *,
                     std::map<ValueInfo, std::unique_ptr<CallsiteInfo>>>
      FunctionCalleesToSynthesizedCallsiteInfos;
};
} // namespace

namespace llvm {
template <>
struct DenseMapInfo<typename CallsiteContextGraph<
    ModuleCallsiteContextGraph, Function, Instruction *>::CallInfo>
    : public DenseMapInfo<std::pair<Instruction *, unsigned>> {};
template <>
struct DenseMapInfo<typename CallsiteContextGraph<
    IndexCallsiteContextGraph, FunctionSummary, IndexCall>::CallInfo>
    : public DenseMapInfo<std::pair<IndexCall, unsigned>> {};
template <>
struct DenseMapInfo<IndexCall>
    : public DenseMapInfo<PointerUnion<CallsiteInfo *, AllocInfo *>> {};
} // end namespace llvm

namespace {

// Map the uint8_t alloc types (which may contain NotCold|Cold) to the alloc
// type we should actually use on the corresponding allocation.
// If we can't clone a node that has NotCold+Cold alloc type, we will fall
// back to using NotCold. So don't bother cloning to distinguish NotCold+Cold
// from NotCold.
AllocationType allocTypeToUse(uint8_t AllocTypes) {
  assert(AllocTypes != (uint8_t)AllocationType::None);
  if (AllocTypes ==
      ((uint8_t)AllocationType::NotCold | (uint8_t)AllocationType::Cold))
    return AllocationType::NotCold;
  else
    return (AllocationType)AllocTypes;
}

// Helper to check if the alloc types for all edges recorded in the
// InAllocTypes vector match the alloc types for all edges in the Edges
// vector.
template <typename DerivedCCG, typename FuncTy, typename CallTy>
bool allocTypesMatch(
    const std::vector<uint8_t> &InAllocTypes,
    const std::vector<std::shared_ptr<ContextEdge<DerivedCCG, FuncTy, CallTy>>>
        &Edges) {
  // This should be called only when the InAllocTypes vector was computed for
  // this set of Edges. Make sure the sizes are the same.
  assert(InAllocTypes.size() == Edges.size());
  return std::equal(
      InAllocTypes.begin(), InAllocTypes.end(), Edges.begin(), Edges.end(),
      [](const uint8_t &l,
         const std::shared_ptr<ContextEdge<DerivedCCG, FuncTy, CallTy>> &r) {
        // Can share if one of the edges is None type - don't
        // care about the type along that edge as it doesn't
        // exist for those context ids.
        if (l == (uint8_t)AllocationType::None ||
            r->AllocTypes == (uint8_t)AllocationType::None)
          return true;
        return allocTypeToUse(l) == allocTypeToUse(r->AllocTypes);
      });
}

// Helper to check if the alloc types for all edges recorded in the
// InAllocTypes vector match the alloc types for callee edges in the given
// clone. Because the InAllocTypes were computed from the original node's callee
// edges, and other cloning could have happened after this clone was created, we
// need to find the matching clone callee edge, which may or may not exist.
template <typename DerivedCCG, typename FuncTy, typename CallTy>
bool allocTypesMatchClone(
    const std::vector<uint8_t> &InAllocTypes,
    const ContextNode<DerivedCCG, FuncTy, CallTy> *Clone) {
  const ContextNode<DerivedCCG, FuncTy, CallTy> *Node = Clone->CloneOf;
  assert(Node);
  // InAllocTypes should have been computed for the original node's callee
  // edges.
  assert(InAllocTypes.size() == Node->CalleeEdges.size());
  // First create a map of the clone callee edge callees to the edge alloc type.
  DenseMap<const ContextNode<DerivedCCG, FuncTy, CallTy> *, uint8_t>
      EdgeCalleeMap;
  for (const auto &E : Clone->CalleeEdges) {
    assert(!EdgeCalleeMap.contains(E->Callee));
    EdgeCalleeMap[E->Callee] = E->AllocTypes;
  }
  // Next, walk the original node's callees, and look for the corresponding
  // clone edge to that callee.
  for (unsigned I = 0; I < Node->CalleeEdges.size(); I++) {
    auto Iter = EdgeCalleeMap.find(Node->CalleeEdges[I]->Callee);
    // Not found is ok, we will simply add an edge if we use this clone.
    if (Iter == EdgeCalleeMap.end())
      continue;
    // Can share if one of the edges is None type - don't
    // care about the type along that edge as it doesn't
    // exist for those context ids.
    if (InAllocTypes[I] == (uint8_t)AllocationType::None ||
        Iter->second == (uint8_t)AllocationType::None)
      continue;
    if (allocTypeToUse(Iter->second) != allocTypeToUse(InAllocTypes[I]))
      return false;
  }
  return true;
}

} // end anonymous namespace

template <typename DerivedCCG, typename FuncTy, typename CallTy>
typename CallsiteContextGraph<DerivedCCG, FuncTy, CallTy>::ContextNode *
CallsiteContextGraph<DerivedCCG, FuncTy, CallTy>::getNodeForInst(
    const CallInfo &C) {
  ContextNode *Node = getNodeForAlloc(C);
  if (Node)
    return Node;

  return NonAllocationCallToContextNodeMap.lookup(C);
}

template <typename DerivedCCG, typename FuncTy, typename CallTy>
typename CallsiteContextGraph<DerivedCCG, FuncTy, CallTy>::ContextNode *
CallsiteContextGraph<DerivedCCG, FuncTy, CallTy>::getNodeForAlloc(
    const CallInfo &C) {
  return AllocationCallToContextNodeMap.lookup(C);
}

template <typename DerivedCCG, typename FuncTy, typename CallTy>
typename CallsiteContextGraph<DerivedCCG, FuncTy, CallTy>::ContextNode *
CallsiteContextGraph<DerivedCCG, FuncTy, CallTy>::getNodeForStackId(
    uint64_t StackId) {
  auto StackEntryNode = StackEntryIdToContextNodeMap.find(StackId);
  if (StackEntryNode != StackEntryIdToContextNodeMap.end())
    return StackEntryNode->second;
  return nullptr;
}

template <typename DerivedCCG, typename FuncTy, typename CallTy>
void CallsiteContextGraph<DerivedCCG, FuncTy, CallTy>::ContextNode::
    addOrUpdateCallerEdge(ContextNode *Caller, AllocationType AllocType,
                          unsigned int ContextId) {
  for (auto &Edge : CallerEdges) {
    if (Edge->Caller == Caller) {
      Edge->AllocTypes |= (uint8_t)AllocType;
      Edge->getContextIds().insert(ContextId);
      return;
    }
  }
  std::shared_ptr<ContextEdge> Edge = std::make_shared<ContextEdge>(
      this, Caller, (uint8_t)AllocType, DenseSet<uint32_t>({ContextId}));
  CallerEdges.push_back(Edge);
  Caller->CalleeEdges.push_back(Edge);
}

template <typename DerivedCCG, typename FuncTy, typename CallTy>
void CallsiteContextGraph<DerivedCCG, FuncTy, CallTy>::removeEdgeFromGraph(
    ContextEdge *Edge, EdgeIter *EI, bool CalleeIter) {
  assert(!EI || (*EI)->get() == Edge);
  assert(!Edge->isRemoved());
  // Save the Caller and Callee pointers so we can erase Edge from their edge
  // lists after clearing Edge below. We do the clearing first in case it is
  // destructed after removing from the edge lists (if those were the last
  // shared_ptr references to Edge).
  auto *Callee = Edge->Callee;
  auto *Caller = Edge->Caller;

  // Make sure the edge fields are cleared out so we can properly detect
  // removed edges if Edge is not destructed because there is still a shared_ptr
  // reference.
  Edge->clear();

#ifndef NDEBUG
  auto CalleeCallerCount = Callee->CallerEdges.size();
  auto CallerCalleeCount = Caller->CalleeEdges.size();
#endif
  if (!EI) {
    Callee->eraseCallerEdge(Edge);
    Caller->eraseCalleeEdge(Edge);
  } else if (CalleeIter) {
    Callee->eraseCallerEdge(Edge);
    *EI = Caller->CalleeEdges.erase(*EI);
  } else {
    Caller->eraseCalleeEdge(Edge);
    *EI = Callee->CallerEdges.erase(*EI);
  }
  assert(Callee->CallerEdges.size() < CalleeCallerCount);
  assert(Caller->CalleeEdges.size() < CallerCalleeCount);
}

template <typename DerivedCCG, typename FuncTy, typename CallTy>
void CallsiteContextGraph<
    DerivedCCG, FuncTy, CallTy>::removeNoneTypeCalleeEdges(ContextNode *Node) {
  for (auto EI = Node->CalleeEdges.begin(); EI != Node->CalleeEdges.end();) {
    auto Edge = *EI;
    if (Edge->AllocTypes == (uint8_t)AllocationType::None) {
      assert(Edge->ContextIds.empty());
      removeEdgeFromGraph(Edge.get(), &EI, /*CalleeIter=*/true);
    } else
      ++EI;
  }
}

template <typename DerivedCCG, typename FuncTy, typename CallTy>
void CallsiteContextGraph<
    DerivedCCG, FuncTy, CallTy>::removeNoneTypeCallerEdges(ContextNode *Node) {
  for (auto EI = Node->CallerEdges.begin(); EI != Node->CallerEdges.end();) {
    auto Edge = *EI;
    if (Edge->AllocTypes == (uint8_t)AllocationType::None) {
      assert(Edge->ContextIds.empty());
      Edge->Caller->eraseCalleeEdge(Edge.get());
      EI = Node->CallerEdges.erase(EI);
    } else
      ++EI;
  }
}

template <typename DerivedCCG, typename FuncTy, typename CallTy>
typename CallsiteContextGraph<DerivedCCG, FuncTy, CallTy>::ContextEdge *
CallsiteContextGraph<DerivedCCG, FuncTy, CallTy>::ContextNode::
    findEdgeFromCallee(const ContextNode *Callee) {
  for (const auto &Edge : CalleeEdges)
    if (Edge->Callee == Callee)
      return Edge.get();
  return nullptr;
}

template <typename DerivedCCG, typename FuncTy, typename CallTy>
typename CallsiteContextGraph<DerivedCCG, FuncTy, CallTy>::ContextEdge *
CallsiteContextGraph<DerivedCCG, FuncTy, CallTy>::ContextNode::
    findEdgeFromCaller(const ContextNode *Caller) {
  for (const auto &Edge : CallerEdges)
    if (Edge->Caller == Caller)
      return Edge.get();
  return nullptr;
}

template <typename DerivedCCG, typename FuncTy, typename CallTy>
void CallsiteContextGraph<DerivedCCG, FuncTy, CallTy>::ContextNode::
    eraseCalleeEdge(const ContextEdge *Edge) {
  auto EI = llvm::find_if(
      CalleeEdges, [Edge](const std::shared_ptr<ContextEdge> &CalleeEdge) {
        return CalleeEdge.get() == Edge;
      });
  assert(EI != CalleeEdges.end());
  CalleeEdges.erase(EI);
}

template <typename DerivedCCG, typename FuncTy, typename CallTy>
void CallsiteContextGraph<DerivedCCG, FuncTy, CallTy>::ContextNode::
    eraseCallerEdge(const ContextEdge *Edge) {
  auto EI = llvm::find_if(
      CallerEdges, [Edge](const std::shared_ptr<ContextEdge> &CallerEdge) {
        return CallerEdge.get() == Edge;
      });
  assert(EI != CallerEdges.end());
  CallerEdges.erase(EI);
}

template <typename DerivedCCG, typename FuncTy, typename CallTy>
uint8_t CallsiteContextGraph<DerivedCCG, FuncTy, CallTy>::computeAllocType(
    DenseSet<uint32_t> &ContextIds) const {
  uint8_t BothTypes =
      (uint8_t)AllocationType::Cold | (uint8_t)AllocationType::NotCold;
  uint8_t AllocType = (uint8_t)AllocationType::None;
  for (auto Id : ContextIds) {
    AllocType |= (uint8_t)ContextIdToAllocationType.at(Id);
    // Bail early if alloc type reached both, no further refinement.
    if (AllocType == BothTypes)
      return AllocType;
  }
  return AllocType;
}

template <typename DerivedCCG, typename FuncTy, typename CallTy>
uint8_t
CallsiteContextGraph<DerivedCCG, FuncTy, CallTy>::intersectAllocTypesImpl(
    const DenseSet<uint32_t> &Node1Ids,
    const DenseSet<uint32_t> &Node2Ids) const {
  uint8_t BothTypes =
      (uint8_t)AllocationType::Cold | (uint8_t)AllocationType::NotCold;
  uint8_t AllocType = (uint8_t)AllocationType::None;
  for (auto Id : Node1Ids) {
    if (!Node2Ids.count(Id))
      continue;
    AllocType |= (uint8_t)ContextIdToAllocationType.at(Id);
    // Bail early if alloc type reached both, no further refinement.
    if (AllocType == BothTypes)
      return AllocType;
  }
  return AllocType;
}

template <typename DerivedCCG, typename FuncTy, typename CallTy>
uint8_t CallsiteContextGraph<DerivedCCG, FuncTy, CallTy>::intersectAllocTypes(
    const DenseSet<uint32_t> &Node1Ids,
    const DenseSet<uint32_t> &Node2Ids) const {
  if (Node1Ids.size() < Node2Ids.size())
    return intersectAllocTypesImpl(Node1Ids, Node2Ids);
  else
    return intersectAllocTypesImpl(Node2Ids, Node1Ids);
}

template <typename DerivedCCG, typename FuncTy, typename CallTy>
typename CallsiteContextGraph<DerivedCCG, FuncTy, CallTy>::ContextNode *
CallsiteContextGraph<DerivedCCG, FuncTy, CallTy>::addAllocNode(
    CallInfo Call, const FuncTy *F) {
  assert(!getNodeForAlloc(Call));
  ContextNode *AllocNode = createNewNode(/*IsAllocation=*/true, F, Call);
  AllocationCallToContextNodeMap[Call] = AllocNode;
  // Use LastContextId as a uniq id for MIB allocation nodes.
  AllocNode->OrigStackOrAllocId = LastContextId;
  // Alloc type should be updated as we add in the MIBs. We should assert
  // afterwards that it is not still None.
  AllocNode->AllocTypes = (uint8_t)AllocationType::None;

  return AllocNode;
}

static std::string getAllocTypeString(uint8_t AllocTypes) {
  if (!AllocTypes)
    return "None";
  std::string Str;
  if (AllocTypes & (uint8_t)AllocationType::NotCold)
    Str += "NotCold";
  if (AllocTypes & (uint8_t)AllocationType::Cold)
    Str += "Cold";
  return Str;
}

template <typename DerivedCCG, typename FuncTy, typename CallTy>
template <class NodeT, class IteratorT>
void CallsiteContextGraph<DerivedCCG, FuncTy, CallTy>::addStackNodesForMIB(
    ContextNode *AllocNode, CallStack<NodeT, IteratorT> &StackContext,
    CallStack<NodeT, IteratorT> &CallsiteContext, AllocationType AllocType,
    ArrayRef<ContextTotalSize> ContextSizeInfo) {
  // Treating the hot alloc type as NotCold before the disambiguation for "hot"
  // is done.
  if (AllocType == AllocationType::Hot)
    AllocType = AllocationType::NotCold;

  ContextIdToAllocationType[++LastContextId] = AllocType;

  if (!ContextSizeInfo.empty()) {
    auto &Entry = ContextIdToContextSizeInfos[LastContextId];
    Entry.insert(Entry.begin(), ContextSizeInfo.begin(), ContextSizeInfo.end());
  }

  // Update alloc type and context ids for this MIB.
  AllocNode->AllocTypes |= (uint8_t)AllocType;

  // Now add or update nodes for each stack id in alloc's context.
  // Later when processing the stack ids on non-alloc callsites we will adjust
  // for any inlining in the context.
  ContextNode *PrevNode = AllocNode;
  // Look for recursion (direct recursion should have been collapsed by
  // module summary analysis, here we should just be detecting mutual
  // recursion). Mark these nodes so we don't try to clone.
  SmallSet<uint64_t, 8> StackIdSet;
  // Skip any on the allocation call (inlining).
  for (auto ContextIter = StackContext.beginAfterSharedPrefix(CallsiteContext);
       ContextIter != StackContext.end(); ++ContextIter) {
    auto StackId = getStackId(*ContextIter);
    ContextNode *StackNode = getNodeForStackId(StackId);
    if (!StackNode) {
      StackNode = createNewNode(/*IsAllocation=*/false);
      StackEntryIdToContextNodeMap[StackId] = StackNode;
      StackNode->OrigStackOrAllocId = StackId;
    }
    // Marking a node recursive will prevent its cloning completely, even for
    // non-recursive contexts flowing through it.
    if (!AllowRecursiveCallsites) {
      auto Ins = StackIdSet.insert(StackId);
      if (!Ins.second)
        StackNode->Recursive = true;
    }
    StackNode->AllocTypes |= (uint8_t)AllocType;
    PrevNode->addOrUpdateCallerEdge(StackNode, AllocType, LastContextId);
    PrevNode = StackNode;
  }
}

template <typename DerivedCCG, typename FuncTy, typename CallTy>
DenseSet<uint32_t>
CallsiteContextGraph<DerivedCCG, FuncTy, CallTy>::duplicateContextIds(
    const DenseSet<uint32_t> &StackSequenceContextIds,
    DenseMap<uint32_t, DenseSet<uint32_t>> &OldToNewContextIds) {
  DenseSet<uint32_t> NewContextIds;
  for (auto OldId : StackSequenceContextIds) {
    NewContextIds.insert(++LastContextId);
    OldToNewContextIds[OldId].insert(LastContextId);
    assert(ContextIdToAllocationType.count(OldId));
    // The new context has the same allocation type as original.
    ContextIdToAllocationType[LastContextId] = ContextIdToAllocationType[OldId];
    if (DotAllocContextIds.contains(OldId))
      DotAllocContextIds.insert(LastContextId);
  }
  return NewContextIds;
}

template <typename DerivedCCG, typename FuncTy, typename CallTy>
void CallsiteContextGraph<DerivedCCG, FuncTy, CallTy>::
    propagateDuplicateContextIds(
        const DenseMap<uint32_t, DenseSet<uint32_t>> &OldToNewContextIds) {
  // Build a set of duplicated context ids corresponding to the input id set.
  auto GetNewIds = [&OldToNewContextIds](const DenseSet<uint32_t> &ContextIds) {
    DenseSet<uint32_t> NewIds;
    for (auto Id : ContextIds)
      if (auto NewId = OldToNewContextIds.find(Id);
          NewId != OldToNewContextIds.end())
        NewIds.insert_range(NewId->second);
    return NewIds;
  };

  // Recursively update context ids sets along caller edges.
  auto UpdateCallers = [&](ContextNode *Node,
                           DenseSet<const ContextEdge *> &Visited,
                           auto &&UpdateCallers) -> void {
    for (const auto &Edge : Node->CallerEdges) {
      auto Inserted = Visited.insert(Edge.get());
      if (!Inserted.second)
        continue;
      ContextNode *NextNode = Edge->Caller;
      DenseSet<uint32_t> NewIdsToAdd = GetNewIds(Edge->getContextIds());
      // Only need to recursively iterate to NextNode via this caller edge if
      // it resulted in any added ids to NextNode.
      if (!NewIdsToAdd.empty()) {
        Edge->getContextIds().insert_range(NewIdsToAdd);
        UpdateCallers(NextNode, Visited, UpdateCallers);
      }
    }
  };

  DenseSet<const ContextEdge *> Visited;
  for (auto &Entry : AllocationCallToContextNodeMap) {
    auto *Node = Entry.second;
    UpdateCallers(Node, Visited, UpdateCallers);
  }
}

template <typename DerivedCCG, typename FuncTy, typename CallTy>
void CallsiteContextGraph<DerivedCCG, FuncTy, CallTy>::connectNewNode(
    ContextNode *NewNode, ContextNode *OrigNode, bool TowardsCallee,
    // This must be passed by value to make a copy since it will be adjusted
    // as ids are moved.
    DenseSet<uint32_t> RemainingContextIds) {
  auto &OrigEdges =
      TowardsCallee ? OrigNode->CalleeEdges : OrigNode->CallerEdges;
  DenseSet<uint32_t> RecursiveContextIds;
  DenseSet<uint32_t> AllCallerContextIds;
  if (AllowRecursiveCallsites) {
    // Identify which context ids are recursive which is needed to properly
    // update the RemainingContextIds set. The relevant recursive context ids
    // are those that are in multiple edges.
    for (auto &CE : OrigEdges) {
      AllCallerContextIds.reserve(CE->getContextIds().size());
      for (auto Id : CE->getContextIds())
        if (!AllCallerContextIds.insert(Id).second)
          RecursiveContextIds.insert(Id);
    }
  }
  // Increment iterator in loop so that we can remove edges as needed.
  for (auto EI = OrigEdges.begin(); EI != OrigEdges.end();) {
    auto Edge = *EI;
    DenseSet<uint32_t> NewEdgeContextIds;
    DenseSet<uint32_t> NotFoundContextIds;
    // Remove any matching context ids from Edge, return set that were found and
    // removed, these are the new edge's context ids. Also update the remaining
    // (not found ids).
    set_subtract(Edge->getContextIds(), RemainingContextIds, NewEdgeContextIds,
                 NotFoundContextIds);
    // Update the remaining context ids set for the later edges. This is a
    // compile time optimization.
    if (RecursiveContextIds.empty()) {
      // No recursive ids, so all of the previously remaining context ids that
      // were not seen on this edge are the new remaining set.
      RemainingContextIds.swap(NotFoundContextIds);
    } else {
      // Keep the recursive ids in the remaining set as we expect to see those
      // on another edge. We can remove the non-recursive remaining ids that
      // were seen on this edge, however. We already have the set of remaining
      // ids that were on this edge (in NewEdgeContextIds). Figure out which are
      // non-recursive and only remove those. Note that despite the higher
      // overhead of updating the remaining context ids set when recursion
      // handling is enabled, it was found to be at worst performance neutral
      // and in one case a clear win.
      DenseSet<uint32_t> NonRecursiveRemainingCurEdgeIds =
          set_difference(NewEdgeContextIds, RecursiveContextIds);
      set_subtract(RemainingContextIds, NonRecursiveRemainingCurEdgeIds);
    }
    // If no matching context ids for this edge, skip it.
    if (NewEdgeContextIds.empty()) {
      ++EI;
      continue;
    }
    if (TowardsCallee) {
      uint8_t NewAllocType = computeAllocType(NewEdgeContextIds);
      auto NewEdge = std::make_shared<ContextEdge>(
          Edge->Callee, NewNode, NewAllocType, std::move(NewEdgeContextIds));
      NewNode->CalleeEdges.push_back(NewEdge);
      NewEdge->Callee->CallerEdges.push_back(NewEdge);
    } else {
      uint8_t NewAllocType = computeAllocType(NewEdgeContextIds);
      auto NewEdge = std::make_shared<ContextEdge>(
          NewNode, Edge->Caller, NewAllocType, std::move(NewEdgeContextIds));
      NewNode->CallerEdges.push_back(NewEdge);
      NewEdge->Caller->CalleeEdges.push_back(NewEdge);
    }
    // Remove old edge if context ids empty.
    if (Edge->getContextIds().empty()) {
      removeEdgeFromGraph(Edge.get(), &EI, TowardsCallee);
      continue;
    }
    ++EI;
  }
}

template <typename DerivedCCG, typename FuncTy, typename CallTy>
static void checkEdge(
    const std::shared_ptr<ContextEdge<DerivedCCG, FuncTy, CallTy>> &Edge) {
  // Confirm that alloc type is not None and that we have at least one context
  // id.
  assert(Edge->AllocTypes != (uint8_t)AllocationType::None);
  assert(!Edge->ContextIds.empty());
}

template <typename DerivedCCG, typename FuncTy, typename CallTy>
static void checkNode(const ContextNode<DerivedCCG, FuncTy, CallTy> *Node,
                      bool CheckEdges = true) {
  if (Node->isRemoved())
    return;
#ifndef NDEBUG
  // Compute node's context ids once for use in asserts.
  auto NodeContextIds = Node->getContextIds();
#endif
  // Node's context ids should be the union of both its callee and caller edge
  // context ids.
  if (Node->CallerEdges.size()) {
    DenseSet<uint32_t> CallerEdgeContextIds(
        Node->CallerEdges.front()->ContextIds);
    for (const auto &Edge : llvm::drop_begin(Node->CallerEdges)) {
      if (CheckEdges)
        checkEdge<DerivedCCG, FuncTy, CallTy>(Edge);
      set_union(CallerEdgeContextIds, Edge->ContextIds);
    }
    // Node can have more context ids than callers if some contexts terminate at
    // node and some are longer. If we are allowing recursive callsites and
    // contexts this will be violated for incompletely cloned recursive cycles,
    // so skip the checking in that case.
    assert((AllowRecursiveCallsites && AllowRecursiveContexts) ||
           NodeContextIds == CallerEdgeContextIds ||
           set_is_subset(CallerEdgeContextIds, NodeContextIds));
  }
  if (Node->CalleeEdges.size()) {
    DenseSet<uint32_t> CalleeEdgeContextIds(
        Node->CalleeEdges.front()->ContextIds);
    for (const auto &Edge : llvm::drop_begin(Node->CalleeEdges)) {
      if (CheckEdges)
        checkEdge<DerivedCCG, FuncTy, CallTy>(Edge);
      set_union(CalleeEdgeContextIds, Edge->getContextIds());
    }
    // If we are allowing recursive callsites and contexts this will be violated
    // for incompletely cloned recursive cycles, so skip the checking in that
    // case.
    assert((AllowRecursiveCallsites && AllowRecursiveContexts) ||
           NodeContextIds == CalleeEdgeContextIds);
  }
  // FIXME: Since this checking is only invoked under an option, we should
  // change the error checking from using assert to something that will trigger
  // an error on a release build.
#ifndef NDEBUG
  // Make sure we don't end up with duplicate edges between the same caller and
  // callee.
  DenseSet<ContextNode<DerivedCCG, FuncTy, CallTy> *> NodeSet;
  for (const auto &E : Node->CalleeEdges)
    NodeSet.insert(E->Callee);
  assert(NodeSet.size() == Node->CalleeEdges.size());
#endif
}

template <typename DerivedCCG, typename FuncTy, typename CallTy>
void CallsiteContextGraph<DerivedCCG, FuncTy, CallTy>::
    assignStackNodesPostOrder(
        ContextNode *Node, DenseSet<const ContextNode *> &Visited,
        DenseMap<uint64_t, std::vector<CallContextInfo>>
            &StackIdToMatchingCalls,
        DenseMap<CallInfo, CallInfo> &CallToMatchingCall) {
  auto Inserted = Visited.insert(Node);
  if (!Inserted.second)
    return;
  // Post order traversal. Iterate over a copy since we may add nodes and
  // therefore new callers during the recursive call, invalidating any
  // iterator over the original edge vector. We don't need to process these
  // new nodes as they were already processed on creation.
  auto CallerEdges = Node->CallerEdges;
  for (auto &Edge : CallerEdges) {
    // Skip any that have been removed during the recursion.
    if (Edge->isRemoved()) {
      assert(!is_contained(Node->CallerEdges, Edge));
      continue;
    }
    assignStackNodesPostOrder(Edge->Caller, Visited, StackIdToMatchingCalls,
                              CallToMatchingCall);
  }

  // If this node's stack id is in the map, update the graph to contain new
  // nodes representing any inlining at interior callsites. Note we move the
  // associated context ids over to the new nodes.

  // Ignore this node if it is for an allocation or we didn't record any
  // stack id lists ending at it.
  if (Node->IsAllocation ||
      !StackIdToMatchingCalls.count(Node->OrigStackOrAllocId))
    return;

  auto &Calls = StackIdToMatchingCalls[Node->OrigStackOrAllocId];
  // Handle the simple case first. A single call with a single stack id.
  // In this case there is no need to create any new context nodes, simply
  // assign the context node for stack id to this Call.
  if (Calls.size() == 1) {
    auto &[Call, Ids, Func, SavedContextIds] = Calls[0];
    if (Ids.size() == 1) {
      assert(SavedContextIds.empty());
      // It should be this Node
      assert(Node == getNodeForStackId(Ids[0]));
      if (Node->Recursive)
        return;
      Node->setCall(Call);
      NonAllocationCallToContextNodeMap[Call] = Node;
      NodeToCallingFunc[Node] = Func;
      return;
    }
  }

#ifndef NDEBUG
  // Find the node for the last stack id, which should be the same
  // across all calls recorded for this id, and is this node's id.
  uint64_t LastId = Node->OrigStackOrAllocId;
  ContextNode *LastNode = getNodeForStackId(LastId);
  // We should only have kept stack ids that had nodes.
  assert(LastNode);
  assert(LastNode == Node);
#else
  ContextNode *LastNode = Node;
#endif

  // Compute the last node's context ids once, as it is shared by all calls in
  // this entry.
  DenseSet<uint32_t> LastNodeContextIds = LastNode->getContextIds();

  [[maybe_unused]] bool PrevIterCreatedNode = false;
  bool CreatedNode = false;
  for (unsigned I = 0; I < Calls.size();
       I++, PrevIterCreatedNode = CreatedNode) {
    CreatedNode = false;
    auto &[Call, Ids, Func, SavedContextIds] = Calls[I];
    // Skip any for which we didn't assign any ids, these don't get a node in
    // the graph.
    if (SavedContextIds.empty()) {
      // If this call has a matching call (located in the same function and
      // having the same stack ids), simply add it to the context node created
      // for its matching call earlier. These can be treated the same through
      // cloning and get updated at the same time.
      if (!CallToMatchingCall.contains(Call))
        continue;
      auto MatchingCall = CallToMatchingCall[Call];
      if (!NonAllocationCallToContextNodeMap.contains(MatchingCall)) {
        // This should only happen if we had a prior iteration, and it didn't
        // create a node because of the below recomputation of context ids
        // finding none remaining and continuing early.
        assert(I > 0 && !PrevIterCreatedNode);
        continue;
      }
      NonAllocationCallToContextNodeMap[MatchingCall]->MatchingCalls.push_back(
          Call);
      continue;
    }

    assert(LastId == Ids.back());

    // Recompute the context ids for this stack id sequence (the
    // intersection of the context ids of the corresponding nodes).
    // Start with the ids we saved in the map for this call, which could be
    // duplicated context ids. We have to recompute as we might have overlap
    // overlap between the saved context ids for different last nodes, and
    // removed them already during the post order traversal.
    set_intersect(SavedContextIds, LastNodeContextIds);
    ContextNode *PrevNode = LastNode;
    bool Skip = false;
    // Iterate backwards through the stack Ids, starting after the last Id
    // in the list, which was handled once outside for all Calls.
    for (auto IdIter = Ids.rbegin() + 1; IdIter != Ids.rend(); IdIter++) {
      auto Id = *IdIter;
      ContextNode *CurNode = getNodeForStackId(Id);
      // We should only have kept stack ids that had nodes and weren't
      // recursive.
      assert(CurNode);
      assert(!CurNode->Recursive);

      auto *Edge = CurNode->findEdgeFromCaller(PrevNode);
      if (!Edge) {
        Skip = true;
        break;
      }
      PrevNode = CurNode;

      // Update the context ids, which is the intersection of the ids along
      // all edges in the sequence.
      set_intersect(SavedContextIds, Edge->getContextIds());

      // If we now have no context ids for clone, skip this call.
      if (SavedContextIds.empty()) {
        Skip = true;
        break;
      }
    }
    if (Skip)
      continue;

    // Create new context node.
    ContextNode *NewNode = createNewNode(/*IsAllocation=*/false, Func, Call);
    NonAllocationCallToContextNodeMap[Call] = NewNode;
    CreatedNode = true;
    NewNode->AllocTypes = computeAllocType(SavedContextIds);

    ContextNode *FirstNode = getNodeForStackId(Ids[0]);
    assert(FirstNode);

    // Connect to callees of innermost stack frame in inlined call chain.
    // This updates context ids for FirstNode's callee's to reflect those
    // moved to NewNode.
    connectNewNode(NewNode, FirstNode, /*TowardsCallee=*/true, SavedContextIds);

    // Connect to callers of outermost stack frame in inlined call chain.
    // This updates context ids for FirstNode's caller's to reflect those
    // moved to NewNode.
    connectNewNode(NewNode, LastNode, /*TowardsCallee=*/false, SavedContextIds);

    // Now we need to remove context ids from edges/nodes between First and
    // Last Node.
    PrevNode = nullptr;
    for (auto Id : Ids) {
      ContextNode *CurNode = getNodeForStackId(Id);
      // We should only have kept stack ids that had nodes.
      assert(CurNode);

      // Remove the context ids moved to NewNode from CurNode, and the
      // edge from the prior node.
      if (PrevNode) {
        auto *PrevEdge = CurNode->findEdgeFromCallee(PrevNode);
        // If the sequence contained recursion, we might have already removed
        // some edges during the connectNewNode calls above.
        if (!PrevEdge) {
          PrevNode = CurNode;
          continue;
        }
        set_subtract(PrevEdge->getContextIds(), SavedContextIds);
        if (PrevEdge->getContextIds().empty())
          removeEdgeFromGraph(PrevEdge);
      }
      // Since we update the edges from leaf to tail, only look at the callee
      // edges. This isn't an alloc node, so if there are no callee edges, the
      // alloc type is None.
      CurNode->AllocTypes = CurNode->CalleeEdges.empty()
                                ? (uint8_t)AllocationType::None
                                : CurNode->computeAllocType();
      PrevNode = CurNode;
    }
    if (VerifyNodes) {
      checkNode<DerivedCCG, FuncTy, CallTy>(NewNode, /*CheckEdges=*/true);
      for (auto Id : Ids) {
        ContextNode *CurNode = getNodeForStackId(Id);
        // We should only have kept stack ids that had nodes.
        assert(CurNode);
        checkNode<DerivedCCG, FuncTy, CallTy>(CurNode, /*CheckEdges=*/true);
      }
    }
  }
}

template <typename DerivedCCG, typename FuncTy, typename CallTy>
void CallsiteContextGraph<DerivedCCG, FuncTy, CallTy>::updateStackNodes() {
  // Map of stack id to all calls with that as the last (outermost caller)
  // callsite id that has a context node (some might not due to pruning
  // performed during matching of the allocation profile contexts).
  // The CallContextInfo contains the Call and a list of its stack ids with
  // ContextNodes, the function containing Call, and the set of context ids
  // the analysis will eventually identify for use in any new node created
  // for that callsite.
  DenseMap<uint64_t, std::vector<CallContextInfo>> StackIdToMatchingCalls;
  for (auto &[Func, CallsWithMetadata] : FuncToCallsWithMetadata) {
    for (auto &Call : CallsWithMetadata) {
      // Ignore allocations, already handled.
      if (AllocationCallToContextNodeMap.count(Call))
        continue;
      auto StackIdsWithContextNodes =
          getStackIdsWithContextNodesForCall(Call.call());
      // If there were no nodes created for MIBs on allocs (maybe this was in
      // the unambiguous part of the MIB stack that was pruned), ignore.
      if (StackIdsWithContextNodes.empty())
        continue;
      // Otherwise, record this Call along with the list of ids for the last
      // (outermost caller) stack id with a node.
      StackIdToMatchingCalls[StackIdsWithContextNodes.back()].push_back(
          {Call.call(), StackIdsWithContextNodes, Func, {}});
    }
  }

  // First make a pass through all stack ids that correspond to a call,
  // as identified in the above loop. Compute the context ids corresponding to
  // each of these calls when they correspond to multiple stack ids due to
  // due to inlining. Perform any duplication of context ids required when
  // there is more than one call with the same stack ids. Their (possibly newly
  // duplicated) context ids are saved in the StackIdToMatchingCalls map.
  DenseMap<uint32_t, DenseSet<uint32_t>> OldToNewContextIds;
  // Save a map from each call to any that are found to match it. I.e. located
  // in the same function and have the same (possibly pruned) stack ids. We use
  // this to avoid creating extra graph nodes as they can be treated the same.
  DenseMap<CallInfo, CallInfo> CallToMatchingCall;
  for (auto &It : StackIdToMatchingCalls) {
    auto &Calls = It.getSecond();
    // Skip single calls with a single stack id. These don't need a new node.
    if (Calls.size() == 1) {
      auto &Ids = Calls[0].StackIds;
      if (Ids.size() == 1)
        continue;
    }
    // In order to do the best and maximal matching of inlined calls to context
    // node sequences we will sort the vectors of stack ids in descending order
    // of length, and within each length, lexicographically by stack id. The
    // latter is so that we can specially handle calls that have identical stack
    // id sequences (either due to cloning or artificially because of the MIB
    // context pruning). Those with the same Ids are then sorted by function to
    // facilitate efficiently mapping them to the same context node.
    // Because the functions are pointers, to ensure a stable sort first assign
    // each function pointer to its first index in the Calls array, and then use
    // that to sort by.
    DenseMap<const FuncTy *, unsigned> FuncToIndex;
    for (const auto &[Idx, CallCtxInfo] : enumerate(Calls))
      FuncToIndex.insert({CallCtxInfo.Func, Idx});
    llvm::stable_sort(
        Calls,
        [&FuncToIndex](const CallContextInfo &A, const CallContextInfo &B) {
          return A.StackIds.size() > B.StackIds.size() ||
                 (A.StackIds.size() == B.StackIds.size() &&
                  (A.StackIds < B.StackIds ||
                   (A.StackIds == B.StackIds &&
                    FuncToIndex[A.Func] < FuncToIndex[B.Func])));
        });

    // Find the node for the last stack id, which should be the same
    // across all calls recorded for this id, and is the id for this
    // entry in the StackIdToMatchingCalls map.
    uint64_t LastId = It.getFirst();
    ContextNode *LastNode = getNodeForStackId(LastId);
    // We should only have kept stack ids that had nodes.
    assert(LastNode);

    if (LastNode->Recursive)
      continue;

    // Initialize the context ids with the last node's. We will subsequently
    // refine the context ids by computing the intersection along all edges.
    DenseSet<uint32_t> LastNodeContextIds = LastNode->getContextIds();
    assert(!LastNodeContextIds.empty());

#ifndef NDEBUG
    // Save the set of functions seen for a particular set of the same stack
    // ids. This is used to ensure that they have been correctly sorted to be
    // adjacent in the Calls list, since we rely on that to efficiently place
    // all such matching calls onto the same context node.
    DenseSet<const FuncTy *> MatchingIdsFuncSet;
#endif

    for (unsigned I = 0; I < Calls.size(); I++) {
      auto &[Call, Ids, Func, SavedContextIds] = Calls[I];
      assert(SavedContextIds.empty());
      assert(LastId == Ids.back());

#ifndef NDEBUG
      // If this call has a different set of ids than the last one, clear the
      // set used to ensure they are sorted properly.
      if (I > 0 && Ids != Calls[I - 1].StackIds)
        MatchingIdsFuncSet.clear();
#endif

      // First compute the context ids for this stack id sequence (the
      // intersection of the context ids of the corresponding nodes).
      // Start with the remaining saved ids for the last node.
      assert(!LastNodeContextIds.empty());
      DenseSet<uint32_t> StackSequenceContextIds = LastNodeContextIds;

      ContextNode *PrevNode = LastNode;
      ContextNode *CurNode = LastNode;
      bool Skip = false;

      // Iterate backwards through the stack Ids, starting after the last Id
      // in the list, which was handled once outside for all Calls.
      for (auto IdIter = Ids.rbegin() + 1; IdIter != Ids.rend(); IdIter++) {
        auto Id = *IdIter;
        CurNode = getNodeForStackId(Id);
        // We should only have kept stack ids that had nodes.
        assert(CurNode);

        if (CurNode->Recursive) {
          Skip = true;
          break;
        }

        auto *Edge = CurNode->findEdgeFromCaller(PrevNode);
        // If there is no edge then the nodes belong to different MIB contexts,
        // and we should skip this inlined context sequence. For example, this
        // particular inlined context may include stack ids A->B, and we may
        // indeed have nodes for both A and B, but it is possible that they were
        // never profiled in sequence in a single MIB for any allocation (i.e.
        // we might have profiled an allocation that involves the callsite A,
        // but through a different one of its callee callsites, and we might
        // have profiled an allocation that involves callsite B, but reached
        // from a different caller callsite).
        if (!Edge) {
          Skip = true;
          break;
        }
        PrevNode = CurNode;

        // Update the context ids, which is the intersection of the ids along
        // all edges in the sequence.
        set_intersect(StackSequenceContextIds, Edge->getContextIds());

        // If we now have no context ids for clone, skip this call.
        if (StackSequenceContextIds.empty()) {
          Skip = true;
          break;
        }
      }
      if (Skip)
        continue;

      // If some of this call's stack ids did not have corresponding nodes (due
      // to pruning), don't include any context ids for contexts that extend
      // beyond these nodes. Otherwise we would be matching part of unrelated /
      // not fully matching stack contexts. To do this, subtract any context ids
      // found in caller nodes of the last node found above.
      if (Ids.back() != getLastStackId(Call)) {
        for (const auto &PE : LastNode->CallerEdges) {
          set_subtract(StackSequenceContextIds, PE->getContextIds());
          if (StackSequenceContextIds.empty())
            break;
        }
        // If we now have no context ids for clone, skip this call.
        if (StackSequenceContextIds.empty())
          continue;
      }

#ifndef NDEBUG
      // If the prior call had the same stack ids this set would not be empty.
      // Check if we already have a call that "matches" because it is located
      // in the same function. If the Calls list was sorted properly we should
      // not encounter this situation as all such entries should be adjacent
      // and processed in bulk further below.
      assert(!MatchingIdsFuncSet.contains(Func));

      MatchingIdsFuncSet.insert(Func);
#endif

      // Check if the next set of stack ids is the same (since the Calls vector
      // of tuples is sorted by the stack ids we can just look at the next one).
      // If so, save them in the CallToMatchingCall map so that they get
      // assigned to the same context node, and skip them.
      bool DuplicateContextIds = false;
      for (unsigned J = I + 1; J < Calls.size(); J++) {
        auto &CallCtxInfo = Calls[J];
        auto &NextIds = CallCtxInfo.StackIds;
        if (NextIds != Ids)
          break;
        auto *NextFunc = CallCtxInfo.Func;
        if (NextFunc != Func) {
          // We have another Call with the same ids but that cannot share this
          // node, must duplicate ids for it.
          DuplicateContextIds = true;
          break;
        }
        auto &NextCall = CallCtxInfo.Call;
        CallToMatchingCall[NextCall] = Call;
        // Update I so that it gets incremented correctly to skip this call.
        I = J;
      }

      // If we don't have duplicate context ids, then we can assign all the
      // context ids computed for the original node sequence to this call.
      // If there are duplicate calls with the same stack ids then we synthesize
      // new context ids that are duplicates of the originals. These are
      // assigned to SavedContextIds, which is a reference into the map entry
      // for this call, allowing us to access these ids later on.
      OldToNewContextIds.reserve(OldToNewContextIds.size() +
                                 StackSequenceContextIds.size());
      SavedContextIds =
          DuplicateContextIds
              ? duplicateContextIds(StackSequenceContextIds, OldToNewContextIds)
              : StackSequenceContextIds;
      assert(!SavedContextIds.empty());

      if (!DuplicateContextIds) {
        // Update saved last node's context ids to remove those that are
        // assigned to other calls, so that it is ready for the next call at
        // this stack id.
        set_subtract(LastNodeContextIds, StackSequenceContextIds);
        if (LastNodeContextIds.empty())
          break;
      }
    }
  }

  // Propagate the duplicate context ids over the graph.
  propagateDuplicateContextIds(OldToNewContextIds);

  if (VerifyCCG)
    check();

  // Now perform a post-order traversal over the graph, starting with the
  // allocation nodes, essentially processing nodes from callers to callees.
  // For any that contains an id in the map, update the graph to contain new
  // nodes representing any inlining at interior callsites. Note we move the
  // associated context ids over to the new nodes.
  DenseSet<const ContextNode *> Visited;
  for (auto &Entry : AllocationCallToContextNodeMap)
    assignStackNodesPostOrder(Entry.second, Visited, StackIdToMatchingCalls,
                              CallToMatchingCall);
  if (VerifyCCG)
    check();
}

uint64_t ModuleCallsiteContextGraph::getLastStackId(Instruction *Call) {
  CallStack<MDNode, MDNode::op_iterator> CallsiteContext(
      Call->getMetadata(LLVMContext::MD_callsite));
  return CallsiteContext.back();
}

uint64_t IndexCallsiteContextGraph::getLastStackId(IndexCall &Call) {
  assert(isa<CallsiteInfo *>(Call));
  CallStack<CallsiteInfo, SmallVector<unsigned>::const_iterator>
      CallsiteContext(dyn_cast_if_present<CallsiteInfo *>(Call));
  // Need to convert index into stack id.
  return Index.getStackIdAtIndex(CallsiteContext.back());
}

static const std::string MemProfCloneSuffix = ".memprof.";

static std::string getMemProfFuncName(Twine Base, unsigned CloneNo) {
  // We use CloneNo == 0 to refer to the original version, which doesn't get
  // renamed with a suffix.
  if (!CloneNo)
    return Base.str();
  return (Base + MemProfCloneSuffix + Twine(CloneNo)).str();
}

static bool isMemProfClone(const Function &F) {
  return F.getName().contains(MemProfCloneSuffix);
}

// Return the clone number of the given function by extracting it from the
// memprof suffix. Assumes the caller has already confirmed it is a memprof
// clone.
static unsigned getMemProfCloneNum(const Function &F) {
  assert(isMemProfClone(F));
  auto Pos = F.getName().find_last_of('.');
  assert(Pos > 0);
  unsigned CloneNo;
  bool Err = F.getName().drop_front(Pos + 1).getAsInteger(10, CloneNo);
  assert(!Err);
  (void)Err;
  return CloneNo;
}

std::string ModuleCallsiteContextGraph::getLabel(const Function *Func,
                                                 const Instruction *Call,
                                                 unsigned CloneNo) const {
  return (Twine(Call->getFunction()->getName()) + " -> " +
          cast<CallBase>(Call)->getCalledFunction()->getName())
      .str();
}

std::string IndexCallsiteContextGraph::getLabel(const FunctionSummary *Func,
                                                const IndexCall &Call,
                                                unsigned CloneNo) const {
  auto VI = FSToVIMap.find(Func);
  assert(VI != FSToVIMap.end());
  std::string CallerName = getMemProfFuncName(VI->second.name(), CloneNo);
  if (isa<AllocInfo *>(Call))
    return CallerName + " -> alloc";
  else {
    auto *Callsite = dyn_cast_if_present<CallsiteInfo *>(Call);
    return CallerName + " -> " +
           getMemProfFuncName(Callsite->Callee.name(),
                              Callsite->Clones[CloneNo]);
  }
}

std::vector<uint64_t>
ModuleCallsiteContextGraph::getStackIdsWithContextNodesForCall(
    Instruction *Call) {
  CallStack<MDNode, MDNode::op_iterator> CallsiteContext(
      Call->getMetadata(LLVMContext::MD_callsite));
  return getStackIdsWithContextNodes<MDNode, MDNode::op_iterator>(
      CallsiteContext);
}

std::vector<uint64_t>
IndexCallsiteContextGraph::getStackIdsWithContextNodesForCall(IndexCall &Call) {
  assert(isa<CallsiteInfo *>(Call));
  CallStack<CallsiteInfo, SmallVector<unsigned>::const_iterator>
      CallsiteContext(dyn_cast_if_present<CallsiteInfo *>(Call));
  return getStackIdsWithContextNodes<CallsiteInfo,
                                     SmallVector<unsigned>::const_iterator>(
      CallsiteContext);
}

template <typename DerivedCCG, typename FuncTy, typename CallTy>
template <class NodeT, class IteratorT>
std::vector<uint64_t>
CallsiteContextGraph<DerivedCCG, FuncTy, CallTy>::getStackIdsWithContextNodes(
    CallStack<NodeT, IteratorT> &CallsiteContext) {
  std::vector<uint64_t> StackIds;
  for (auto IdOrIndex : CallsiteContext) {
    auto StackId = getStackId(IdOrIndex);
    ContextNode *Node = getNodeForStackId(StackId);
    if (!Node)
      break;
    StackIds.push_back(StackId);
  }
  return StackIds;
}

ModuleCallsiteContextGraph::ModuleCallsiteContextGraph(
    Module &M,
    llvm::function_ref<OptimizationRemarkEmitter &(Function *)> OREGetter)
    : Mod(M), OREGetter(OREGetter) {
  for (auto &F : M) {
    std::vector<CallInfo> CallsWithMetadata;
    for (auto &BB : F) {
      for (auto &I : BB) {
        if (!isa<CallBase>(I))
          continue;
        if (auto *MemProfMD = I.getMetadata(LLVMContext::MD_memprof)) {
          CallsWithMetadata.push_back(&I);
          auto *AllocNode = addAllocNode(&I, &F);
          auto *CallsiteMD = I.getMetadata(LLVMContext::MD_callsite);
          assert(CallsiteMD);
          CallStack<MDNode, MDNode::op_iterator> CallsiteContext(CallsiteMD);
          // Add all of the MIBs and their stack nodes.
          for (auto &MDOp : MemProfMD->operands()) {
            auto *MIBMD = cast<const MDNode>(MDOp);
            std::vector<ContextTotalSize> ContextSizeInfo;
            // Collect the context size information if it exists.
            if (MIBMD->getNumOperands() > 2) {
              for (unsigned I = 2; I < MIBMD->getNumOperands(); I++) {
                MDNode *ContextSizePair =
                    dyn_cast<MDNode>(MIBMD->getOperand(I));
                assert(ContextSizePair->getNumOperands() == 2);
                uint64_t FullStackId = mdconst::dyn_extract<ConstantInt>(
                                           ContextSizePair->getOperand(0))
                                           ->getZExtValue();
                uint64_t TotalSize = mdconst::dyn_extract<ConstantInt>(
                                         ContextSizePair->getOperand(1))
                                         ->getZExtValue();
                ContextSizeInfo.push_back({FullStackId, TotalSize});
              }
            }
            MDNode *StackNode = getMIBStackNode(MIBMD);
            assert(StackNode);
            CallStack<MDNode, MDNode::op_iterator> StackContext(StackNode);
            addStackNodesForMIB<MDNode, MDNode::op_iterator>(
                AllocNode, StackContext, CallsiteContext,
                getMIBAllocType(MIBMD), ContextSizeInfo);
          }
          // If exporting the graph to dot and an allocation id of interest was
          // specified, record all the context ids for this allocation node.
          if (ExportToDot && AllocNode->OrigStackOrAllocId == AllocIdForDot)
            DotAllocContextIds = AllocNode->getContextIds();
          assert(AllocNode->AllocTypes != (uint8_t)AllocationType::None);
          // Memprof and callsite metadata on memory allocations no longer
          // needed.
          I.setMetadata(LLVMContext::MD_memprof, nullptr);
          I.setMetadata(LLVMContext::MD_callsite, nullptr);
        }
        // For callsite metadata, add to list for this function for later use.
        else if (I.getMetadata(LLVMContext::MD_callsite)) {
          CallsWithMetadata.push_back(&I);
        }
      }
    }
    if (!CallsWithMetadata.empty())
      FuncToCallsWithMetadata[&F] = CallsWithMetadata;
  }

  if (DumpCCG) {
    dbgs() << "CCG before updating call stack chains:\n";
    dbgs() << *this;
  }

  if (ExportToDot)
    exportToDot("prestackupdate");

  updateStackNodes();

  if (ExportToDot)
    exportToDot("poststackupdate");

  handleCallsitesWithMultipleTargets();

  markBackedges();

  // Strip off remaining callsite metadata, no longer needed.
  for (auto &FuncEntry : FuncToCallsWithMetadata)
    for (auto &Call : FuncEntry.second)
      Call.call()->setMetadata(LLVMContext::MD_callsite, nullptr);
}

IndexCallsiteContextGraph::IndexCallsiteContextGraph(
    ModuleSummaryIndex &Index,
    llvm::function_ref<bool(GlobalValue::GUID, const GlobalValueSummary *)>
        isPrevailing)
    : Index(Index), isPrevailing(isPrevailing) {
  for (auto &I : Index) {
    auto VI = Index.getValueInfo(I);
    for (auto &S : VI.getSummaryList()) {
      // We should only add the prevailing nodes. Otherwise we may try to clone
      // in a weak copy that won't be linked (and may be different than the
      // prevailing version).
      // We only keep the memprof summary on the prevailing copy now when
      // building the combined index, as a space optimization, however don't
      // rely on this optimization. The linker doesn't resolve local linkage
      // values so don't check whether those are prevailing.
      if (!GlobalValue::isLocalLinkage(S->linkage()) &&
          !isPrevailing(VI.getGUID(), S.get()))
        continue;
      auto *FS = dyn_cast<FunctionSummary>(S.get());
      if (!FS)
        continue;
      std::vector<CallInfo> CallsWithMetadata;
      if (!FS->allocs().empty()) {
        for (auto &AN : FS->mutableAllocs()) {
          // This can happen because of recursion elimination handling that
          // currently exists in ModuleSummaryAnalysis. Skip these for now.
          // We still added them to the summary because we need to be able to
          // correlate properly in applyImport in the backends.
          if (AN.MIBs.empty())
            continue;
          IndexCall AllocCall(&AN);
          CallsWithMetadata.push_back(AllocCall);
          auto *AllocNode = addAllocNode(AllocCall, FS);
          // Pass an empty CallStack to the CallsiteContext (second)
          // parameter, since for ThinLTO we already collapsed out the inlined
          // stack ids on the allocation call during ModuleSummaryAnalysis.
          CallStack<MIBInfo, SmallVector<unsigned>::const_iterator>
              EmptyContext;
          unsigned I = 0;
          assert(!metadataMayIncludeContextSizeInfo() ||
                 AN.ContextSizeInfos.size() == AN.MIBs.size());
          // Now add all of the MIBs and their stack nodes.
          for (auto &MIB : AN.MIBs) {
            CallStack<MIBInfo, SmallVector<unsigned>::const_iterator>
                StackContext(&MIB);
            std::vector<ContextTotalSize> ContextSizeInfo;
            if (!AN.ContextSizeInfos.empty()) {
              for (auto [FullStackId, TotalSize] : AN.ContextSizeInfos[I])
                ContextSizeInfo.push_back({FullStackId, TotalSize});
            }
            addStackNodesForMIB<MIBInfo, SmallVector<unsigned>::const_iterator>(
                AllocNode, StackContext, EmptyContext, MIB.AllocType,
                ContextSizeInfo);
            I++;
          }
          // If exporting the graph to dot and an allocation id of interest was
          // specified, record all the context ids for this allocation node.
          if (ExportToDot && AllocNode->OrigStackOrAllocId == AllocIdForDot)
            DotAllocContextIds = AllocNode->getContextIds();
          assert(AllocNode->AllocTypes != (uint8_t)AllocationType::None);
          // Initialize version 0 on the summary alloc node to the current alloc
          // type, unless it has both types in which case make it default, so
          // that in the case where we aren't able to clone the original version
          // always ends up with the default allocation behavior.
          AN.Versions[0] = (uint8_t)allocTypeToUse(AllocNode->AllocTypes);
        }
      }
      // For callsite metadata, add to list for this function for later use.
      if (!FS->callsites().empty())
        for (auto &SN : FS->mutableCallsites()) {
          IndexCall StackNodeCall(&SN);
          CallsWithMetadata.push_back(StackNodeCall);
        }

      if (!CallsWithMetadata.empty())
        FuncToCallsWithMetadata[FS] = CallsWithMetadata;

      if (!FS->allocs().empty() || !FS->callsites().empty())
        FSToVIMap[FS] = VI;
    }
  }

  if (DumpCCG) {
    dbgs() << "CCG before updating call stack chains:\n";
    dbgs() << *this;
  }

  if (ExportToDot)
    exportToDot("prestackupdate");

  updateStackNodes();

  if (ExportToDot)
    exportToDot("poststackupdate");

  handleCallsitesWithMultipleTargets();

  markBackedges();
}

template <typename DerivedCCG, typename FuncTy, typename CallTy>
void CallsiteContextGraph<DerivedCCG, FuncTy,
                          CallTy>::handleCallsitesWithMultipleTargets() {
  // Look for and workaround callsites that call multiple functions.
  // This can happen for indirect calls, which needs better handling, and in
  // more rare cases (e.g. macro expansion).
  // TODO: To fix this for indirect calls we will want to perform speculative
  // devirtualization using either the normal PGO info with ICP, or using the
  // information in the profiled MemProf contexts. We can do this prior to
  // this transformation for regular LTO, and for ThinLTO we can simulate that
  // effect in the summary and perform the actual speculative devirtualization
  // while cloning in the ThinLTO backend.

  // Keep track of the new nodes synthesized for discovered tail calls missing
  // from the profiled contexts.
  MapVector<CallInfo, ContextNode *> TailCallToContextNodeMap;

  std::vector<std::pair<CallInfo, ContextNode *>> NewCallToNode;
  for (auto &Entry : NonAllocationCallToContextNodeMap) {
    auto *Node = Entry.second;
    assert(Node->Clones.empty());
    // Check all node callees and see if in the same function.
    // We need to check all of the calls recorded in this Node, because in some
    // cases we may have had multiple calls with the same debug info calling
    // different callees. This can happen, for example, when an object is
    // constructed in the paramter list - the destructor call of the object has
    // the same debug info (line/col) as the call the object was passed to.
    // Here we will prune any that don't match all callee nodes.
    std::vector<CallInfo> AllCalls;
    AllCalls.reserve(Node->MatchingCalls.size() + 1);
    AllCalls.push_back(Node->Call);
    llvm::append_range(AllCalls, Node->MatchingCalls);

    // First see if we can partition the calls by callee function, creating new
    // nodes to host each set of calls calling the same callees. This is
    // necessary for support indirect calls with ThinLTO, for which we
    // synthesized CallsiteInfo records for each target. They will all have the
    // same callsite stack ids and would be sharing a context node at this
    // point. We need to perform separate cloning for each, which will be
    // applied along with speculative devirtualization in the ThinLTO backends
    // as needed. Note this does not currently support looking through tail
    // calls, it is unclear if we need that for indirect call targets.
    // First partition calls by callee func. Map indexed by func, value is
    // struct with list of matching calls, assigned node.
    if (partitionCallsByCallee(Node, AllCalls, NewCallToNode))
      continue;

    auto It = AllCalls.begin();
    // Iterate through the calls until we find the first that matches.
    for (; It != AllCalls.end(); ++It) {
      auto ThisCall = *It;
      bool Match = true;
      for (auto EI = Node->CalleeEdges.begin(); EI != Node->CalleeEdges.end();
           ++EI) {
        auto Edge = *EI;
        if (!Edge->Callee->hasCall())
          continue;
        assert(NodeToCallingFunc.count(Edge->Callee));
        // Check if the called function matches that of the callee node.
        if (!calleesMatch(ThisCall.call(), EI, TailCallToContextNodeMap)) {
          Match = false;
          break;
        }
      }
      // Found a call that matches the callee nodes, we can quit now.
      if (Match) {
        // If the first match is not the primary call on the Node, update it
        // now. We will update the list of matching calls further below.
        if (Node->Call != ThisCall) {
          Node->setCall(ThisCall);
          // We need to update the NonAllocationCallToContextNodeMap, but don't
          // want to do this during iteration over that map, so save the calls
          // that need updated entries.
          NewCallToNode.push_back({ThisCall, Node});
        }
        break;
      }
    }
    // We will update this list below (or leave it cleared if there was no
    // match found above).
    Node->MatchingCalls.clear();
    // If we hit the end of the AllCalls vector, no call matching the callee
    // nodes was found, clear the call information in the node.
    if (It == AllCalls.end()) {
      RemovedEdgesWithMismatchedCallees++;
      // Work around by setting Node to have a null call, so it gets
      // skipped during cloning. Otherwise assignFunctions will assert
      // because its data structures are not designed to handle this case.
      Node->setCall(CallInfo());
      continue;
    }
    // Now add back any matching calls that call the same function as the
    // matching primary call on Node.
    for (++It; It != AllCalls.end(); ++It) {
      auto ThisCall = *It;
      if (!sameCallee(Node->Call.call(), ThisCall.call()))
        continue;
      Node->MatchingCalls.push_back(ThisCall);
    }
  }

  // Remove all mismatched nodes identified in the above loop from the node map
  // (checking whether they have a null call which is set above). For a
  // MapVector like NonAllocationCallToContextNodeMap it is much more efficient
  // to do the removal via remove_if than by individually erasing entries above.
  // Also remove any entries if we updated the node's primary call above.
  NonAllocationCallToContextNodeMap.remove_if([](const auto &it) {
    return !it.second->hasCall() || it.second->Call != it.first;
  });

  // Add entries for any new primary calls recorded above.
  for (auto &[Call, Node] : NewCallToNode)
    NonAllocationCallToContextNodeMap[Call] = Node;

  // Add the new nodes after the above loop so that the iteration is not
  // invalidated.
  for (auto &[Call, Node] : TailCallToContextNodeMap)
    NonAllocationCallToContextNodeMap[Call] = Node;
}

template <typename DerivedCCG, typename FuncTy, typename CallTy>
bool CallsiteContextGraph<DerivedCCG, FuncTy, CallTy>::partitionCallsByCallee(
    ContextNode *Node, ArrayRef<CallInfo> AllCalls,
    std::vector<std::pair<CallInfo, ContextNode *>> &NewCallToNode) {
  // Struct to keep track of all the calls having the same callee function,
  // and the node we eventually assign to them. Eventually we will record the
  // context node assigned to this group of calls.
  struct CallsWithSameCallee {
    std::vector<CallInfo> Calls;
    ContextNode *Node = nullptr;
  };

  // First partition calls by callee function. Build map from each function
  // to the list of matching calls.
  DenseMap<const FuncTy *, CallsWithSameCallee> CalleeFuncToCallInfo;
  for (auto ThisCall : AllCalls) {
    auto *F = getCalleeFunc(ThisCall.call());
    if (F)
      CalleeFuncToCallInfo[F].Calls.push_back(ThisCall);
  }

  // Next, walk through all callee edges. For each callee node, get its
  // containing function and see if it was recorded in the above map (meaning we
  // have at least one matching call). Build another map from each callee node
  // with a matching call to the structure instance created above containing all
  // the calls.
  DenseMap<ContextNode *, CallsWithSameCallee *> CalleeNodeToCallInfo;
  for (const auto &Edge : Node->CalleeEdges) {
    if (!Edge->Callee->hasCall())
      continue;
    const FuncTy *ProfiledCalleeFunc = NodeToCallingFunc[Edge->Callee];
    if (CalleeFuncToCallInfo.contains(ProfiledCalleeFunc))
      CalleeNodeToCallInfo[Edge->Callee] =
          &CalleeFuncToCallInfo[ProfiledCalleeFunc];
  }

  // If there are entries in the second map, then there were no matching
  // calls/callees, nothing to do here. Return so we can go to the handling that
  // looks through tail calls.
  if (CalleeNodeToCallInfo.empty())
    return false;

  // Walk through all callee edges again. Any and all callee edges that didn't
  // match any calls (callee not in the CalleeNodeToCallInfo map) are moved to a
  // new caller node (UnmatchedCalleesNode) which gets a null call so that it is
  // ignored during cloning. If it is in the map, then we use the node recorded
  // in that entry (creating it if needed), and move the callee edge to it.
  // The first callee will use the original node instead of creating a new one.
  // Note that any of the original calls on this node (in AllCalls) that didn't
  // have a callee function automatically get dropped from the node as part of
  // this process.
  ContextNode *UnmatchedCalleesNode = nullptr;
  // Track whether we already assigned original node to a callee.
  bool UsedOrigNode = false;
  assert(NodeToCallingFunc[Node]);
  // Iterate over a copy of Node's callee edges, since we may need to remove
  // edges in moveCalleeEdgeToNewCaller, and this simplifies the handling and
  // makes it less error-prone.
  auto CalleeEdges = Node->CalleeEdges;
  for (auto &Edge : CalleeEdges) {
    if (!Edge->Callee->hasCall())
      continue;

    // Will be updated below to point to whatever (caller) node this callee edge
    // should be moved to.
    ContextNode *CallerNodeToUse = nullptr;

    // Handle the case where there were no matching calls first. Move this
    // callee edge to the UnmatchedCalleesNode, creating it if needed.
    if (!CalleeNodeToCallInfo.contains(Edge->Callee)) {
      if (!UnmatchedCalleesNode)
        UnmatchedCalleesNode =
            createNewNode(/*IsAllocation=*/false, NodeToCallingFunc[Node]);
      CallerNodeToUse = UnmatchedCalleesNode;
    } else {
      // Look up the information recorded for this callee node, and use the
      // recorded caller node (creating it if needed).
      auto *Info = CalleeNodeToCallInfo[Edge->Callee];
      if (!Info->Node) {
        // If we haven't assigned any callees to the original node use it.
        if (!UsedOrigNode) {
          Info->Node = Node;
          // Clear the set of matching calls which will be updated below.
          Node->MatchingCalls.clear();
          UsedOrigNode = true;
        } else
          Info->Node =
              createNewNode(/*IsAllocation=*/false, NodeToCallingFunc[Node]);
        assert(!Info->Calls.empty());
        // The first call becomes the primary call for this caller node, and the
        // rest go in the matching calls list.
        Info->Node->setCall(Info->Calls.front());
        llvm::append_range(Info->Node->MatchingCalls,
                           llvm::drop_begin(Info->Calls));
        // Save the primary call to node correspondence so that we can update
        // the NonAllocationCallToContextNodeMap, which is being iterated in the
        // caller of this function.
        NewCallToNode.push_back({Info->Node->Call, Info->Node});
      }
      CallerNodeToUse = Info->Node;
    }

    // Don't need to move edge if we are using the original node;
    if (CallerNodeToUse == Node)
      continue;

    moveCalleeEdgeToNewCaller(Edge, CallerNodeToUse);
  }
  // Now that we are done moving edges, clean up any caller edges that ended
  // up with no type or context ids. During moveCalleeEdgeToNewCaller all
  // caller edges from Node are replicated onto the new callers, and it
  // simplifies the handling to leave them until we have moved all
  // edges/context ids.
  for (auto &I : CalleeNodeToCallInfo)
    removeNoneTypeCallerEdges(I.second->Node);
  if (UnmatchedCalleesNode)
    removeNoneTypeCallerEdges(UnmatchedCalleesNode);
  removeNoneTypeCallerEdges(Node);

  return true;
}

uint64_t ModuleCallsiteContextGraph::getStackId(uint64_t IdOrIndex) const {
  // In the Module (IR) case this is already the Id.
  return IdOrIndex;
}

uint64_t IndexCallsiteContextGraph::getStackId(uint64_t IdOrIndex) const {
  // In the Index case this is an index into the stack id list in the summary
  // index, convert it to an Id.
  return Index.getStackIdAtIndex(IdOrIndex);
}

template <typename DerivedCCG, typename FuncTy, typename CallTy>
bool CallsiteContextGraph<DerivedCCG, FuncTy, CallTy>::calleesMatch(
    CallTy Call, EdgeIter &EI,
    MapVector<CallInfo, ContextNode *> &TailCallToContextNodeMap) {
  auto Edge = *EI;
  const FuncTy *ProfiledCalleeFunc = NodeToCallingFunc[Edge->Callee];
  const FuncTy *CallerFunc = NodeToCallingFunc[Edge->Caller];
  // Will be populated in order of callee to caller if we find a chain of tail
  // calls between the profiled caller and callee.
  std::vector<std::pair<CallTy, FuncTy *>> FoundCalleeChain;
  if (!calleeMatchesFunc(Call, ProfiledCalleeFunc, CallerFunc,
                         FoundCalleeChain))
    return false;

  // The usual case where the profiled callee matches that of the IR/summary.
  if (FoundCalleeChain.empty())
    return true;

  auto AddEdge = [Edge, &EI](ContextNode *Caller, ContextNode *Callee) {
    auto *CurEdge = Callee->findEdgeFromCaller(Caller);
    // If there is already an edge between these nodes, simply update it and
    // return.
    if (CurEdge) {
      CurEdge->ContextIds.insert_range(Edge->ContextIds);
      CurEdge->AllocTypes |= Edge->AllocTypes;
      return;
    }
    // Otherwise, create a new edge and insert it into the caller and callee
    // lists.
    auto NewEdge = std::make_shared<ContextEdge>(
        Callee, Caller, Edge->AllocTypes, Edge->ContextIds);
    Callee->CallerEdges.push_back(NewEdge);
    if (Caller == Edge->Caller) {
      // If we are inserting the new edge into the current edge's caller, insert
      // the new edge before the current iterator position, and then increment
      // back to the current edge.
      EI = Caller->CalleeEdges.insert(EI, NewEdge);
      ++EI;
      assert(*EI == Edge &&
             "Iterator position not restored after insert and increment");
    } else
      Caller->CalleeEdges.push_back(NewEdge);
  };

  // Create new nodes for each found callee and connect in between the profiled
  // caller and callee.
  auto *CurCalleeNode = Edge->Callee;
  for (auto &[NewCall, Func] : FoundCalleeChain) {
    ContextNode *NewNode = nullptr;
    // First check if we have already synthesized a node for this tail call.
    if (TailCallToContextNodeMap.count(NewCall)) {
      NewNode = TailCallToContextNodeMap[NewCall];
      NewNode->AllocTypes |= Edge->AllocTypes;
    } else {
      FuncToCallsWithMetadata[Func].push_back({NewCall});
      // Create Node and record node info.
      NewNode = createNewNode(/*IsAllocation=*/false, Func, NewCall);
      TailCallToContextNodeMap[NewCall] = NewNode;
      NewNode->AllocTypes = Edge->AllocTypes;
    }

    // Hook up node to its callee node
    AddEdge(NewNode, CurCalleeNode);

    CurCalleeNode = NewNode;
  }

  // Hook up edge's original caller to new callee node.
  AddEdge(Edge->Caller, CurCalleeNode);

#ifndef NDEBUG
  // Save this because Edge's fields get cleared below when removed.
  auto *Caller = Edge->Caller;
#endif

  // Remove old edge
  removeEdgeFromGraph(Edge.get(), &EI, /*CalleeIter=*/true);

  // To simplify the increment of EI in the caller, subtract one from EI.
  // In the final AddEdge call we would have either added a new callee edge,
  // to Edge->Caller, or found an existing one. Either way we are guaranteed
  // that there is at least one callee edge.
  assert(!Caller->CalleeEdges.empty());
  --EI;

  return true;
}

bool ModuleCallsiteContextGraph::findProfiledCalleeThroughTailCalls(
    const Function *ProfiledCallee, Value *CurCallee, unsigned Depth,
    std::vector<std::pair<Instruction *, Function *>> &FoundCalleeChain,
    bool &FoundMultipleCalleeChains) {
  // Stop recursive search if we have already explored the maximum specified
  // depth.
  if (Depth > TailCallSearchDepth)
    return false;

  auto SaveCallsiteInfo = [&](Instruction *Callsite, Function *F) {
    FoundCalleeChain.push_back({Callsite, F});
  };

  auto *CalleeFunc = dyn_cast<Function>(CurCallee);
  if (!CalleeFunc) {
    auto *Alias = dyn_cast<GlobalAlias>(CurCallee);
    assert(Alias);
    CalleeFunc = dyn_cast<Function>(Alias->getAliasee());
    assert(CalleeFunc);
  }

  // Look for tail calls in this function, and check if they either call the
  // profiled callee directly, or indirectly (via a recursive search).
  // Only succeed if there is a single unique tail call chain found between the
  // profiled caller and callee, otherwise we could perform incorrect cloning.
  bool FoundSingleCalleeChain = false;
  for (auto &BB : *CalleeFunc) {
    for (auto &I : BB) {
      auto *CB = dyn_cast<CallBase>(&I);
      if (!CB || !CB->isTailCall())
        continue;
      auto *CalledValue = CB->getCalledOperand();
      auto *CalledFunction = CB->getCalledFunction();
      if (CalledValue && !CalledFunction) {
        CalledValue = CalledValue->stripPointerCasts();
        // Stripping pointer casts can reveal a called function.
        CalledFunction = dyn_cast<Function>(CalledValue);
      }
      // Check if this is an alias to a function. If so, get the
      // called aliasee for the checks below.
      if (auto *GA = dyn_cast<GlobalAlias>(CalledValue)) {
        assert(!CalledFunction &&
               "Expected null called function in callsite for alias");
        CalledFunction = dyn_cast<Function>(GA->getAliaseeObject());
      }
      if (!CalledFunction)
        continue;
      if (CalledFunction == ProfiledCallee) {
        if (FoundSingleCalleeChain) {
          FoundMultipleCalleeChains = true;
          return false;
        }
        FoundSingleCalleeChain = true;
        FoundProfiledCalleeCount++;
        FoundProfiledCalleeDepth += Depth;
        if (Depth > FoundProfiledCalleeMaxDepth)
          FoundProfiledCalleeMaxDepth = Depth;
        SaveCallsiteInfo(&I, CalleeFunc);
      } else if (findProfiledCalleeThroughTailCalls(
                     ProfiledCallee, CalledFunction, Depth + 1,
                     FoundCalleeChain, FoundMultipleCalleeChains)) {
        // findProfiledCalleeThroughTailCalls should not have returned
        // true if FoundMultipleCalleeChains.
        assert(!FoundMultipleCalleeChains);
        if (FoundSingleCalleeChain) {
          FoundMultipleCalleeChains = true;
          return false;
        }
        FoundSingleCalleeChain = true;
        SaveCallsiteInfo(&I, CalleeFunc);
      } else if (FoundMultipleCalleeChains)
        return false;
    }
  }

  return FoundSingleCalleeChain;
}

const Function *ModuleCallsiteContextGraph::getCalleeFunc(Instruction *Call) {
  auto *CB = dyn_cast<CallBase>(Call);
  if (!CB->getCalledOperand() || CB->isIndirectCall())
    return nullptr;
  auto *CalleeVal = CB->getCalledOperand()->stripPointerCasts();
  auto *Alias = dyn_cast<GlobalAlias>(CalleeVal);
  if (Alias)
    return dyn_cast<Function>(Alias->getAliasee());
  return dyn_cast<Function>(CalleeVal);
}

bool ModuleCallsiteContextGraph::calleeMatchesFunc(
    Instruction *Call, const Function *Func, const Function *CallerFunc,
    std::vector<std::pair<Instruction *, Function *>> &FoundCalleeChain) {
  auto *CB = dyn_cast<CallBase>(Call);
  if (!CB->getCalledOperand() || CB->isIndirectCall())
    return false;
  auto *CalleeVal = CB->getCalledOperand()->stripPointerCasts();
  auto *CalleeFunc = dyn_cast<Function>(CalleeVal);
  if (CalleeFunc == Func)
    return true;
  auto *Alias = dyn_cast<GlobalAlias>(CalleeVal);
  if (Alias && Alias->getAliasee() == Func)
    return true;

  // Recursively search for the profiled callee through tail calls starting with
  // the actual Callee. The discovered tail call chain is saved in
  // FoundCalleeChain, and we will fixup the graph to include these callsites
  // after returning.
  // FIXME: We will currently redo the same recursive walk if we find the same
  // mismatched callee from another callsite. We can improve this with more
  // bookkeeping of the created chain of new nodes for each mismatch.
  unsigned Depth = 1;
  bool FoundMultipleCalleeChains = false;
  if (!findProfiledCalleeThroughTailCalls(Func, CalleeVal, Depth,
                                          FoundCalleeChain,
                                          FoundMultipleCalleeChains)) {
    LLVM_DEBUG(dbgs() << "Not found through unique tail call chain: "
                      << Func->getName() << " from " << CallerFunc->getName()
                      << " that actually called " << CalleeVal->getName()
                      << (FoundMultipleCalleeChains
                              ? " (found multiple possible chains)"
                              : "")
                      << "\n");
    if (FoundMultipleCalleeChains)
      FoundProfiledCalleeNonUniquelyCount++;
    return false;
  }

  return true;
}

bool ModuleCallsiteContextGraph::sameCallee(Instruction *Call1,
                                            Instruction *Call2) {
  auto *CB1 = cast<CallBase>(Call1);
  if (!CB1->getCalledOperand() || CB1->isIndirectCall())
    return false;
  auto *CalleeVal1 = CB1->getCalledOperand()->stripPointerCasts();
  auto *CalleeFunc1 = dyn_cast<Function>(CalleeVal1);
  auto *CB2 = cast<CallBase>(Call2);
  if (!CB2->getCalledOperand() || CB2->isIndirectCall())
    return false;
  auto *CalleeVal2 = CB2->getCalledOperand()->stripPointerCasts();
  auto *CalleeFunc2 = dyn_cast<Function>(CalleeVal2);
  return CalleeFunc1 == CalleeFunc2;
}

bool IndexCallsiteContextGraph::findProfiledCalleeThroughTailCalls(
    ValueInfo ProfiledCallee, ValueInfo CurCallee, unsigned Depth,
    std::vector<std::pair<IndexCall, FunctionSummary *>> &FoundCalleeChain,
    bool &FoundMultipleCalleeChains) {
  // Stop recursive search if we have already explored the maximum specified
  // depth.
  if (Depth > TailCallSearchDepth)
    return false;

  auto CreateAndSaveCallsiteInfo = [&](ValueInfo Callee, FunctionSummary *FS) {
    // Make a CallsiteInfo for each discovered callee, if one hasn't already
    // been synthesized.
    if (!FunctionCalleesToSynthesizedCallsiteInfos.count(FS) ||
        !FunctionCalleesToSynthesizedCallsiteInfos[FS].count(Callee))
      // StackIds is empty (we don't have debug info available in the index for
      // these callsites)
      FunctionCalleesToSynthesizedCallsiteInfos[FS][Callee] =
          std::make_unique<CallsiteInfo>(Callee, SmallVector<unsigned>());
    CallsiteInfo *NewCallsiteInfo =
        FunctionCalleesToSynthesizedCallsiteInfos[FS][Callee].get();
    FoundCalleeChain.push_back({NewCallsiteInfo, FS});
  };

  // Look for tail calls in this function, and check if they either call the
  // profiled callee directly, or indirectly (via a recursive search).
  // Only succeed if there is a single unique tail call chain found between the
  // profiled caller and callee, otherwise we could perform incorrect cloning.
  bool FoundSingleCalleeChain = false;
  for (auto &S : CurCallee.getSummaryList()) {
    if (!GlobalValue::isLocalLinkage(S->linkage()) &&
        !isPrevailing(CurCallee.getGUID(), S.get()))
      continue;
    auto *FS = dyn_cast<FunctionSummary>(S->getBaseObject());
    if (!FS)
      continue;
    auto FSVI = CurCallee;
    auto *AS = dyn_cast<AliasSummary>(S.get());
    if (AS)
      FSVI = AS->getAliaseeVI();
    for (auto &CallEdge : FS->calls()) {
      if (!CallEdge.second.hasTailCall())
        continue;
      if (CallEdge.first == ProfiledCallee) {
        if (FoundSingleCalleeChain) {
          FoundMultipleCalleeChains = true;
          return false;
        }
        FoundSingleCalleeChain = true;
        FoundProfiledCalleeCount++;
        FoundProfiledCalleeDepth += Depth;
        if (Depth > FoundProfiledCalleeMaxDepth)
          FoundProfiledCalleeMaxDepth = Depth;
        CreateAndSaveCallsiteInfo(CallEdge.first, FS);
        // Add FS to FSToVIMap  in case it isn't already there.
        assert(!FSToVIMap.count(FS) || FSToVIMap[FS] == FSVI);
        FSToVIMap[FS] = FSVI;
      } else if (findProfiledCalleeThroughTailCalls(
                     ProfiledCallee, CallEdge.first, Depth + 1,
                     FoundCalleeChain, FoundMultipleCalleeChains)) {
        // findProfiledCalleeThroughTailCalls should not have returned
        // true if FoundMultipleCalleeChains.
        assert(!FoundMultipleCalleeChains);
        if (FoundSingleCalleeChain) {
          FoundMultipleCalleeChains = true;
          return false;
        }
        FoundSingleCalleeChain = true;
        CreateAndSaveCallsiteInfo(CallEdge.first, FS);
        // Add FS to FSToVIMap  in case it isn't already there.
        assert(!FSToVIMap.count(FS) || FSToVIMap[FS] == FSVI);
        FSToVIMap[FS] = FSVI;
      } else if (FoundMultipleCalleeChains)
        return false;
    }
  }

  return FoundSingleCalleeChain;
}

const FunctionSummary *
IndexCallsiteContextGraph::getCalleeFunc(IndexCall &Call) {
  ValueInfo Callee = dyn_cast_if_present<CallsiteInfo *>(Call)->Callee;
  if (Callee.getSummaryList().empty())
    return nullptr;
  return dyn_cast<FunctionSummary>(Callee.getSummaryList()[0]->getBaseObject());
}

bool IndexCallsiteContextGraph::calleeMatchesFunc(
    IndexCall &Call, const FunctionSummary *Func,
    const FunctionSummary *CallerFunc,
    std::vector<std::pair<IndexCall, FunctionSummary *>> &FoundCalleeChain) {
  ValueInfo Callee = dyn_cast_if_present<CallsiteInfo *>(Call)->Callee;
  // If there is no summary list then this is a call to an externally defined
  // symbol.
  AliasSummary *Alias =
      Callee.getSummaryList().empty()
          ? nullptr
          : dyn_cast<AliasSummary>(Callee.getSummaryList()[0].get());
  assert(FSToVIMap.count(Func));
  auto FuncVI = FSToVIMap[Func];
  if (Callee == FuncVI ||
      // If callee is an alias, check the aliasee, since only function
      // summary base objects will contain the stack node summaries and thus
      // get a context node.
      (Alias && Alias->getAliaseeVI() == FuncVI))
    return true;

  // Recursively search for the profiled callee through tail calls starting with
  // the actual Callee. The discovered tail call chain is saved in
  // FoundCalleeChain, and we will fixup the graph to include these callsites
  // after returning.
  // FIXME: We will currently redo the same recursive walk if we find the same
  // mismatched callee from another callsite. We can improve this with more
  // bookkeeping of the created chain of new nodes for each mismatch.
  unsigned Depth = 1;
  bool FoundMultipleCalleeChains = false;
  if (!findProfiledCalleeThroughTailCalls(
          FuncVI, Callee, Depth, FoundCalleeChain, FoundMultipleCalleeChains)) {
    LLVM_DEBUG(dbgs() << "Not found through unique tail call chain: " << FuncVI
                      << " from " << FSToVIMap[CallerFunc]
                      << " that actually called " << Callee
                      << (FoundMultipleCalleeChains
                              ? " (found multiple possible chains)"
                              : "")
                      << "\n");
    if (FoundMultipleCalleeChains)
      FoundProfiledCalleeNonUniquelyCount++;
    return false;
  }

  return true;
}

bool IndexCallsiteContextGraph::sameCallee(IndexCall &Call1, IndexCall &Call2) {
  ValueInfo Callee1 = dyn_cast_if_present<CallsiteInfo *>(Call1)->Callee;
  ValueInfo Callee2 = dyn_cast_if_present<CallsiteInfo *>(Call2)->Callee;
  return Callee1 == Callee2;
}

template <typename DerivedCCG, typename FuncTy, typename CallTy>
void CallsiteContextGraph<DerivedCCG, FuncTy, CallTy>::ContextNode::dump()
    const {
  print(dbgs());
  dbgs() << "\n";
}

template <typename DerivedCCG, typename FuncTy, typename CallTy>
void CallsiteContextGraph<DerivedCCG, FuncTy, CallTy>::ContextNode::print(
    raw_ostream &OS) const {
  OS << "Node " << this << "\n";
  OS << "\t";
  printCall(OS);
  if (Recursive)
    OS << " (recursive)";
  OS << "\n";
  if (!MatchingCalls.empty()) {
    OS << "\tMatchingCalls:\n";
    for (auto &MatchingCall : MatchingCalls) {
      OS << "\t";
      MatchingCall.print(OS);
      OS << "\n";
    }
  }
  OS << "\tAllocTypes: " << getAllocTypeString(AllocTypes) << "\n";
  OS << "\tContextIds:";
  // Make a copy of the computed context ids that we can sort for stability.
  auto ContextIds = getContextIds();
  std::vector<uint32_t> SortedIds(ContextIds.begin(), ContextIds.end());
  std::sort(SortedIds.begin(), SortedIds.end());
  for (auto Id : SortedIds)
    OS << " " << Id;
  OS << "\n";
  OS << "\tCalleeEdges:\n";
  for (auto &Edge : CalleeEdges)
    OS << "\t\t" << *Edge << "\n";
  OS << "\tCallerEdges:\n";
  for (auto &Edge : CallerEdges)
    OS << "\t\t" << *Edge << "\n";
  if (!Clones.empty()) {
    OS << "\tClones: " << llvm::interleaved(Clones) << "\n";
  } else if (CloneOf) {
    OS << "\tClone of " << CloneOf << "\n";
  }
}

template <typename DerivedCCG, typename FuncTy, typename CallTy>
void CallsiteContextGraph<DerivedCCG, FuncTy, CallTy>::ContextEdge::dump()
    const {
  print(dbgs());
  dbgs() << "\n";
}

template <typename DerivedCCG, typename FuncTy, typename CallTy>
void CallsiteContextGraph<DerivedCCG, FuncTy, CallTy>::ContextEdge::print(
    raw_ostream &OS) const {
  OS << "Edge from Callee " << Callee << " to Caller: " << Caller
     << (IsBackedge ? " (BE)" : "")
     << " AllocTypes: " << getAllocTypeString(AllocTypes);
  OS << " ContextIds:";
  std::vector<uint32_t> SortedIds(ContextIds.begin(), ContextIds.end());
  std::sort(SortedIds.begin(), SortedIds.end());
  for (auto Id : SortedIds)
    OS << " " << Id;
}

template <typename DerivedCCG, typename FuncTy, typename CallTy>
void CallsiteContextGraph<DerivedCCG, FuncTy, CallTy>::dump() const {
  print(dbgs());
}

template <typename DerivedCCG, typename FuncTy, typename CallTy>
void CallsiteContextGraph<DerivedCCG, FuncTy, CallTy>::print(
    raw_ostream &OS) const {
  OS << "Callsite Context Graph:\n";
  using GraphType = const CallsiteContextGraph<DerivedCCG, FuncTy, CallTy> *;
  for (const auto Node : nodes<GraphType>(this)) {
    if (Node->isRemoved())
      continue;
    Node->print(OS);
    OS << "\n";
  }
}

template <typename DerivedCCG, typename FuncTy, typename CallTy>
void CallsiteContextGraph<DerivedCCG, FuncTy, CallTy>::printTotalSizes(
    raw_ostream &OS) const {
  using GraphType = const CallsiteContextGraph<DerivedCCG, FuncTy, CallTy> *;
  for (const auto Node : nodes<GraphType>(this)) {
    if (Node->isRemoved())
      continue;
    if (!Node->IsAllocation)
      continue;
    DenseSet<uint32_t> ContextIds = Node->getContextIds();
    auto AllocTypeFromCall = getAllocationCallType(Node->Call);
    std::vector<uint32_t> SortedIds(ContextIds.begin(), ContextIds.end());
    std::sort(SortedIds.begin(), SortedIds.end());
    for (auto Id : SortedIds) {
      auto TypeI = ContextIdToAllocationType.find(Id);
      assert(TypeI != ContextIdToAllocationType.end());
      auto CSI = ContextIdToContextSizeInfos.find(Id);
      if (CSI != ContextIdToContextSizeInfos.end()) {
        for (auto &Info : CSI->second) {
          OS << "MemProf hinting: "
             << getAllocTypeString((uint8_t)TypeI->second)
             << " full allocation context " << Info.FullStackId
             << " with total size " << Info.TotalSize << " is "
             << getAllocTypeString(Node->AllocTypes) << " after cloning";
          if (allocTypeToUse(Node->AllocTypes) != AllocTypeFromCall)
            OS << " marked " << getAllocTypeString((uint8_t)AllocTypeFromCall)
               << " due to cold byte percent";
          // Print the internal context id to aid debugging and visualization.
          OS << " (context id " << Id << ")";
          OS << "\n";
        }
      }
    }
  }
}

template <typename DerivedCCG, typename FuncTy, typename CallTy>
void CallsiteContextGraph<DerivedCCG, FuncTy, CallTy>::check() const {
  using GraphType = const CallsiteContextGraph<DerivedCCG, FuncTy, CallTy> *;
  for (const auto Node : nodes<GraphType>(this)) {
    checkNode<DerivedCCG, FuncTy, CallTy>(Node, /*CheckEdges=*/false);
    for (auto &Edge : Node->CallerEdges)
      checkEdge<DerivedCCG, FuncTy, CallTy>(Edge);
  }
}

template <typename DerivedCCG, typename FuncTy, typename CallTy>
struct GraphTraits<const CallsiteContextGraph<DerivedCCG, FuncTy, CallTy> *> {
  using GraphType = const CallsiteContextGraph<DerivedCCG, FuncTy, CallTy> *;
  using NodeRef = const ContextNode<DerivedCCG, FuncTy, CallTy> *;

  using NodePtrTy = std::unique_ptr<ContextNode<DerivedCCG, FuncTy, CallTy>>;
  static NodeRef getNode(const NodePtrTy &P) { return P.get(); }

  using nodes_iterator =
      mapped_iterator<typename std::vector<NodePtrTy>::const_iterator,
                      decltype(&getNode)>;

  static nodes_iterator nodes_begin(GraphType G) {
    return nodes_iterator(G->NodeOwner.begin(), &getNode);
  }

  static nodes_iterator nodes_end(GraphType G) {
    return nodes_iterator(G->NodeOwner.end(), &getNode);
  }

  static NodeRef getEntryNode(GraphType G) {
    return G->NodeOwner.begin()->get();
  }

  using EdgePtrTy = std::shared_ptr<ContextEdge<DerivedCCG, FuncTy, CallTy>>;
  static const ContextNode<DerivedCCG, FuncTy, CallTy> *
  GetCallee(const EdgePtrTy &P) {
    return P->Callee;
  }

  using ChildIteratorType =
      mapped_iterator<typename std::vector<std::shared_ptr<ContextEdge<
                          DerivedCCG, FuncTy, CallTy>>>::const_iterator,
                      decltype(&GetCallee)>;

  static ChildIteratorType child_begin(NodeRef N) {
    return ChildIteratorType(N->CalleeEdges.begin(), &GetCallee);
  }

  static ChildIteratorType child_end(NodeRef N) {
    return ChildIteratorType(N->CalleeEdges.end(), &GetCallee);
  }
};

template <typename DerivedCCG, typename FuncTy, typename CallTy>
struct DOTGraphTraits<const CallsiteContextGraph<DerivedCCG, FuncTy, CallTy> *>
    : public DefaultDOTGraphTraits {
  DOTGraphTraits(bool IsSimple = false) : DefaultDOTGraphTraits(IsSimple) {
    // If the user requested the full graph to be exported, but provided an
    // allocation id, or if the user gave a context id and requested more than
    // just a specific context to be exported, note that highlighting is
    // enabled.
    DoHighlight =
        (AllocIdForDot.getNumOccurrences() && DotGraphScope == DotScope::All) ||
        (ContextIdForDot.getNumOccurrences() &&
         DotGraphScope != DotScope::Context);
  }

  using GraphType = const CallsiteContextGraph<DerivedCCG, FuncTy, CallTy> *;
  using GTraits = GraphTraits<GraphType>;
  using NodeRef = typename GTraits::NodeRef;
  using ChildIteratorType = typename GTraits::ChildIteratorType;

  static std::string getNodeLabel(NodeRef Node, GraphType G) {
    std::string LabelString =
        (Twine("OrigId: ") + (Node->IsAllocation ? "Alloc" : "") +
         Twine(Node->OrigStackOrAllocId))
            .str();
    LabelString += "\n";
    if (Node->hasCall()) {
      auto Func = G->NodeToCallingFunc.find(Node);
      assert(Func != G->NodeToCallingFunc.end());
      LabelString +=
          G->getLabel(Func->second, Node->Call.call(), Node->Call.cloneNo());
    } else {
      LabelString += "null call";
      if (Node->Recursive)
        LabelString += " (recursive)";
      else
        LabelString += " (external)";
    }
    return LabelString;
  }

  static std::string getNodeAttributes(NodeRef Node, GraphType G) {
    auto ContextIds = Node->getContextIds();
    // If highlighting enabled, see if this node contains any of the context ids
    // of interest. If so, it will use a different color and a larger fontsize
    // (which makes the node larger as well).
    bool Highlight = false;
    if (DoHighlight) {
      assert(ContextIdForDot.getNumOccurrences() ||
             AllocIdForDot.getNumOccurrences());
      if (ContextIdForDot.getNumOccurrences())
        Highlight = ContextIds.contains(ContextIdForDot);
      else
        Highlight = set_intersects(ContextIds, G->DotAllocContextIds);
    }
    std::string AttributeString = (Twine("tooltip=\"") + getNodeId(Node) + " " +
                                   getContextIds(ContextIds) + "\"")
                                      .str();
    // Default fontsize is 14
    if (Highlight)
      AttributeString += ",fontsize=\"30\"";
    AttributeString +=
        (Twine(",fillcolor=\"") + getColor(Node->AllocTypes, Highlight) + "\"")
            .str();
    if (Node->CloneOf) {
      AttributeString += ",color=\"blue\"";
      AttributeString += ",style=\"filled,bold,dashed\"";
    } else
      AttributeString += ",style=\"filled\"";
    return AttributeString;
  }

  static std::string getEdgeAttributes(NodeRef, ChildIteratorType ChildIter,
                                       GraphType G) {
    auto &Edge = *(ChildIter.getCurrent());
    // If highlighting enabled, see if this edge contains any of the context ids
    // of interest. If so, it will use a different color and a heavier arrow
    // size and weight (the larger weight makes the highlighted path
    // straighter).
    bool Highlight = false;
    if (DoHighlight) {
      assert(ContextIdForDot.getNumOccurrences() ||
             AllocIdForDot.getNumOccurrences());
      if (ContextIdForDot.getNumOccurrences())
        Highlight = Edge->ContextIds.contains(ContextIdForDot);
      else
        Highlight = set_intersects(Edge->ContextIds, G->DotAllocContextIds);
    }
    auto Color = getColor(Edge->AllocTypes, Highlight);
    std::string AttributeString =
        (Twine("tooltip=\"") + getContextIds(Edge->ContextIds) + "\"" +
         // fillcolor is the arrow head and color is the line
         Twine(",fillcolor=\"") + Color + "\"" + Twine(",color=\"") + Color +
         "\"")
            .str();
    if (Edge->IsBackedge)
      AttributeString += ",style=\"dotted\"";
    // Default penwidth and weight are both 1.
    if (Highlight)
      AttributeString += ",penwidth=\"2.0\",weight=\"2\"";
    return AttributeString;
  }

  // Since the NodeOwners list includes nodes that are no longer connected to
  // the graph, skip them here.
  static bool isNodeHidden(NodeRef Node, GraphType G) {
    if (Node->isRemoved())
      return true;
    // If a scope smaller than the full graph was requested, see if this node
    // contains any of the context ids of interest.
    if (DotGraphScope == DotScope::Alloc)
      return !set_intersects(Node->getContextIds(), G->DotAllocContextIds);
    if (DotGraphScope == DotScope::Context)
      return !Node->getContextIds().contains(ContextIdForDot);
    return false;
  }

private:
  static std::string getContextIds(const DenseSet<uint32_t> &ContextIds) {
    std::string IdString = "ContextIds:";
    if (ContextIds.size() < 100) {
      std::vector<uint32_t> SortedIds(ContextIds.begin(), ContextIds.end());
      std::sort(SortedIds.begin(), SortedIds.end());
      for (auto Id : SortedIds)
        IdString += (" " + Twine(Id)).str();
    } else {
      IdString += (" (" + Twine(ContextIds.size()) + " ids)").str();
    }
    return IdString;
  }

  static std::string getColor(uint8_t AllocTypes, bool Highlight) {
    // If DoHighlight is not enabled, we want to use the highlight colors for
    // NotCold and Cold, and the non-highlight color for NotCold+Cold. This is
    // both compatible with the color scheme before highlighting was supported,
    // and for the NotCold+Cold color the non-highlight color is a bit more
    // readable.
    if (AllocTypes == (uint8_t)AllocationType::NotCold)
      // Color "brown1" actually looks like a lighter red.
      return !DoHighlight || Highlight ? "brown1" : "lightpink";
    if (AllocTypes == (uint8_t)AllocationType::Cold)
      return !DoHighlight || Highlight ? "cyan" : "lightskyblue";
    if (AllocTypes ==
        ((uint8_t)AllocationType::NotCold | (uint8_t)AllocationType::Cold))
      return Highlight ? "magenta" : "mediumorchid1";
    return "gray";
  }

  static std::string getNodeId(NodeRef Node) {
    std::stringstream SStream;
    SStream << std::hex << "N0x" << (unsigned long long)Node;
    std::string Result = SStream.str();
    return Result;
  }

  // True if we should highlight a specific context or allocation's contexts in
  // the emitted graph.
  static bool DoHighlight;
};

template <typename DerivedCCG, typename FuncTy, typename CallTy>
bool DOTGraphTraits<
    const CallsiteContextGraph<DerivedCCG, FuncTy, CallTy> *>::DoHighlight =
    false;

template <typename DerivedCCG, typename FuncTy, typename CallTy>
void CallsiteContextGraph<DerivedCCG, FuncTy, CallTy>::exportToDot(
    std::string Label) const {
  WriteGraph(this, "", false, Label,
             DotFilePathPrefix + "ccg." + Label + ".dot");
}

template <typename DerivedCCG, typename FuncTy, typename CallTy>
typename CallsiteContextGraph<DerivedCCG, FuncTy, CallTy>::ContextNode *
CallsiteContextGraph<DerivedCCG, FuncTy, CallTy>::moveEdgeToNewCalleeClone(
    const std::shared_ptr<ContextEdge> &Edge,
    DenseSet<uint32_t> ContextIdsToMove) {
  ContextNode *Node = Edge->Callee;
  assert(NodeToCallingFunc.count(Node));
  ContextNode *Clone =
      createNewNode(Node->IsAllocation, NodeToCallingFunc[Node], Node->Call);
  Node->addClone(Clone);
  Clone->MatchingCalls = Node->MatchingCalls;
  moveEdgeToExistingCalleeClone(Edge, Clone, /*NewClone=*/true,
                                ContextIdsToMove);
  return Clone;
}

template <typename DerivedCCG, typename FuncTy, typename CallTy>
void CallsiteContextGraph<DerivedCCG, FuncTy, CallTy>::
    moveEdgeToExistingCalleeClone(const std::shared_ptr<ContextEdge> &Edge,
                                  ContextNode *NewCallee, bool NewClone,
                                  DenseSet<uint32_t> ContextIdsToMove) {
  // NewCallee and Edge's current callee must be clones of the same original
  // node (Edge's current callee may be the original node too).
  assert(NewCallee->getOrigNode() == Edge->Callee->getOrigNode());

  bool EdgeIsRecursive = Edge->Callee == Edge->Caller;

  ContextNode *OldCallee = Edge->Callee;

  // We might already have an edge to the new callee from earlier cloning for a
  // different allocation. If one exists we will reuse it.
  auto ExistingEdgeToNewCallee = NewCallee->findEdgeFromCaller(Edge->Caller);

  // Callers will pass an empty ContextIdsToMove set when they want to move the
  // edge. Copy in Edge's ids for simplicity.
  if (ContextIdsToMove.empty())
    ContextIdsToMove = Edge->getContextIds();

  // If we are moving all of Edge's ids, then just move the whole Edge.
  // Otherwise only move the specified subset, to a new edge if needed.
  if (Edge->getContextIds().size() == ContextIdsToMove.size()) {
    // First, update the alloc types on New Callee from Edge.
    // Do this before we potentially clear Edge's fields below!
    NewCallee->AllocTypes |= Edge->AllocTypes;
    // Moving the whole Edge.
    if (ExistingEdgeToNewCallee) {
      // Since we already have an edge to NewCallee, simply move the ids
      // onto it, and remove the existing Edge.
      ExistingEdgeToNewCallee->getContextIds().insert_range(ContextIdsToMove);
      ExistingEdgeToNewCallee->AllocTypes |= Edge->AllocTypes;
      assert(Edge->ContextIds == ContextIdsToMove);
      removeEdgeFromGraph(Edge.get());
    } else {
      // Otherwise just reconnect Edge to NewCallee.
      Edge->Callee = NewCallee;
      NewCallee->CallerEdges.push_back(Edge);
      // Remove it from callee where it was previously connected.
      OldCallee->eraseCallerEdge(Edge.get());
      // Don't need to update Edge's context ids since we are simply
      // reconnecting it.
    }
  } else {
    // Only moving a subset of Edge's ids.
    // Compute the alloc type of the subset of ids being moved.
    auto CallerEdgeAllocType = computeAllocType(ContextIdsToMove);
    if (ExistingEdgeToNewCallee) {
      // Since we already have an edge to NewCallee, simply move the ids
      // onto it.
      ExistingEdgeToNewCallee->getContextIds().insert_range(ContextIdsToMove);
      ExistingEdgeToNewCallee->AllocTypes |= CallerEdgeAllocType;
    } else {
      // Otherwise, create a new edge to NewCallee for the ids being moved.
      auto NewEdge = std::make_shared<ContextEdge>(
          NewCallee, Edge->Caller, CallerEdgeAllocType, ContextIdsToMove);
      Edge->Caller->CalleeEdges.push_back(NewEdge);
      NewCallee->CallerEdges.push_back(NewEdge);
    }
    // In either case, need to update the alloc types on NewCallee, and remove
    // those ids and update the alloc type on the original Edge.
    NewCallee->AllocTypes |= CallerEdgeAllocType;
    set_subtract(Edge->ContextIds, ContextIdsToMove);
    Edge->AllocTypes = computeAllocType(Edge->ContextIds);
  }
  // Now walk the old callee node's callee edges and move Edge's context ids
  // over to the corresponding edge into the clone (which is created here if
  // this is a newly created clone).
  for (auto &OldCalleeEdge : OldCallee->CalleeEdges) {
    ContextNode *CalleeToUse = OldCalleeEdge->Callee;
    // If this is a direct recursion edge, use NewCallee (the clone) as the
    // callee as well, so that any edge updated/created here is also direct
    // recursive.
    if (CalleeToUse == OldCallee) {
      // If this is a recursive edge, see if we already moved a recursive edge
      // (which would have to have been this one) - if we were only moving a
      // subset of context ids it would still be on OldCallee.
      if (EdgeIsRecursive) {
        assert(OldCalleeEdge == Edge);
        continue;
      }
      CalleeToUse = NewCallee;
    }
    // The context ids moving to the new callee are the subset of this edge's
    // context ids and the context ids on the caller edge being moved.
    DenseSet<uint32_t> EdgeContextIdsToMove =
        set_intersection(OldCalleeEdge->getContextIds(), ContextIdsToMove);
    set_subtract(OldCalleeEdge->getContextIds(), EdgeContextIdsToMove);
    OldCalleeEdge->AllocTypes =
        computeAllocType(OldCalleeEdge->getContextIds());
    if (!NewClone) {
      // Update context ids / alloc type on corresponding edge to NewCallee.
      // There is a chance this may not exist if we are reusing an existing
      // clone, specifically during function assignment, where we would have
      // removed none type edges after creating the clone. If we can't find
      // a corresponding edge there, fall through to the cloning below.
      if (auto *NewCalleeEdge = NewCallee->findEdgeFromCallee(CalleeToUse)) {
        NewCalleeEdge->getContextIds().insert_range(EdgeContextIdsToMove);
        NewCalleeEdge->AllocTypes |= computeAllocType(EdgeContextIdsToMove);
        continue;
      }
    }
    auto NewEdge = std::make_shared<ContextEdge>(
        CalleeToUse, NewCallee, computeAllocType(EdgeContextIdsToMove),
        EdgeContextIdsToMove);
    NewCallee->CalleeEdges.push_back(NewEdge);
    NewEdge->Callee->CallerEdges.push_back(NewEdge);
  }
  // Recompute the node alloc type now that its callee edges have been
  // updated (since we will compute from those edges).
  OldCallee->AllocTypes = OldCallee->computeAllocType();
  // OldCallee alloc type should be None iff its context id set is now empty.
  assert((OldCallee->AllocTypes == (uint8_t)AllocationType::None) ==
         OldCallee->emptyContextIds());
  if (VerifyCCG) {
    checkNode<DerivedCCG, FuncTy, CallTy>(OldCallee, /*CheckEdges=*/false);
    checkNode<DerivedCCG, FuncTy, CallTy>(NewCallee, /*CheckEdges=*/false);
    for (const auto &OldCalleeEdge : OldCallee->CalleeEdges)
      checkNode<DerivedCCG, FuncTy, CallTy>(OldCalleeEdge->Callee,
                                            /*CheckEdges=*/false);
    for (const auto &NewCalleeEdge : NewCallee->CalleeEdges)
      checkNode<DerivedCCG, FuncTy, CallTy>(NewCalleeEdge->Callee,
                                            /*CheckEdges=*/false);
  }
}

template <typename DerivedCCG, typename FuncTy, typename CallTy>
void CallsiteContextGraph<DerivedCCG, FuncTy, CallTy>::
    moveCalleeEdgeToNewCaller(const std::shared_ptr<ContextEdge> &Edge,
                              ContextNode *NewCaller) {
  auto *OldCallee = Edge->Callee;
  auto *NewCallee = OldCallee;
  // If this edge was direct recursive, make any new/updated edge also direct
  // recursive to NewCaller.
  bool Recursive = Edge->Caller == Edge->Callee;
  if (Recursive)
    NewCallee = NewCaller;

  ContextNode *OldCaller = Edge->Caller;
  OldCaller->eraseCalleeEdge(Edge.get());

  // We might already have an edge to the new caller. If one exists we will
  // reuse it.
  auto ExistingEdgeToNewCaller = NewCaller->findEdgeFromCallee(NewCallee);

  if (ExistingEdgeToNewCaller) {
    // Since we already have an edge to NewCaller, simply move the ids
    // onto it, and remove the existing Edge.
    ExistingEdgeToNewCaller->getContextIds().insert_range(
        Edge->getContextIds());
    ExistingEdgeToNewCaller->AllocTypes |= Edge->AllocTypes;
    Edge->ContextIds.clear();
    Edge->AllocTypes = (uint8_t)AllocationType::None;
    OldCallee->eraseCallerEdge(Edge.get());
  } else {
    // Otherwise just reconnect Edge to NewCaller.
    Edge->Caller = NewCaller;
    NewCaller->CalleeEdges.push_back(Edge);
    if (Recursive) {
      assert(NewCallee == NewCaller);
      // In the case of (direct) recursive edges, we update the callee as well
      // so that it becomes recursive on the new caller.
      Edge->Callee = NewCallee;
      NewCallee->CallerEdges.push_back(Edge);
      OldCallee->eraseCallerEdge(Edge.get());
    }
    // Don't need to update Edge's context ids since we are simply
    // reconnecting it.
  }
  // In either case, need to update the alloc types on New Caller.
  NewCaller->AllocTypes |= Edge->AllocTypes;

  // Now walk the old caller node's caller edges and move Edge's context ids
  // over to the corresponding edge into the node (which is created here if
  // this is a newly created node). We can tell whether this is a newly created
  // node by seeing if it has any caller edges yet.
#ifndef NDEBUG
  bool IsNewNode = NewCaller->CallerEdges.empty();
#endif
  // If we just moved a direct recursive edge, presumably its context ids should
  // also flow out of OldCaller via some other non-recursive callee edge. We
  // don't want to remove the recursive context ids from other caller edges yet,
  // otherwise the context ids get into an inconsistent state on OldCaller.
  // We will update these context ids on the non-recursive caller edge when and
  // if they are updated on the non-recursive callee.
  if (!Recursive) {
    for (auto &OldCallerEdge : OldCaller->CallerEdges) {
      auto OldCallerCaller = OldCallerEdge->Caller;
      // The context ids moving to the new caller are the subset of this edge's
      // context ids and the context ids on the callee edge being moved.
      DenseSet<uint32_t> EdgeContextIdsToMove = set_intersection(
          OldCallerEdge->getContextIds(), Edge->getContextIds());
      if (OldCaller == OldCallerCaller) {
        OldCallerCaller = NewCaller;
        // Don't actually move this one. The caller will move it directly via a
        // call to this function with this as the Edge if it is appropriate to
        // move to a diff node that has a matching callee (itself).
        continue;
      }
      set_subtract(OldCallerEdge->getContextIds(), EdgeContextIdsToMove);
      OldCallerEdge->AllocTypes =
          computeAllocType(OldCallerEdge->getContextIds());
      // In this function we expect that any pre-existing node already has edges
      // from the same callers as the old node. That should be true in the
      // current use case, where we will remove None-type edges after copying
      // over all caller edges from the callee.
      auto *ExistingCallerEdge = NewCaller->findEdgeFromCaller(OldCallerCaller);
      // Since we would have skipped caller edges when moving a direct recursive
      // edge, this may not hold true when recursive handling enabled.
      assert(IsNewNode || ExistingCallerEdge || AllowRecursiveCallsites);
      if (ExistingCallerEdge) {
        ExistingCallerEdge->getContextIds().insert_range(EdgeContextIdsToMove);
        ExistingCallerEdge->AllocTypes |=
            computeAllocType(EdgeContextIdsToMove);
        continue;
      }
      auto NewEdge = std::make_shared<ContextEdge>(
          NewCaller, OldCallerCaller, computeAllocType(EdgeContextIdsToMove),
          EdgeContextIdsToMove);
      NewCaller->CallerEdges.push_back(NewEdge);
      NewEdge->Caller->CalleeEdges.push_back(NewEdge);
    }
  }
  // Recompute the node alloc type now that its caller edges have been
  // updated (since we will compute from those edges).
  OldCaller->AllocTypes = OldCaller->computeAllocType();
  // OldCaller alloc type should be None iff its context id set is now empty.
  assert((OldCaller->AllocTypes == (uint8_t)AllocationType::None) ==
         OldCaller->emptyContextIds());
  if (VerifyCCG) {
    checkNode<DerivedCCG, FuncTy, CallTy>(OldCaller, /*CheckEdges=*/false);
    checkNode<DerivedCCG, FuncTy, CallTy>(NewCaller, /*CheckEdges=*/false);
    for (const auto &OldCallerEdge : OldCaller->CallerEdges)
      checkNode<DerivedCCG, FuncTy, CallTy>(OldCallerEdge->Caller,
                                            /*CheckEdges=*/false);
    for (const auto &NewCallerEdge : NewCaller->CallerEdges)
      checkNode<DerivedCCG, FuncTy, CallTy>(NewCallerEdge->Caller,
                                            /*CheckEdges=*/false);
  }
}

template <typename DerivedCCG, typename FuncTy, typename CallTy>
void CallsiteContextGraph<DerivedCCG, FuncTy, CallTy>::
    recursivelyRemoveNoneTypeCalleeEdges(
        ContextNode *Node, DenseSet<const ContextNode *> &Visited) {
  auto Inserted = Visited.insert(Node);
  if (!Inserted.second)
    return;

  removeNoneTypeCalleeEdges(Node);

  for (auto *Clone : Node->Clones)
    recursivelyRemoveNoneTypeCalleeEdges(Clone, Visited);

  // The recursive call may remove some of this Node's caller edges.
  // Iterate over a copy and skip any that were removed.
  auto CallerEdges = Node->CallerEdges;
  for (auto &Edge : CallerEdges) {
    // Skip any that have been removed by an earlier recursive call.
    if (Edge->isRemoved()) {
      assert(!is_contained(Node->CallerEdges, Edge));
      continue;
    }
    recursivelyRemoveNoneTypeCalleeEdges(Edge->Caller, Visited);
  }
}

// This is the standard DFS based backedge discovery algorithm.
template <typename DerivedCCG, typename FuncTy, typename CallTy>
void CallsiteContextGraph<DerivedCCG, FuncTy, CallTy>::markBackedges() {
  // If we are cloning recursive contexts, find and mark backedges from all root
  // callers, using the typical DFS based backedge analysis.
  if (!CloneRecursiveContexts)
    return;
  DenseSet<const ContextNode *> Visited;
  DenseSet<const ContextNode *> CurrentStack;
  for (auto &Entry : NonAllocationCallToContextNodeMap) {
    auto *Node = Entry.second;
    if (Node->isRemoved())
      continue;
    // It is a root if it doesn't have callers.
    if (!Node->CallerEdges.empty())
      continue;
    markBackedges(Node, Visited, CurrentStack);
    assert(CurrentStack.empty());
  }
}

// Recursive helper for above markBackedges method.
template <typename DerivedCCG, typename FuncTy, typename CallTy>
void CallsiteContextGraph<DerivedCCG, FuncTy, CallTy>::markBackedges(
    ContextNode *Node, DenseSet<const ContextNode *> &Visited,
    DenseSet<const ContextNode *> &CurrentStack) {
  auto I = Visited.insert(Node);
  // We should only call this for unvisited nodes.
  assert(I.second);
  (void)I;
  for (auto &CalleeEdge : Node->CalleeEdges) {
    auto *Callee = CalleeEdge->Callee;
    if (Visited.count(Callee)) {
      // Since this was already visited we need to check if it is currently on
      // the recursive stack in which case it is a backedge.
      if (CurrentStack.count(Callee))
        CalleeEdge->IsBackedge = true;
      continue;
    }
    CurrentStack.insert(Callee);
    markBackedges(Callee, Visited, CurrentStack);
    CurrentStack.erase(Callee);
  }
}

template <typename DerivedCCG, typename FuncTy, typename CallTy>
void CallsiteContextGraph<DerivedCCG, FuncTy, CallTy>::identifyClones() {
  DenseSet<const ContextNode *> Visited;
  for (auto &Entry : AllocationCallToContextNodeMap) {
    Visited.clear();
    identifyClones(Entry.second, Visited, Entry.second->getContextIds());
  }
  Visited.clear();
  for (auto &Entry : AllocationCallToContextNodeMap)
    recursivelyRemoveNoneTypeCalleeEdges(Entry.second, Visited);
  if (VerifyCCG)
    check();
}

// helper function to check an AllocType is cold or notcold or both.
bool checkColdOrNotCold(uint8_t AllocType) {
  return (AllocType == (uint8_t)AllocationType::Cold) ||
         (AllocType == (uint8_t)AllocationType::NotCold) ||
         (AllocType ==
          ((uint8_t)AllocationType::Cold | (uint8_t)AllocationType::NotCold));
}

template <typename DerivedCCG, typename FuncTy, typename CallTy>
void CallsiteContextGraph<DerivedCCG, FuncTy, CallTy>::identifyClones(
    ContextNode *Node, DenseSet<const ContextNode *> &Visited,
    const DenseSet<uint32_t> &AllocContextIds) {
  if (VerifyNodes)
    checkNode<DerivedCCG, FuncTy, CallTy>(Node, /*CheckEdges=*/false);
  assert(!Node->CloneOf);

  // If Node as a null call, then either it wasn't found in the module (regular
  // LTO) or summary index (ThinLTO), or there were other conditions blocking
  // cloning (e.g. recursion, calls multiple targets, etc).
  // Do this here so that we don't try to recursively clone callers below, which
  // isn't useful at least for this node.
  if (!Node->hasCall())
    return;

  // No need to look at any callers if allocation type already unambiguous.
  if (hasSingleAllocType(Node->AllocTypes))
    return;

#ifndef NDEBUG
  auto Insert =
#endif
      Visited.insert(Node);
  // We should not have visited this node yet.
  assert(Insert.second);
  // The recursive call to identifyClones may delete the current edge from the
  // CallerEdges vector. Make a copy and iterate on that, simpler than passing
  // in an iterator and having recursive call erase from it. Other edges may
  // also get removed during the recursion, which will have null Callee and
  // Caller pointers (and are deleted later), so we skip those below.
  {
    auto CallerEdges = Node->CallerEdges;
    for (auto &Edge : CallerEdges) {
      // Skip any that have been removed by an earlier recursive call.
      if (Edge->isRemoved()) {
        assert(!is_contained(Node->CallerEdges, Edge));
        continue;
      }
      // Defer backedges. See comments further below where these edges are
      // handled during the cloning of this Node.
      if (Edge->IsBackedge) {
        // We should only mark these if cloning recursive contexts, where we
        // need to do this deferral.
        assert(CloneRecursiveContexts);
        continue;
      }
      // Ignore any caller we previously visited via another edge.
      if (!Visited.count(Edge->Caller) && !Edge->Caller->CloneOf) {
        identifyClones(Edge->Caller, Visited, AllocContextIds);
      }
    }
  }

  // Check if we reached an unambiguous call or have have only a single caller.
  if (hasSingleAllocType(Node->AllocTypes) || Node->CallerEdges.size() <= 1)
    return;

  // We need to clone.

  // Try to keep the original version as alloc type NotCold. This will make
  // cases with indirect calls or any other situation with an unknown call to
  // the original function get the default behavior. We do this by sorting the
  // CallerEdges of the Node we will clone by alloc type.
  //
  // Give NotCold edge the lowest sort priority so those edges are at the end of
  // the caller edges vector, and stay on the original version (since the below
  // code clones greedily until it finds all remaining edges have the same type
  // and leaves the remaining ones on the original Node).
  //
  // We shouldn't actually have any None type edges, so the sorting priority for
  // that is arbitrary, and we assert in that case below.
  const unsigned AllocTypeCloningPriority[] = {/*None*/ 3, /*NotCold*/ 4,
                                               /*Cold*/ 1,
                                               /*NotColdCold*/ 2};
  llvm::stable_sort(Node->CallerEdges,
                    [&](const std::shared_ptr<ContextEdge> &A,
                        const std::shared_ptr<ContextEdge> &B) {
                      // Nodes with non-empty context ids should be sorted
                      // before those with empty context ids.
                      if (A->ContextIds.empty())
                        // Either B ContextIds are non-empty (in which case we
                        // should return false because B < A), or B ContextIds
                        // are empty, in which case they are equal, and we
                        // should maintain the original relative ordering.
                        return false;
                      if (B->ContextIds.empty())
                        return true;

                      if (A->AllocTypes == B->AllocTypes)
                        // Use the first context id for each edge as a
                        // tie-breaker.
                        return *A->ContextIds.begin() < *B->ContextIds.begin();
                      return AllocTypeCloningPriority[A->AllocTypes] <
                             AllocTypeCloningPriority[B->AllocTypes];
                    });

  assert(Node->AllocTypes != (uint8_t)AllocationType::None);

  DenseSet<uint32_t> RecursiveContextIds;
  assert(AllowRecursiveContexts || !CloneRecursiveContexts);
  // If we are allowing recursive callsites, but have also disabled recursive
  // contexts, look for context ids that show up in multiple caller edges.
  if (AllowRecursiveCallsites && !AllowRecursiveContexts) {
    DenseSet<uint32_t> AllCallerContextIds;
    for (auto &CE : Node->CallerEdges) {
      // Resize to the largest set of caller context ids, since we know the
      // final set will be at least that large.
      AllCallerContextIds.reserve(CE->getContextIds().size());
      for (auto Id : CE->getContextIds())
        if (!AllCallerContextIds.insert(Id).second)
          RecursiveContextIds.insert(Id);
    }
  }

  // Iterate until we find no more opportunities for disambiguating the alloc
  // types via cloning. In most cases this loop will terminate once the Node
  // has a single allocation type, in which case no more cloning is needed.
  // Iterate over a copy of Node's caller edges, since we may need to remove
  // edges in the moveEdgeTo* methods, and this simplifies the handling and
  // makes it less error-prone.
  auto CallerEdges = Node->CallerEdges;
  for (auto &CallerEdge : CallerEdges) {
    // Skip any that have been removed by an earlier recursive call.
    if (CallerEdge->isRemoved()) {
      assert(!is_contained(Node->CallerEdges, CallerEdge));
      continue;
    }
    assert(CallerEdge->Callee == Node);

    // See if cloning the prior caller edge left this node with a single alloc
    // type or a single caller. In that case no more cloning of Node is needed.
    if (hasSingleAllocType(Node->AllocTypes) || Node->CallerEdges.size() <= 1)
      break;

    // If the caller was not successfully matched to a call in the IR/summary,
    // there is no point in trying to clone for it as we can't update that call.
    if (!CallerEdge->Caller->hasCall())
      continue;

    // Only need to process the ids along this edge pertaining to the given
    // allocation.
    auto CallerEdgeContextsForAlloc =
        set_intersection(CallerEdge->getContextIds(), AllocContextIds);
    if (!RecursiveContextIds.empty())
      CallerEdgeContextsForAlloc =
          set_difference(CallerEdgeContextsForAlloc, RecursiveContextIds);
    if (CallerEdgeContextsForAlloc.empty())
      continue;

    auto CallerAllocTypeForAlloc = computeAllocType(CallerEdgeContextsForAlloc);

    // Compute the node callee edge alloc types corresponding to the context ids
    // for this caller edge.
    std::vector<uint8_t> CalleeEdgeAllocTypesForCallerEdge;
    CalleeEdgeAllocTypesForCallerEdge.reserve(Node->CalleeEdges.size());
    for (auto &CalleeEdge : Node->CalleeEdges)
      CalleeEdgeAllocTypesForCallerEdge.push_back(intersectAllocTypes(
          CalleeEdge->getContextIds(), CallerEdgeContextsForAlloc));

    // Don't clone if doing so will not disambiguate any alloc types amongst
    // caller edges (including the callee edges that would be cloned).
    // Otherwise we will simply move all edges to the clone.
    //
    // First check if by cloning we will disambiguate the caller allocation
    // type from node's allocation type. Query allocTypeToUse so that we don't
    // bother cloning to distinguish NotCold+Cold from NotCold. Note that
    // neither of these should be None type.
    //
    // Then check if by cloning node at least one of the callee edges will be
    // disambiguated by splitting out different context ids.
    //
    // However, always do the cloning if this is a backedge, in which case we
    // have not yet cloned along this caller edge.
    assert(CallerEdge->AllocTypes != (uint8_t)AllocationType::None);
    assert(Node->AllocTypes != (uint8_t)AllocationType::None);
    if (!CallerEdge->IsBackedge &&
        allocTypeToUse(CallerAllocTypeForAlloc) ==
            allocTypeToUse(Node->AllocTypes) &&
        allocTypesMatch<DerivedCCG, FuncTy, CallTy>(
            CalleeEdgeAllocTypesForCallerEdge, Node->CalleeEdges)) {
      continue;
    }

    if (CallerEdge->IsBackedge) {
      // We should only mark these if cloning recursive contexts, where we
      // need to do this deferral.
      assert(CloneRecursiveContexts);
      DeferredBackedges++;
    }

    // If this is a backedge, we now do recursive cloning starting from its
    // caller since we may have moved unambiguous caller contexts to a clone
    // of this Node in a previous iteration of the current loop, giving more
    // opportunity for cloning through the backedge. Because we sorted the
    // caller edges earlier so that cold caller edges are first, we would have
    // visited and cloned this node for any unamibiguously cold non-recursive
    // callers before any ambiguous backedge callers. Note that we don't do this
    // if the caller is already cloned or visited during cloning (e.g. via a
    // different context path from the allocation).
    // TODO: Can we do better in the case where the caller was already visited?
    if (CallerEdge->IsBackedge && !CallerEdge->Caller->CloneOf &&
        !Visited.count(CallerEdge->Caller)) {
      const auto OrigIdCount = CallerEdge->getContextIds().size();
      // Now do the recursive cloning of this backedge's caller, which was
      // deferred earlier.
      identifyClones(CallerEdge->Caller, Visited, CallerEdgeContextsForAlloc);
      removeNoneTypeCalleeEdges(CallerEdge->Caller);
      // See if the recursive call to identifyClones moved the context ids to a
      // new edge from this node to a clone of caller, and switch to looking at
      // that new edge so that we clone Node for the new caller clone.
      bool UpdatedEdge = false;
      if (OrigIdCount > CallerEdge->getContextIds().size()) {
        for (auto E : Node->CallerEdges) {
          // Only interested in clones of the current edges caller.
          if (E->Caller->CloneOf != CallerEdge->Caller)
            continue;
          // See if this edge contains any of the context ids originally on the
          // current caller edge.
          auto CallerEdgeContextsForAllocNew =
              set_intersection(CallerEdgeContextsForAlloc, E->getContextIds());
          if (CallerEdgeContextsForAllocNew.empty())
            continue;
          // Make sure we don't pick a previously existing caller edge of this
          // Node, which would be processed on a different iteration of the
          // outer loop over the saved CallerEdges.
          if (llvm::is_contained(CallerEdges, E))
            continue;
          // The CallerAllocTypeForAlloc and CalleeEdgeAllocTypesForCallerEdge
          // are updated further below for all cases where we just invoked
          // identifyClones recursively.
          CallerEdgeContextsForAlloc.swap(CallerEdgeContextsForAllocNew);
          CallerEdge = E;
          UpdatedEdge = true;
          break;
        }
      }
      // If cloning removed this edge (and we didn't update it to a new edge
      // above), we're done with this edge. It's possible we moved all of the
      // context ids to an existing clone, in which case there's no need to do
      // further processing for them.
      if (CallerEdge->isRemoved())
        continue;

      // Now we need to update the information used for the cloning decisions
      // further below, as we may have modified edges and their context ids.

      // Note if we changed the CallerEdge above we would have already updated
      // the context ids.
      if (!UpdatedEdge) {
        CallerEdgeContextsForAlloc = set_intersection(
            CallerEdgeContextsForAlloc, CallerEdge->getContextIds());
        if (CallerEdgeContextsForAlloc.empty())
          continue;
      }
      // Update the other information that depends on the edges and on the now
      // updated CallerEdgeContextsForAlloc.
      CallerAllocTypeForAlloc = computeAllocType(CallerEdgeContextsForAlloc);
      CalleeEdgeAllocTypesForCallerEdge.clear();
      for (auto &CalleeEdge : Node->CalleeEdges) {
        CalleeEdgeAllocTypesForCallerEdge.push_back(intersectAllocTypes(
            CalleeEdge->getContextIds(), CallerEdgeContextsForAlloc));
      }
    }

    // First see if we can use an existing clone. Check each clone and its
    // callee edges for matching alloc types.
    ContextNode *Clone = nullptr;
    for (auto *CurClone : Node->Clones) {
      if (allocTypeToUse(CurClone->AllocTypes) !=
          allocTypeToUse(CallerAllocTypeForAlloc))
        continue;

      bool BothSingleAlloc = hasSingleAllocType(CurClone->AllocTypes) &&
                             hasSingleAllocType(CallerAllocTypeForAlloc);
      // The above check should mean that if both have single alloc types that
      // they should be equal.
      assert(!BothSingleAlloc ||
             CurClone->AllocTypes == CallerAllocTypeForAlloc);

      // If either both have a single alloc type (which are the same), or if the
      // clone's callee edges have the same alloc types as those for the current
      // allocation on Node's callee edges (CalleeEdgeAllocTypesForCallerEdge),
      // then we can reuse this clone.
      if (BothSingleAlloc || allocTypesMatchClone<DerivedCCG, FuncTy, CallTy>(
                                 CalleeEdgeAllocTypesForCallerEdge, CurClone)) {
        Clone = CurClone;
        break;
      }
    }

    // The edge iterator is adjusted when we move the CallerEdge to the clone.
    if (Clone)
      moveEdgeToExistingCalleeClone(CallerEdge, Clone, /*NewClone=*/false,
                                    CallerEdgeContextsForAlloc);
    else
      Clone = moveEdgeToNewCalleeClone(CallerEdge, CallerEdgeContextsForAlloc);

    // Sanity check that no alloc types on clone or its edges are None.
    assert(Clone->AllocTypes != (uint8_t)AllocationType::None);
  }

  // We should still have some context ids on the original Node.
  assert(!Node->emptyContextIds());

  // Sanity check that no alloc types on node or edges are None.
  assert(Node->AllocTypes != (uint8_t)AllocationType::None);

  if (VerifyNodes)
    checkNode<DerivedCCG, FuncTy, CallTy>(Node, /*CheckEdges=*/false);
}

void ModuleCallsiteContextGraph::updateAllocationCall(
    CallInfo &Call, AllocationType AllocType) {
  std::string AllocTypeString = getAllocTypeAttributeString(AllocType);
  auto A = llvm::Attribute::get(Call.call()->getFunction()->getContext(),
                                "memprof", AllocTypeString);
  cast<CallBase>(Call.call())->addFnAttr(A);
  OREGetter(Call.call()->getFunction())
      .emit(OptimizationRemark(DEBUG_TYPE, "MemprofAttribute", Call.call())
            << ore::NV("AllocationCall", Call.call()) << " in clone "
            << ore::NV("Caller", Call.call()->getFunction())
            << " marked with memprof allocation attribute "
            << ore::NV("Attribute", AllocTypeString));
}

void IndexCallsiteContextGraph::updateAllocationCall(CallInfo &Call,
                                                     AllocationType AllocType) {
  auto *AI = cast<AllocInfo *>(Call.call());
  assert(AI);
  assert(AI->Versions.size() > Call.cloneNo());
  AI->Versions[Call.cloneNo()] = (uint8_t)AllocType;
}

AllocationType
ModuleCallsiteContextGraph::getAllocationCallType(const CallInfo &Call) const {
  const auto *CB = cast<CallBase>(Call.call());
  if (!CB->getAttributes().hasFnAttr("memprof"))
    return AllocationType::None;
  return CB->getAttributes().getFnAttr("memprof").getValueAsString() == "cold"
             ? AllocationType::Cold
             : AllocationType::NotCold;
}

AllocationType
IndexCallsiteContextGraph::getAllocationCallType(const CallInfo &Call) const {
  const auto *AI = cast<AllocInfo *>(Call.call());
  assert(AI->Versions.size() > Call.cloneNo());
  return (AllocationType)AI->Versions[Call.cloneNo()];
}

void ModuleCallsiteContextGraph::updateCall(CallInfo &CallerCall,
                                            FuncInfo CalleeFunc) {
  auto *CurF = cast<CallBase>(CallerCall.call())->getCalledFunction();
  auto NewCalleeCloneNo = CalleeFunc.cloneNo();
  if (isMemProfClone(*CurF)) {
    // If we already assigned this callsite to call a specific non-default
    // clone (i.e. not the original function which is clone 0), ensure that we
    // aren't trying to now update it to call a different clone, which is
    // indicative of a bug in the graph or function assignment.
    auto CurCalleeCloneNo = getMemProfCloneNum(*CurF);
    if (CurCalleeCloneNo != NewCalleeCloneNo) {
      LLVM_DEBUG(dbgs() << "Mismatch in call clone assignment: was "
                        << CurCalleeCloneNo << " now " << NewCalleeCloneNo
                        << "\n");
      MismatchedCloneAssignments++;
    }
  }
  if (NewCalleeCloneNo > 0)
    cast<CallBase>(CallerCall.call())->setCalledFunction(CalleeFunc.func());
  OREGetter(CallerCall.call()->getFunction())
      .emit(OptimizationRemark(DEBUG_TYPE, "MemprofCall", CallerCall.call())
            << ore::NV("Call", CallerCall.call()) << " in clone "
            << ore::NV("Caller", CallerCall.call()->getFunction())
            << " assigned to call function clone "
            << ore::NV("Callee", CalleeFunc.func()));
}

void IndexCallsiteContextGraph::updateCall(CallInfo &CallerCall,
                                           FuncInfo CalleeFunc) {
  auto *CI = cast<CallsiteInfo *>(CallerCall.call());
  assert(CI &&
         "Caller cannot be an allocation which should not have profiled calls");
  assert(CI->Clones.size() > CallerCall.cloneNo());
  auto NewCalleeCloneNo = CalleeFunc.cloneNo();
  auto &CurCalleeCloneNo = CI->Clones[CallerCall.cloneNo()];
  // If we already assigned this callsite to call a specific non-default
  // clone (i.e. not the original function which is clone 0), ensure that we
  // aren't trying to now update it to call a different clone, which is
  // indicative of a bug in the graph or function assignment.
  if (CurCalleeCloneNo != 0 && CurCalleeCloneNo != NewCalleeCloneNo) {
    LLVM_DEBUG(dbgs() << "Mismatch in call clone assignment: was "
                      << CurCalleeCloneNo << " now " << NewCalleeCloneNo
                      << "\n");
    MismatchedCloneAssignments++;
  }
  CurCalleeCloneNo = NewCalleeCloneNo;
}

// Update the debug information attached to NewFunc to use the clone Name. Note
// this needs to be done for both any existing DISubprogram for the definition,
// as well as any separate declaration DISubprogram.
static void updateSubprogramLinkageName(Function *NewFunc, StringRef Name) {
  assert(Name == NewFunc->getName());
  auto *SP = NewFunc->getSubprogram();
  if (!SP)
    return;
  auto *MDName = MDString::get(NewFunc->getParent()->getContext(), Name);
  SP->replaceLinkageName(MDName);
  DISubprogram *Decl = SP->getDeclaration();
  if (!Decl)
    return;
  TempDISubprogram NewDecl = Decl->clone();
  NewDecl->replaceLinkageName(MDName);
  SP->replaceDeclaration(MDNode::replaceWithUniqued(std::move(NewDecl)));
}

CallsiteContextGraph<ModuleCallsiteContextGraph, Function,
                     Instruction *>::FuncInfo
ModuleCallsiteContextGraph::cloneFunctionForCallsite(
    FuncInfo &Func, CallInfo &Call, std::map<CallInfo, CallInfo> &CallMap,
    std::vector<CallInfo> &CallsWithMetadataInFunc, unsigned CloneNo) {
  // Use existing LLVM facilities for cloning and obtaining Call in clone
  ValueToValueMapTy VMap;
  auto *NewFunc = CloneFunction(Func.func(), VMap);
  std::string Name = getMemProfFuncName(Func.func()->getName(), CloneNo);
  assert(!Func.func()->getParent()->getFunction(Name));
  NewFunc->setName(Name);
  updateSubprogramLinkageName(NewFunc, Name);
  for (auto &Inst : CallsWithMetadataInFunc) {
    // This map always has the initial version in it.
    assert(Inst.cloneNo() == 0);
    CallMap[Inst] = {cast<Instruction>(VMap[Inst.call()]), CloneNo};
  }
  OREGetter(Func.func())
      .emit(OptimizationRemark(DEBUG_TYPE, "MemprofClone", Func.func())
            << "created clone " << ore::NV("NewFunction", NewFunc));
  return {NewFunc, CloneNo};
}

CallsiteContextGraph<IndexCallsiteContextGraph, FunctionSummary,
                     IndexCall>::FuncInfo
IndexCallsiteContextGraph::cloneFunctionForCallsite(
    FuncInfo &Func, CallInfo &Call, std::map<CallInfo, CallInfo> &CallMap,
    std::vector<CallInfo> &CallsWithMetadataInFunc, unsigned CloneNo) {
  // Check how many clones we have of Call (and therefore function).
  // The next clone number is the current size of versions array.
  // Confirm this matches the CloneNo provided by the caller, which is based on
  // the number of function clones we have.
  assert(CloneNo == (isa<AllocInfo *>(Call.call())
                         ? cast<AllocInfo *>(Call.call())->Versions.size()
                         : cast<CallsiteInfo *>(Call.call())->Clones.size()));
  // Walk all the instructions in this function. Create a new version for
  // each (by adding an entry to the Versions/Clones summary array), and copy
  // over the version being called for the function clone being cloned here.
  // Additionally, add an entry to the CallMap for the new function clone,
  // mapping the original call (clone 0, what is in CallsWithMetadataInFunc)
  // to the new call clone.
  for (auto &Inst : CallsWithMetadataInFunc) {
    // This map always has the initial version in it.
    assert(Inst.cloneNo() == 0);
    if (auto *AI = dyn_cast<AllocInfo *>(Inst.call())) {
      assert(AI->Versions.size() == CloneNo);
      // We assign the allocation type later (in updateAllocationCall), just add
      // an entry for it here.
      AI->Versions.push_back(0);
    } else {
      auto *CI = cast<CallsiteInfo *>(Inst.call());
      assert(CI && CI->Clones.size() == CloneNo);
      // We assign the clone number later (in updateCall), just add an entry for
      // it here.
      CI->Clones.push_back(0);
    }
    CallMap[Inst] = {Inst.call(), CloneNo};
  }
  return {Func.func(), CloneNo};
}

// We perform cloning for each allocation node separately. However, this
// sometimes results in a situation where the same node calls multiple
// clones of the same callee, created for different allocations. This
// causes issues when assigning functions to these clones, as each node can
// in reality only call a single callee clone.
//
// To address this, before assigning functions, merge callee clone nodes as
// needed using a post order traversal from the allocations. We attempt to
// use existing clones as the merge node when legal, and to share them
// among callers with the same properties (callers calling the same set of
// callee clone nodes for the same allocations).
//
// Without this fix, in some cases incorrect function assignment will lead
// to calling the wrong allocation clone.
template <typename DerivedCCG, typename FuncTy, typename CallTy>
void CallsiteContextGraph<DerivedCCG, FuncTy, CallTy>::mergeClones() {
  if (!MergeClones)
    return;

  // Generate a map from context id to the associated allocation node for use
  // when merging clones.
  DenseMap<uint32_t, ContextNode *> ContextIdToAllocationNode;
  for (auto &Entry : AllocationCallToContextNodeMap) {
    auto *Node = Entry.second;
    for (auto Id : Node->getContextIds())
      ContextIdToAllocationNode[Id] = Node->getOrigNode();
    for (auto *Clone : Node->Clones) {
      for (auto Id : Clone->getContextIds())
        ContextIdToAllocationNode[Id] = Clone->getOrigNode();
    }
  }

  // Post order traversal starting from allocations to ensure each callsite
  // calls a single clone of its callee. Callee nodes that are clones of each
  // other are merged (via new merge nodes if needed) to achieve this.
  DenseSet<const ContextNode *> Visited;
  for (auto &Entry : AllocationCallToContextNodeMap) {
    auto *Node = Entry.second;

    mergeClones(Node, Visited, ContextIdToAllocationNode);

    // Make a copy so the recursive post order traversal that may create new
    // clones doesn't mess up iteration. Note that the recursive traversal
    // itself does not call mergeClones on any of these nodes, which are all
    // (clones of) allocations.
    auto Clones = Node->Clones;
    for (auto *Clone : Clones)
      mergeClones(Clone, Visited, ContextIdToAllocationNode);
  }

  if (DumpCCG) {
    dbgs() << "CCG after merging:\n";
    dbgs() << *this;
  }
  if (ExportToDot)
    exportToDot("aftermerge");

  if (VerifyCCG) {
    check();
  }
}

// Recursive helper for above mergeClones method.
template <typename DerivedCCG, typename FuncTy, typename CallTy>
void CallsiteContextGraph<DerivedCCG, FuncTy, CallTy>::mergeClones(
    ContextNode *Node, DenseSet<const ContextNode *> &Visited,
    DenseMap<uint32_t, ContextNode *> &ContextIdToAllocationNode) {
  auto Inserted = Visited.insert(Node);
  if (!Inserted.second)
    return;

  // Make a copy since the recursive call may move a caller edge to a new
  // callee, messing up the iterator.
  auto CallerEdges = Node->CallerEdges;
  for (auto CallerEdge : CallerEdges) {
    // Skip any caller edge moved onto a different callee during recursion.
    if (CallerEdge->Callee != Node)
      continue;
    mergeClones(CallerEdge->Caller, Visited, ContextIdToAllocationNode);
  }

  // Merge for this node after we handle its callers.
  mergeNodeCalleeClones(Node, Visited, ContextIdToAllocationNode);
}

template <typename DerivedCCG, typename FuncTy, typename CallTy>
void CallsiteContextGraph<DerivedCCG, FuncTy, CallTy>::mergeNodeCalleeClones(
    ContextNode *Node, DenseSet<const ContextNode *> &Visited,
    DenseMap<uint32_t, ContextNode *> &ContextIdToAllocationNode) {
  // Ignore Node if we moved all of its contexts to clones.
  if (Node->emptyContextIds())
    return;

  // First identify groups of clones among Node's callee edges, by building
  // a map from each callee base node to the associated callee edges from Node.
  MapVector<ContextNode *, std::vector<std::shared_ptr<ContextEdge>>>
      OrigNodeToCloneEdges;
  for (const auto &E : Node->CalleeEdges) {
    auto *Callee = E->Callee;
    if (!Callee->CloneOf && Callee->Clones.empty())
      continue;
    ContextNode *Base = Callee->getOrigNode();
    OrigNodeToCloneEdges[Base].push_back(E);
  }

  // Helper for callee edge sorting below. Return true if A's callee has fewer
  // caller edges than B, or if A is a clone and B is not, or if A's first
  // context id is smaller than B's.
  auto CalleeCallerEdgeLessThan = [](const std::shared_ptr<ContextEdge> &A,
                                     const std::shared_ptr<ContextEdge> &B) {
    if (A->Callee->CallerEdges.size() != B->Callee->CallerEdges.size())
      return A->Callee->CallerEdges.size() < B->Callee->CallerEdges.size();
    if (A->Callee->CloneOf && !B->Callee->CloneOf)
      return true;
    else if (!A->Callee->CloneOf && B->Callee->CloneOf)
      return false;
    // Use the first context id for each edge as a
    // tie-breaker.
    return *A->ContextIds.begin() < *B->ContextIds.begin();
  };

  // Process each set of callee clones called by Node, performing the needed
  // merging.
  for (auto Entry : OrigNodeToCloneEdges) {
    // CalleeEdges is the set of edges from Node reaching callees that are
    // mutual clones of each other.
    auto &CalleeEdges = Entry.second;
    auto NumCalleeClones = CalleeEdges.size();
    // A single edge means there is no merging needed.
    if (NumCalleeClones == 1)
      continue;
    // Sort the CalleeEdges calling this group of clones in ascending order of
    // their caller edge counts, putting the original non-clone node first in
    // cases of a tie. This simplifies finding an existing node to use as the
    // merge node.
    llvm::stable_sort(CalleeEdges, CalleeCallerEdgeLessThan);

    /// Find other callers of the given set of callee edges that can
    /// share the same callee merge node. See the comments at this method
    /// definition for details.
    DenseSet<ContextNode *> OtherCallersToShareMerge;
    findOtherCallersToShareMerge(Node, CalleeEdges, ContextIdToAllocationNode,
                                 OtherCallersToShareMerge);

    // Now do the actual merging. Identify existing or create a new MergeNode
    // during the first iteration. Move each callee over, along with edges from
    // other callers we've determined above can share the same merge node.
    ContextNode *MergeNode = nullptr;
    DenseMap<ContextNode *, unsigned> CallerToMoveCount;
    for (auto CalleeEdge : CalleeEdges) {
      auto *OrigCallee = CalleeEdge->Callee;
      // If we don't have a MergeNode yet (only happens on the first iteration,
      // as a new one will be created when we go to move the first callee edge
      // over as needed), see if we can use this callee.
      if (!MergeNode) {
        // If there are no other callers, simply use this callee.
        if (CalleeEdge->Callee->CallerEdges.size() == 1) {
          MergeNode = OrigCallee;
          NonNewMergedNodes++;
          continue;
        }
        // Otherwise, if we have identified other caller nodes that can share
        // the merge node with Node, see if all of OrigCallee's callers are
        // going to share the same merge node. In that case we can use callee
        // (since all of its callers would move to the new merge node).
        if (!OtherCallersToShareMerge.empty()) {
          bool MoveAllCallerEdges = true;
          for (auto CalleeCallerE : OrigCallee->CallerEdges) {
            if (CalleeCallerE == CalleeEdge)
              continue;
            if (!OtherCallersToShareMerge.contains(CalleeCallerE->Caller)) {
              MoveAllCallerEdges = false;
              break;
            }
          }
          // If we are going to move all callers over, we can use this callee as
          // the MergeNode.
          if (MoveAllCallerEdges) {
            MergeNode = OrigCallee;
            NonNewMergedNodes++;
            continue;
          }
        }
      }
      // Move this callee edge, creating a new merge node if necessary.
      if (MergeNode) {
        assert(MergeNode != OrigCallee);
        moveEdgeToExistingCalleeClone(CalleeEdge, MergeNode,
                                      /*NewClone*/ false);
      } else {
        MergeNode = moveEdgeToNewCalleeClone(CalleeEdge);
        NewMergedNodes++;
      }
      // Now move all identified edges from other callers over to the merge node
      // as well.
      if (!OtherCallersToShareMerge.empty()) {
        // Make and iterate over a copy of OrigCallee's caller edges because
        // some of these will be moved off of the OrigCallee and that would mess
        // up the iteration from OrigCallee.
        auto OrigCalleeCallerEdges = OrigCallee->CallerEdges;
        for (auto &CalleeCallerE : OrigCalleeCallerEdges) {
          if (CalleeCallerE == CalleeEdge)
            continue;
          if (!OtherCallersToShareMerge.contains(CalleeCallerE->Caller))
            continue;
          CallerToMoveCount[CalleeCallerE->Caller]++;
          moveEdgeToExistingCalleeClone(CalleeCallerE, MergeNode,
                                        /*NewClone*/ false);
        }
      }
      removeNoneTypeCalleeEdges(OrigCallee);
      removeNoneTypeCalleeEdges(MergeNode);
    }
  }
}

// Look for other nodes that have edges to the same set of callee
// clones as the current Node. Those can share the eventual merge node
// (reducing cloning and binary size overhead) iff:
// - they have edges to the same set of callee clones
// - each callee edge reaches a subset of the same allocations as Node's
//   corresponding edge to the same callee clone.
// The second requirement is to ensure that we don't undo any of the
// necessary cloning to distinguish contexts with different allocation
// behavior.
// FIXME: This is somewhat conservative, as we really just need to ensure
// that they don't reach the same allocations as contexts on edges from Node
// going to any of the *other* callee clones being merged. However, that
// requires more tracking and checking to get right.
template <typename DerivedCCG, typename FuncTy, typename CallTy>
void CallsiteContextGraph<DerivedCCG, FuncTy, CallTy>::
    findOtherCallersToShareMerge(
        ContextNode *Node,
        std::vector<std::shared_ptr<ContextEdge>> &CalleeEdges,
        DenseMap<uint32_t, ContextNode *> &ContextIdToAllocationNode,
        DenseSet<ContextNode *> &OtherCallersToShareMerge) {
  auto NumCalleeClones = CalleeEdges.size();
  // This map counts how many edges to the same callee clone exist for other
  // caller nodes of each callee clone.
  DenseMap<ContextNode *, unsigned> OtherCallersToSharedCalleeEdgeCount;
  // Counts the number of other caller nodes that have edges to all callee
  // clones that don't violate the allocation context checking.
  unsigned PossibleOtherCallerNodes = 0;

  // We only need to look at other Caller nodes if the first callee edge has
  // multiple callers (recall they are sorted in ascending order above).
  if (CalleeEdges[0]->Callee->CallerEdges.size() < 2)
    return;

  // For each callee edge:
  // - Collect the count of other caller nodes calling the same callees.
  // - Collect the alloc nodes reached by contexts on each callee edge.
  DenseMap<ContextEdge *, DenseSet<ContextNode *>> CalleeEdgeToAllocNodes;
  for (auto CalleeEdge : CalleeEdges) {
    assert(CalleeEdge->Callee->CallerEdges.size() > 1);
    // For each other caller of the same callee, increment the count of
    // edges reaching the same callee clone.
    for (auto CalleeCallerEdges : CalleeEdge->Callee->CallerEdges) {
      if (CalleeCallerEdges->Caller == Node) {
        assert(CalleeCallerEdges == CalleeEdge);
        continue;
      }
      OtherCallersToSharedCalleeEdgeCount[CalleeCallerEdges->Caller]++;
      // If this caller edge now reaches all of the same callee clones,
      // increment the count of candidate other caller nodes.
      if (OtherCallersToSharedCalleeEdgeCount[CalleeCallerEdges->Caller] ==
          NumCalleeClones)
        PossibleOtherCallerNodes++;
    }
    // Collect the alloc nodes reached by contexts on each callee edge, for
    // later analysis.
    for (auto Id : CalleeEdge->getContextIds()) {
      auto *Alloc = ContextIdToAllocationNode.lookup(Id);
      if (!Alloc) {
        // FIXME: unclear why this happens occasionally, presumably
        // imperfect graph updates possibly with recursion.
        MissingAllocForContextId++;
        continue;
      }
      CalleeEdgeToAllocNodes[CalleeEdge.get()].insert(Alloc);
    }
  }

  // Now walk the callee edges again, and make sure that for each candidate
  // caller node all of its edges to the callees reach the same allocs (or
  // a subset) as those along the corresponding callee edge from Node.
  for (auto CalleeEdge : CalleeEdges) {
    assert(CalleeEdge->Callee->CallerEdges.size() > 1);
    // Stop if we do not have any (more) candidate other caller nodes.
    if (!PossibleOtherCallerNodes)
      break;
    auto &CurCalleeAllocNodes = CalleeEdgeToAllocNodes[CalleeEdge.get()];
    // Check each other caller of this callee clone.
    for (auto &CalleeCallerE : CalleeEdge->Callee->CallerEdges) {
      // Not interested in the callee edge from Node itself.
      if (CalleeCallerE == CalleeEdge)
        continue;
      // Skip any callers that didn't have callee edges to all the same
      // callee clones.
      if (OtherCallersToSharedCalleeEdgeCount[CalleeCallerE->Caller] !=
          NumCalleeClones)
        continue;
      // Make sure that each context along edge from candidate caller node
      // reaches an allocation also reached by this callee edge from Node.
      for (auto Id : CalleeCallerE->getContextIds()) {
        auto *Alloc = ContextIdToAllocationNode.lookup(Id);
        if (!Alloc)
          continue;
        // If not, simply reset the map entry to 0 so caller is ignored, and
        // reduce the count of candidate other caller nodes.
        if (!CurCalleeAllocNodes.contains(Alloc)) {
          OtherCallersToSharedCalleeEdgeCount[CalleeCallerE->Caller] = 0;
          PossibleOtherCallerNodes--;
          break;
        }
      }
    }
  }

  if (!PossibleOtherCallerNodes)
    return;

  // Build the set of other caller nodes that can use the same callee merge
  // node.
  for (auto &[OtherCaller, Count] : OtherCallersToSharedCalleeEdgeCount) {
    if (Count != NumCalleeClones)
      continue;
    OtherCallersToShareMerge.insert(OtherCaller);
  }
}

// This method assigns cloned callsites to functions, cloning the functions as
// needed. The assignment is greedy and proceeds roughly as follows:
//
// For each function Func:
//   For each call with graph Node having clones:
//     Initialize ClonesWorklist to Node and its clones
//     Initialize NodeCloneCount to 0
//     While ClonesWorklist is not empty:
//        Clone = pop front ClonesWorklist
//        NodeCloneCount++
//        If Func has been cloned less than NodeCloneCount times:
//           If NodeCloneCount is 1:
//             Assign Clone to original Func
//             Continue
//           Create a new function clone
//           If other callers not assigned to call a function clone yet:
//              Assign them to call new function clone
//              Continue
//           Assign any other caller calling the cloned version to new clone
//
//        For each caller of Clone:
//           If caller is assigned to call a specific function clone:
//             If we cannot assign Clone to that function clone:
//               Create new callsite Clone NewClone
//               Add NewClone to ClonesWorklist
//               Continue
//             Assign Clone to existing caller's called function clone
//           Else:
//             If Clone not already assigned to a function clone:
//                Assign to first function clone without assignment
//             Assign caller to selected function clone
template <typename DerivedCCG, typename FuncTy, typename CallTy>
bool CallsiteContextGraph<DerivedCCG, FuncTy, CallTy>::assignFunctions() {
  bool Changed = false;

  mergeClones();

  // Keep track of the assignment of nodes (callsites) to function clones they
  // call.
  DenseMap<ContextNode *, FuncInfo> CallsiteToCalleeFuncCloneMap;

  // Update caller node to call function version CalleeFunc, by recording the
  // assignment in CallsiteToCalleeFuncCloneMap.
  auto RecordCalleeFuncOfCallsite = [&](ContextNode *Caller,
                                        const FuncInfo &CalleeFunc) {
    assert(Caller->hasCall());
    CallsiteToCalleeFuncCloneMap[Caller] = CalleeFunc;
  };

  // Information for a single clone of this Func.
  struct FuncCloneInfo {
    // The function clone.
    FuncInfo FuncClone;
    // Remappings of each call of interest (from original uncloned call to the
    // corresponding cloned call in this function clone).
    std::map<CallInfo, CallInfo> CallMap;
  };

  // Walk all functions for which we saw calls with memprof metadata, and handle
  // cloning for each of its calls.
  for (auto &[Func, CallsWithMetadata] : FuncToCallsWithMetadata) {
    FuncInfo OrigFunc(Func);
    // Map from each clone number of OrigFunc to information about that function
    // clone (the function clone FuncInfo and call remappings). The index into
    // the vector is the clone number, as function clones are created and
    // numbered sequentially.
    std::vector<FuncCloneInfo> FuncCloneInfos;
    for (auto &Call : CallsWithMetadata) {
      ContextNode *Node = getNodeForInst(Call);
      // Skip call if we do not have a node for it (all uses of its stack ids
      // were either on inlined chains or pruned from the MIBs), or if we did
      // not create any clones for it.
      if (!Node || Node->Clones.empty())
        continue;
      assert(Node->hasCall() &&
             "Not having a call should have prevented cloning");

      // Track the assignment of function clones to clones of the current
      // callsite Node being handled.
      std::map<FuncInfo, ContextNode *> FuncCloneToCurNodeCloneMap;

      // Assign callsite version CallsiteClone to function version FuncClone,
      // and also assign (possibly cloned) Call to CallsiteClone.
      auto AssignCallsiteCloneToFuncClone = [&](const FuncInfo &FuncClone,
                                                CallInfo &Call,
                                                ContextNode *CallsiteClone,
                                                bool IsAlloc) {
        // Record the clone of callsite node assigned to this function clone.
        FuncCloneToCurNodeCloneMap[FuncClone] = CallsiteClone;

        assert(FuncCloneInfos.size() > FuncClone.cloneNo());
        std::map<CallInfo, CallInfo> &CallMap =
            FuncCloneInfos[FuncClone.cloneNo()].CallMap;
        CallInfo CallClone(Call);
        if (auto It = CallMap.find(Call); It != CallMap.end())
          CallClone = It->second;
        CallsiteClone->setCall(CallClone);
        // Need to do the same for all matching calls.
        for (auto &MatchingCall : Node->MatchingCalls) {
          CallInfo CallClone(MatchingCall);
          if (auto It = CallMap.find(MatchingCall); It != CallMap.end())
            CallClone = It->second;
          // Updates the call in the list.
          MatchingCall = CallClone;
        }
      };

      // Keep track of the clones of callsite Node that need to be assigned to
      // function clones. This list may be expanded in the loop body below if we
      // find additional cloning is required.
      std::deque<ContextNode *> ClonesWorklist;
      // Ignore original Node if we moved all of its contexts to clones.
      if (!Node->emptyContextIds())
        ClonesWorklist.push_back(Node);
      llvm::append_range(ClonesWorklist, Node->Clones);

      // Now walk through all of the clones of this callsite Node that we need,
      // and determine the assignment to a corresponding clone of the current
      // function (creating new function clones as needed).
      unsigned NodeCloneCount = 0;
      while (!ClonesWorklist.empty()) {
        ContextNode *Clone = ClonesWorklist.front();
        ClonesWorklist.pop_front();
        NodeCloneCount++;
        if (VerifyNodes)
          checkNode<DerivedCCG, FuncTy, CallTy>(Clone);

        // Need to create a new function clone if we have more callsite clones
        // than existing function clones, which would have been assigned to an
        // earlier clone in the list (we assign callsite clones to function
        // clones greedily).
        if (FuncCloneInfos.size() < NodeCloneCount) {
          // If this is the first callsite copy, assign to original function.
          if (NodeCloneCount == 1) {
            // Since FuncCloneInfos is empty in this case, no clones have
            // been created for this function yet, and no callers should have
            // been assigned a function clone for this callee node yet.
            assert(llvm::none_of(
                Clone->CallerEdges, [&](const std::shared_ptr<ContextEdge> &E) {
                  return CallsiteToCalleeFuncCloneMap.count(E->Caller);
                }));
            // Initialize with empty call map, assign Clone to original function
            // and its callers, and skip to the next clone.
            FuncCloneInfos.push_back({OrigFunc, {}});
            AssignCallsiteCloneToFuncClone(
                OrigFunc, Call, Clone,
                AllocationCallToContextNodeMap.count(Call));
            for (auto &CE : Clone->CallerEdges) {
              // Ignore any caller that does not have a recorded callsite Call.
              if (!CE->Caller->hasCall())
                continue;
              RecordCalleeFuncOfCallsite(CE->Caller, OrigFunc);
            }
            continue;
          }

          // First locate which copy of OrigFunc to clone again. If a caller
          // of this callsite clone was already assigned to call a particular
          // function clone, we need to redirect all of those callers to the
          // new function clone, and update their other callees within this
          // function.
          FuncInfo PreviousAssignedFuncClone;
          auto EI = llvm::find_if(
              Clone->CallerEdges, [&](const std::shared_ptr<ContextEdge> &E) {
                return CallsiteToCalleeFuncCloneMap.count(E->Caller);
              });
          bool CallerAssignedToCloneOfFunc = false;
          if (EI != Clone->CallerEdges.end()) {
            const std::shared_ptr<ContextEdge> &Edge = *EI;
            PreviousAssignedFuncClone =
                CallsiteToCalleeFuncCloneMap[Edge->Caller];
            CallerAssignedToCloneOfFunc = true;
          }

          // Clone function and save it along with the CallInfo map created
          // during cloning in the FuncCloneInfos.
          std::map<CallInfo, CallInfo> NewCallMap;
          unsigned CloneNo = FuncCloneInfos.size();
          assert(CloneNo > 0 && "Clone 0 is the original function, which "
                                "should already exist in the map");
          FuncInfo NewFuncClone = cloneFunctionForCallsite(
              OrigFunc, Call, NewCallMap, CallsWithMetadata, CloneNo);
          FuncCloneInfos.push_back({NewFuncClone, std::move(NewCallMap)});
          FunctionClonesAnalysis++;
          Changed = true;

          // If no caller callsites were already assigned to a clone of this
          // function, we can simply assign this clone to the new func clone
          // and update all callers to it, then skip to the next clone.
          if (!CallerAssignedToCloneOfFunc) {
            AssignCallsiteCloneToFuncClone(
                NewFuncClone, Call, Clone,
                AllocationCallToContextNodeMap.count(Call));
            for (auto &CE : Clone->CallerEdges) {
              // Ignore any caller that does not have a recorded callsite Call.
              if (!CE->Caller->hasCall())
                continue;
              RecordCalleeFuncOfCallsite(CE->Caller, NewFuncClone);
            }
            continue;
          }

          // We may need to do additional node cloning in this case.
          // Reset the CallsiteToCalleeFuncCloneMap entry for any callers
          // that were previously assigned to call PreviousAssignedFuncClone,
          // to record that they now call NewFuncClone.
          // The none type edge removal may remove some of this Clone's caller
          // edges, if it is reached via another of its caller's callees.
          // Iterate over a copy and skip any that were removed.
          auto CallerEdges = Clone->CallerEdges;
          for (auto CE : CallerEdges) {
            // Skip any that have been removed on an earlier iteration.
            if (CE->isRemoved()) {
              assert(!is_contained(Clone->CallerEdges, CE));
              continue;
            }
            assert(CE);
            // Ignore any caller that does not have a recorded callsite Call.
            if (!CE->Caller->hasCall())
              continue;

            if (!CallsiteToCalleeFuncCloneMap.count(CE->Caller) ||
                // We subsequently fall through to later handling that
                // will perform any additional cloning required for
                // callers that were calling other function clones.
                CallsiteToCalleeFuncCloneMap[CE->Caller] !=
                    PreviousAssignedFuncClone)
              continue;

            RecordCalleeFuncOfCallsite(CE->Caller, NewFuncClone);

            // If we are cloning a function that was already assigned to some
            // callers, then essentially we are creating new callsite clones
            // of the other callsites in that function that are reached by those
            // callers. Clone the other callees of the current callsite's caller
            // that were already assigned to PreviousAssignedFuncClone
            // accordingly. This is important since we subsequently update the
            // calls from the nodes in the graph and their assignments to callee
            // functions recorded in CallsiteToCalleeFuncCloneMap.
            // The none type edge removal may remove some of this caller's
            // callee edges, if it is reached via another of its callees.
            // Iterate over a copy and skip any that were removed.
            auto CalleeEdges = CE->Caller->CalleeEdges;
            for (auto CalleeEdge : CalleeEdges) {
              // Skip any that have been removed on an earlier iteration when
              // cleaning up newly None type callee edges.
              if (CalleeEdge->isRemoved()) {
                assert(!is_contained(CE->Caller->CalleeEdges, CalleeEdge));
                continue;
              }
              assert(CalleeEdge);
              ContextNode *Callee = CalleeEdge->Callee;
              // Skip the current callsite, we are looking for other
              // callsites Caller calls, as well as any that does not have a
              // recorded callsite Call.
              if (Callee == Clone || !Callee->hasCall())
                continue;
              // Skip direct recursive calls. We don't need/want to clone the
              // caller node again, and this loop will not behave as expected if
              // we tried.
              if (Callee == CalleeEdge->Caller)
                continue;
              ContextNode *NewClone = moveEdgeToNewCalleeClone(CalleeEdge);
              removeNoneTypeCalleeEdges(NewClone);
              // Moving the edge may have resulted in some none type
              // callee edges on the original Callee.
              removeNoneTypeCalleeEdges(Callee);
              assert(NewClone->AllocTypes != (uint8_t)AllocationType::None);
              // If the Callee node was already assigned to call a specific
              // function version, make sure its new clone is assigned to call
              // that same function clone.
              if (CallsiteToCalleeFuncCloneMap.count(Callee))
                RecordCalleeFuncOfCallsite(
                    NewClone, CallsiteToCalleeFuncCloneMap[Callee]);
              // Update NewClone with the new Call clone of this callsite's Call
              // created for the new function clone created earlier.
              // Recall that we have already ensured when building the graph
              // that each caller can only call callsites within the same
              // function, so we are guaranteed that Callee Call is in the
              // current OrigFunc.
              // CallMap is set up as indexed by original Call at clone 0.
              CallInfo OrigCall(Callee->getOrigNode()->Call);
              OrigCall.setCloneNo(0);
              std::map<CallInfo, CallInfo> &CallMap =
                  FuncCloneInfos[NewFuncClone.cloneNo()].CallMap;
              assert(CallMap.count(OrigCall));
              CallInfo NewCall(CallMap[OrigCall]);
              assert(NewCall);
              NewClone->setCall(NewCall);
              // Need to do the same for all matching calls.
              for (auto &MatchingCall : NewClone->MatchingCalls) {
                CallInfo OrigMatchingCall(MatchingCall);
                OrigMatchingCall.setCloneNo(0);
                assert(CallMap.count(OrigMatchingCall));
                CallInfo NewCall(CallMap[OrigMatchingCall]);
                assert(NewCall);
                // Updates the call in the list.
                MatchingCall = NewCall;
              }
            }
          }
          // Fall through to handling below to perform the recording of the
          // function for this callsite clone. This enables handling of cases
          // where the callers were assigned to different clones of a function.
        }

        auto FindFirstAvailFuncClone = [&]() {
          // Find first function in FuncCloneInfos without an assigned
          // clone of this callsite Node. We should always have one
          // available at this point due to the earlier cloning when the
          // FuncCloneInfos size was smaller than the clone number.
          for (auto &CF : FuncCloneInfos) {
            if (!FuncCloneToCurNodeCloneMap.count(CF.FuncClone))
              return CF.FuncClone;
          }
          assert(false &&
                 "Expected an available func clone for this callsite clone");
        };

        // See if we can use existing function clone. Walk through
        // all caller edges to see if any have already been assigned to
        // a clone of this callsite's function. If we can use it, do so. If not,
        // because that function clone is already assigned to a different clone
        // of this callsite, then we need to clone again.
        // Basically, this checking is needed to handle the case where different
        // caller functions/callsites may need versions of this function
        // containing different mixes of callsite clones across the different
        // callsites within the function. If that happens, we need to create
        // additional function clones to handle the various combinations.
        //
        // Keep track of any new clones of this callsite created by the
        // following loop, as well as any existing clone that we decided to
        // assign this clone to.
        std::map<FuncInfo, ContextNode *> FuncCloneToNewCallsiteCloneMap;
        FuncInfo FuncCloneAssignedToCurCallsiteClone;
        // Iterate over a copy of Clone's caller edges, since we may need to
        // remove edges in the moveEdgeTo* methods, and this simplifies the
        // handling and makes it less error-prone.
        auto CloneCallerEdges = Clone->CallerEdges;
        for (auto &Edge : CloneCallerEdges) {
          // Skip removed edges (due to direct recursive edges updated when
          // updating callee edges when moving an edge and subsequently
          // removed by call to removeNoneTypeCalleeEdges on the Clone).
          if (Edge->isRemoved())
            continue;
          // Ignore any caller that does not have a recorded callsite Call.
          if (!Edge->Caller->hasCall())
            continue;
          // If this caller already assigned to call a version of OrigFunc, need
          // to ensure we can assign this callsite clone to that function clone.
          if (CallsiteToCalleeFuncCloneMap.count(Edge->Caller)) {
            FuncInfo FuncCloneCalledByCaller =
                CallsiteToCalleeFuncCloneMap[Edge->Caller];
            // First we need to confirm that this function clone is available
            // for use by this callsite node clone.
            //
            // While FuncCloneToCurNodeCloneMap is built only for this Node and
            // its callsite clones, one of those callsite clones X could have
            // been assigned to the same function clone called by Edge's caller
            // - if Edge's caller calls another callsite within Node's original
            // function, and that callsite has another caller reaching clone X.
            // We need to clone Node again in this case.
            if ((FuncCloneToCurNodeCloneMap.count(FuncCloneCalledByCaller) &&
                 FuncCloneToCurNodeCloneMap[FuncCloneCalledByCaller] !=
                     Clone) ||
                // Detect when we have multiple callers of this callsite that
                // have already been assigned to specific, and different, clones
                // of OrigFunc (due to other unrelated callsites in Func they
                // reach via call contexts). Is this Clone of callsite Node
                // assigned to a different clone of OrigFunc? If so, clone Node
                // again.
                (FuncCloneAssignedToCurCallsiteClone &&
                 FuncCloneAssignedToCurCallsiteClone !=
                     FuncCloneCalledByCaller)) {
              // We need to use a different newly created callsite clone, in
              // order to assign it to another new function clone on a
              // subsequent iteration over the Clones array (adjusted below).
              // Note we specifically do not reset the
              // CallsiteToCalleeFuncCloneMap entry for this caller, so that
              // when this new clone is processed later we know which version of
              // the function to copy (so that other callsite clones we have
              // assigned to that function clone are properly cloned over). See
              // comments in the function cloning handling earlier.

              // Check if we already have cloned this callsite again while
              // walking through caller edges, for a caller calling the same
              // function clone. If so, we can move this edge to that new clone
              // rather than creating yet another new clone.
              if (FuncCloneToNewCallsiteCloneMap.count(
                      FuncCloneCalledByCaller)) {
                ContextNode *NewClone =
                    FuncCloneToNewCallsiteCloneMap[FuncCloneCalledByCaller];
                moveEdgeToExistingCalleeClone(Edge, NewClone);
                // Cleanup any none type edges cloned over.
                removeNoneTypeCalleeEdges(NewClone);
              } else {
                // Create a new callsite clone.
                ContextNode *NewClone = moveEdgeToNewCalleeClone(Edge);
                removeNoneTypeCalleeEdges(NewClone);
                FuncCloneToNewCallsiteCloneMap[FuncCloneCalledByCaller] =
                    NewClone;
                // Add to list of clones and process later.
                ClonesWorklist.push_back(NewClone);
                assert(NewClone->AllocTypes != (uint8_t)AllocationType::None);
              }
              // Moving the caller edge may have resulted in some none type
              // callee edges.
              removeNoneTypeCalleeEdges(Clone);
              // We will handle the newly created callsite clone in a subsequent
              // iteration over this Node's Clones.
              continue;
            }

            // Otherwise, we can use the function clone already assigned to this
            // caller.
            if (!FuncCloneAssignedToCurCallsiteClone) {
              FuncCloneAssignedToCurCallsiteClone = FuncCloneCalledByCaller;
              // Assign Clone to FuncCloneCalledByCaller
              AssignCallsiteCloneToFuncClone(
                  FuncCloneCalledByCaller, Call, Clone,
                  AllocationCallToContextNodeMap.count(Call));
            } else
              // Don't need to do anything - callsite is already calling this
              // function clone.
              assert(FuncCloneAssignedToCurCallsiteClone ==
                     FuncCloneCalledByCaller);

          } else {
            // We have not already assigned this caller to a version of
            // OrigFunc. Do the assignment now.

            // First check if we have already assigned this callsite clone to a
            // clone of OrigFunc for another caller during this iteration over
            // its caller edges.
            if (!FuncCloneAssignedToCurCallsiteClone) {
              FuncCloneAssignedToCurCallsiteClone = FindFirstAvailFuncClone();
              assert(FuncCloneAssignedToCurCallsiteClone);
              // Assign Clone to FuncCloneAssignedToCurCallsiteClone
              AssignCallsiteCloneToFuncClone(
                  FuncCloneAssignedToCurCallsiteClone, Call, Clone,
                  AllocationCallToContextNodeMap.count(Call));
            } else
              assert(FuncCloneToCurNodeCloneMap
                         [FuncCloneAssignedToCurCallsiteClone] == Clone);
            // Update callers to record function version called.
            RecordCalleeFuncOfCallsite(Edge->Caller,
                                       FuncCloneAssignedToCurCallsiteClone);
          }
        }
        // If we didn't assign a function clone to this callsite clone yet, e.g.
        // none of its callers has a non-null call, do the assignment here.
        // We want to ensure that every callsite clone is assigned to some
        // function clone, so that the call updates below work as expected.
        // In particular if this is the original callsite, we want to ensure it
        // is assigned to the original function, otherwise the original function
        // will appear available for assignment to other callsite clones,
        // leading to unintended effects. For one, the unknown and not updated
        // callers will call into cloned paths leading to the wrong hints,
        // because they still call the original function (clone 0). Also,
        // because all callsites start out as being clone 0 by default, we can't
        // easily distinguish between callsites explicitly assigned to clone 0
        // vs those never assigned, which can lead to multiple updates of the
        // calls when invoking updateCall below, with mismatched clone values.
        // TODO: Add a flag to the callsite nodes or some other mechanism to
        // better distinguish and identify callsite clones that are not getting
        // assigned to function clones as expected.
        if (!FuncCloneAssignedToCurCallsiteClone) {
          FuncCloneAssignedToCurCallsiteClone = FindFirstAvailFuncClone();
          assert(FuncCloneAssignedToCurCallsiteClone &&
                 "No available func clone for this callsite clone");
          AssignCallsiteCloneToFuncClone(
              FuncCloneAssignedToCurCallsiteClone, Call, Clone,
              /*IsAlloc=*/AllocationCallToContextNodeMap.contains(Call));
        }
      }
      if (VerifyCCG) {
        checkNode<DerivedCCG, FuncTy, CallTy>(Node);
        for (const auto &PE : Node->CalleeEdges)
          checkNode<DerivedCCG, FuncTy, CallTy>(PE->Callee);
        for (const auto &CE : Node->CallerEdges)
          checkNode<DerivedCCG, FuncTy, CallTy>(CE->Caller);
        for (auto *Clone : Node->Clones) {
          checkNode<DerivedCCG, FuncTy, CallTy>(Clone);
          for (const auto &PE : Clone->CalleeEdges)
            checkNode<DerivedCCG, FuncTy, CallTy>(PE->Callee);
          for (const auto &CE : Clone->CallerEdges)
            checkNode<DerivedCCG, FuncTy, CallTy>(CE->Caller);
        }
      }
    }
  }

  uint8_t BothTypes =
      (uint8_t)AllocationType::Cold | (uint8_t)AllocationType::NotCold;

  auto UpdateCalls = [&](ContextNode *Node,
                         DenseSet<const ContextNode *> &Visited,
                         auto &&UpdateCalls) {
    auto Inserted = Visited.insert(Node);
    if (!Inserted.second)
      return;

    for (auto *Clone : Node->Clones)
      UpdateCalls(Clone, Visited, UpdateCalls);

    for (auto &Edge : Node->CallerEdges)
      UpdateCalls(Edge->Caller, Visited, UpdateCalls);

    // Skip if either no call to update, or if we ended up with no context ids
    // (we moved all edges onto other clones).
    if (!Node->hasCall() || Node->emptyContextIds())
      return;

    if (Node->IsAllocation) {
      auto AT = allocTypeToUse(Node->AllocTypes);
      // If the allocation type is ambiguous, and more aggressive hinting
      // has been enabled via the MinClonedColdBytePercent flag, see if this
      // allocation should be hinted cold anyway because its fraction cold bytes
      // allocated is at least the given threshold.
      if (Node->AllocTypes == BothTypes && MinClonedColdBytePercent < 100 &&
          !ContextIdToContextSizeInfos.empty()) {
        uint64_t TotalCold = 0;
        uint64_t Total = 0;
        for (auto Id : Node->getContextIds()) {
          auto TypeI = ContextIdToAllocationType.find(Id);
          assert(TypeI != ContextIdToAllocationType.end());
          auto CSI = ContextIdToContextSizeInfos.find(Id);
          if (CSI != ContextIdToContextSizeInfos.end()) {
            for (auto &Info : CSI->second) {
              Total += Info.TotalSize;
              if (TypeI->second == AllocationType::Cold)
                TotalCold += Info.TotalSize;
            }
          }
        }
        if (TotalCold * 100 >= Total * MinClonedColdBytePercent)
          AT = AllocationType::Cold;
      }
      updateAllocationCall(Node->Call, AT);
      assert(Node->MatchingCalls.empty());
      return;
    }

    if (!CallsiteToCalleeFuncCloneMap.count(Node))
      return;

    auto CalleeFunc = CallsiteToCalleeFuncCloneMap[Node];
    updateCall(Node->Call, CalleeFunc);
    // Update all the matching calls as well.
    for (auto &Call : Node->MatchingCalls)
      updateCall(Call, CalleeFunc);
  };

  // Performs DFS traversal starting from allocation nodes to update calls to
  // reflect cloning decisions recorded earlier. For regular LTO this will
  // update the actual calls in the IR to call the appropriate function clone
  // (and add attributes to allocation calls), whereas for ThinLTO the decisions
  // are recorded in the summary entries.
  DenseSet<const ContextNode *> Visited;
  for (auto &Entry : AllocationCallToContextNodeMap)
    UpdateCalls(Entry.second, Visited, UpdateCalls);

  return Changed;
}

static SmallVector<std::unique_ptr<ValueToValueMapTy>, 4> createFunctionClones(
    Function &F, unsigned NumClones, Module &M, OptimizationRemarkEmitter &ORE,
    std::map<const Function *, SmallPtrSet<const GlobalAlias *, 1>>
        &FuncToAliasMap) {
  // The first "clone" is the original copy, we should only call this if we
  // needed to create new clones.
  assert(NumClones > 1);
  SmallVector<std::unique_ptr<ValueToValueMapTy>, 4> VMaps;
  VMaps.reserve(NumClones - 1);
  FunctionsClonedThinBackend++;
  for (unsigned I = 1; I < NumClones; I++) {
    VMaps.emplace_back(std::make_unique<ValueToValueMapTy>());
    auto *NewF = CloneFunction(&F, *VMaps.back());
    FunctionClonesThinBackend++;
    // Strip memprof and callsite metadata from clone as they are no longer
    // needed.
    for (auto &BB : *NewF) {
      for (auto &Inst : BB) {
        Inst.setMetadata(LLVMContext::MD_memprof, nullptr);
        Inst.setMetadata(LLVMContext::MD_callsite, nullptr);
      }
    }
    std::string Name = getMemProfFuncName(F.getName(), I);
    auto *PrevF = M.getFunction(Name);
    if (PrevF) {
      // We might have created this when adjusting callsite in another
      // function. It should be a declaration.
      assert(PrevF->isDeclaration());
      NewF->takeName(PrevF);
      PrevF->replaceAllUsesWith(NewF);
      PrevF->eraseFromParent();
    } else
      NewF->setName(Name);
    updateSubprogramLinkageName(NewF, Name);
    ORE.emit(OptimizationRemark(DEBUG_TYPE, "MemprofClone", &F)
             << "created clone " << ore::NV("NewFunction", NewF));

    // Now handle aliases to this function, and clone those as well.
    if (!FuncToAliasMap.count(&F))
      continue;
    for (auto *A : FuncToAliasMap[&F]) {
      std::string Name = getMemProfFuncName(A->getName(), I);
      auto *PrevA = M.getNamedAlias(Name);
      auto *NewA = GlobalAlias::create(A->getValueType(),
                                       A->getType()->getPointerAddressSpace(),
                                       A->getLinkage(), Name, NewF);
      NewA->copyAttributesFrom(A);
      if (PrevA) {
        // We might have created this when adjusting callsite in another
        // function. It should be a declaration.
        assert(PrevA->isDeclaration());
        NewA->takeName(PrevA);
        PrevA->replaceAllUsesWith(NewA);
        PrevA->eraseFromParent();
      }
    }
  }
  return VMaps;
}

// Locate the summary for F. This is complicated by the fact that it might
// have been internalized or promoted.
static ValueInfo findValueInfoForFunc(const Function &F, const Module &M,
                                      const ModuleSummaryIndex *ImportSummary,
                                      const Function *CallingFunc = nullptr) {
  // FIXME: Ideally we would retain the original GUID in some fashion on the
  // function (e.g. as metadata), but for now do our best to locate the
  // summary without that information.
  ValueInfo TheFnVI = ImportSummary->getValueInfo(F.getGUID());
  if (!TheFnVI)
    // See if theFn was internalized, by checking index directly with
    // original name (this avoids the name adjustment done by getGUID() for
    // internal symbols).
    TheFnVI = ImportSummary->getValueInfo(
        GlobalValue::getGUIDAssumingExternalLinkage(F.getName()));
  if (TheFnVI)
    return TheFnVI;
  // Now query with the original name before any promotion was performed.
  StringRef OrigName =
      ModuleSummaryIndex::getOriginalNameBeforePromote(F.getName());
  // When this pass is enabled, we always add thinlto_src_file provenance
  // metadata to imported function definitions, which allows us to recreate the
  // original internal symbol's GUID.
  auto SrcFileMD = F.getMetadata("thinlto_src_file");
  // If this is a call to an imported/promoted local for which we didn't import
  // the definition, the metadata will not exist on the declaration. However,
  // since we are doing this early, before any inlining in the LTO backend, we
  // can simply look at the metadata on the calling function which must have
  // been from the same module if F was an internal symbol originally.
  if (!SrcFileMD && F.isDeclaration()) {
    // We would only call this for a declaration for a direct callsite, in which
    // case the caller would have provided the calling function pointer.
    assert(CallingFunc);
    SrcFileMD = CallingFunc->getMetadata("thinlto_src_file");
    // If this is a promoted local (OrigName != F.getName()), since this is a
    // declaration, it must be imported from a different module and therefore we
    // should always find the metadata on its calling function. Any call to a
    // promoted local that came from this module should still be a definition.
    assert(SrcFileMD || OrigName == F.getName());
  }
  StringRef SrcFile = M.getSourceFileName();
  if (SrcFileMD)
    SrcFile = dyn_cast<MDString>(SrcFileMD->getOperand(0))->getString();
  std::string OrigId = GlobalValue::getGlobalIdentifier(
      OrigName, GlobalValue::InternalLinkage, SrcFile);
  TheFnVI = ImportSummary->getValueInfo(
      GlobalValue::getGUIDAssumingExternalLinkage(OrigId));
  // Internal func in original module may have gotten a numbered suffix if we
  // imported an external function with the same name. This happens
  // automatically during IR linking for naming conflicts. It would have to
  // still be internal in that case (otherwise it would have been renamed on
  // promotion in which case we wouldn't have a naming conflict).
  if (!TheFnVI && OrigName == F.getName() && F.hasLocalLinkage() &&
      F.getName().contains('.')) {
    OrigName = F.getName().rsplit('.').first;
    OrigId = GlobalValue::getGlobalIdentifier(
        OrigName, GlobalValue::InternalLinkage, SrcFile);
    TheFnVI = ImportSummary->getValueInfo(
        GlobalValue::getGUIDAssumingExternalLinkage(OrigId));
  }
  // The only way we may not have a VI is if this is a declaration created for
  // an imported reference. For distributed ThinLTO we may not have a VI for
  // such declarations in the distributed summary.
  assert(TheFnVI || F.isDeclaration());
  return TheFnVI;
}

bool MemProfContextDisambiguation::initializeIndirectCallPromotionInfo(
    Module &M) {
  ICallAnalysis = std::make_unique<ICallPromotionAnalysis>();
  Symtab = std::make_unique<InstrProfSymtab>();
  // Don't add canonical names, to avoid multiple functions to the symtab
  // when they both have the same root name with "." suffixes stripped.
  // If we pick the wrong one then this could lead to incorrect ICP and calling
  // a memprof clone that we don't actually create (resulting in linker unsats).
  // What this means is that the GUID of the function (or its PGOFuncName
  // metadata) *must* match that in the VP metadata to allow promotion.
  // In practice this should not be a limitation, since local functions should
  // have PGOFuncName metadata and global function names shouldn't need any
  // special handling (they should not get the ".llvm.*" suffix that the
  // canonicalization handling is attempting to strip).
  if (Error E = Symtab->create(M, /*InLTO=*/true, /*AddCanonical=*/false)) {
    std::string SymtabFailure = toString(std::move(E));
    M.getContext().emitError("Failed to create symtab: " + SymtabFailure);
    return false;
  }
  return true;
}

#ifndef NDEBUG
// Sanity check that the MIB stack ids match between the summary and
// instruction metadata.
static void checkAllocContextIds(
    const AllocInfo &AllocNode, const MDNode *MemProfMD,
    const CallStack<MDNode, MDNode::op_iterator> &CallsiteContext,
    const ModuleSummaryIndex *ImportSummary) {
  auto MIBIter = AllocNode.MIBs.begin();
  for (auto &MDOp : MemProfMD->operands()) {
    assert(MIBIter != AllocNode.MIBs.end());
    auto StackIdIndexIter = MIBIter->StackIdIndices.begin();
    auto *MIBMD = cast<const MDNode>(MDOp);
    MDNode *StackMDNode = getMIBStackNode(MIBMD);
    assert(StackMDNode);
    CallStack<MDNode, MDNode::op_iterator> StackContext(StackMDNode);
    auto ContextIterBegin =
        StackContext.beginAfterSharedPrefix(CallsiteContext);
    // Skip the checking on the first iteration.
    uint64_t LastStackContextId =
        (ContextIterBegin != StackContext.end() && *ContextIterBegin == 0) ? 1
                                                                           : 0;
    for (auto ContextIter = ContextIterBegin; ContextIter != StackContext.end();
         ++ContextIter) {
      // If this is a direct recursion, simply skip the duplicate
      // entries, to be consistent with how the summary ids were
      // generated during ModuleSummaryAnalysis.
      if (LastStackContextId == *ContextIter)
        continue;
      LastStackContextId = *ContextIter;
      assert(StackIdIndexIter != MIBIter->StackIdIndices.end());
      assert(ImportSummary->getStackIdAtIndex(*StackIdIndexIter) ==
             *ContextIter);
      StackIdIndexIter++;
    }
    MIBIter++;
  }
}
#endif

bool MemProfContextDisambiguation::applyImport(Module &M) {
  assert(ImportSummary);
  bool Changed = false;

  // We also need to clone any aliases that reference cloned functions, because
  // the modified callsites may invoke via the alias. Keep track of the aliases
  // for each function.
  std::map<const Function *, SmallPtrSet<const GlobalAlias *, 1>>
      FuncToAliasMap;
  for (auto &A : M.aliases()) {
    auto *Aliasee = A.getAliaseeObject();
    if (auto *F = dyn_cast<Function>(Aliasee))
      FuncToAliasMap[F].insert(&A);
  }

  if (!initializeIndirectCallPromotionInfo(M))
    return false;

  for (auto &F : M) {
    if (F.isDeclaration() || isMemProfClone(F))
      continue;

    OptimizationRemarkEmitter ORE(&F);

    SmallVector<std::unique_ptr<ValueToValueMapTy>, 4> VMaps;
    bool ClonesCreated = false;
    unsigned NumClonesCreated = 0;
    auto CloneFuncIfNeeded = [&](unsigned NumClones) {
      // We should at least have version 0 which is the original copy.
      assert(NumClones > 0);
      // If only one copy needed use original.
      if (NumClones == 1)
        return;
      // If we already performed cloning of this function, confirm that the
      // requested number of clones matches (the thin link should ensure the
      // number of clones for each constituent callsite is consistent within
      // each function), before returning.
      if (ClonesCreated) {
        assert(NumClonesCreated == NumClones);
        return;
      }
      VMaps = createFunctionClones(F, NumClones, M, ORE, FuncToAliasMap);
      // The first "clone" is the original copy, which doesn't have a VMap.
      assert(VMaps.size() == NumClones - 1);
      Changed = true;
      ClonesCreated = true;
      NumClonesCreated = NumClones;
    };

    auto CloneCallsite = [&](const CallsiteInfo &StackNode, CallBase *CB,
                             Function *CalledFunction) {
      // Perform cloning if not yet done.
      CloneFuncIfNeeded(/*NumClones=*/StackNode.Clones.size());

      assert(!isMemProfClone(*CalledFunction));

      // Because we update the cloned calls by calling setCalledOperand (see
      // comment below), out of an abundance of caution make sure the called
      // function was actually the called operand (or its aliasee). We also
      // strip pointer casts when looking for calls (to match behavior during
      // summary generation), however, with opaque pointers in theory this
      // should not be an issue. Note we still clone the current function
      // (containing this call) above, as that could be needed for its callers.
      auto *GA = dyn_cast_or_null<GlobalAlias>(CB->getCalledOperand());
      if (CalledFunction != CB->getCalledOperand() &&
          (!GA || CalledFunction != GA->getAliaseeObject())) {
        SkippedCallsCloning++;
        return;
      }
      // Update the calls per the summary info.
      // Save orig name since it gets updated in the first iteration
      // below.
      auto CalleeOrigName = CalledFunction->getName();
      for (unsigned J = 0; J < StackNode.Clones.size(); J++) {
        // Do nothing if this version calls the original version of its
        // callee.
        if (!StackNode.Clones[J])
          continue;
        auto NewF = M.getOrInsertFunction(
            getMemProfFuncName(CalleeOrigName, StackNode.Clones[J]),
            CalledFunction->getFunctionType());
        CallBase *CBClone;
        // Copy 0 is the original function.
        if (!J)
          CBClone = CB;
        else
          CBClone = cast<CallBase>((*VMaps[J - 1])[CB]);
        // Set the called operand directly instead of calling setCalledFunction,
        // as the latter mutates the function type on the call. In rare cases
        // we may have a slightly different type on a callee function
        // declaration due to it being imported from a different module with
        // incomplete types. We really just want to change the name of the
        // function to the clone, and not make any type changes.
        CBClone->setCalledOperand(NewF.getCallee());
        ORE.emit(OptimizationRemark(DEBUG_TYPE, "MemprofCall", CBClone)
                 << ore::NV("Call", CBClone) << " in clone "
                 << ore::NV("Caller", CBClone->getFunction())
                 << " assigned to call function clone "
                 << ore::NV("Callee", NewF.getCallee()));
      }
    };

    // Locate the summary for F.
    ValueInfo TheFnVI = findValueInfoForFunc(F, M, ImportSummary);
    // If not found, this could be an imported local (see comment in
    // findValueInfoForFunc). Skip for now as it will be cloned in its original
    // module (where it would have been promoted to global scope so should
    // satisfy any reference in this module).
    if (!TheFnVI)
      continue;

    auto *GVSummary =
        ImportSummary->findSummaryInModule(TheFnVI, M.getModuleIdentifier());
    if (!GVSummary) {
      // Must have been imported, use the summary which matches the definition。
      // (might be multiple if this was a linkonce_odr).
      auto SrcModuleMD = F.getMetadata("thinlto_src_module");
      assert(SrcModuleMD &&
             "enable-import-metadata is needed to emit thinlto_src_module");
      StringRef SrcModule =
          dyn_cast<MDString>(SrcModuleMD->getOperand(0))->getString();
      for (auto &GVS : TheFnVI.getSummaryList()) {
        if (GVS->modulePath() == SrcModule) {
          GVSummary = GVS.get();
          break;
        }
      }
      assert(GVSummary && GVSummary->modulePath() == SrcModule);
    }

    // If this was an imported alias skip it as we won't have the function
    // summary, and it should be cloned in the original module.
    if (isa<AliasSummary>(GVSummary))
      continue;

    auto *FS = cast<FunctionSummary>(GVSummary->getBaseObject());

    if (FS->allocs().empty() && FS->callsites().empty())
      continue;

    auto SI = FS->callsites().begin();
    auto AI = FS->allocs().begin();

    // To handle callsite infos synthesized for tail calls which have missing
    // frames in the profiled context, map callee VI to the synthesized callsite
    // info.
    DenseMap<ValueInfo, CallsiteInfo> MapTailCallCalleeVIToCallsite;
    // Iterate the callsites for this function in reverse, since we place all
    // those synthesized for tail calls at the end.
    for (auto CallsiteIt = FS->callsites().rbegin();
         CallsiteIt != FS->callsites().rend(); CallsiteIt++) {
      auto &Callsite = *CallsiteIt;
      // Stop as soon as we see a non-synthesized callsite info (see comment
      // above loop). All the entries added for discovered tail calls have empty
      // stack ids.
      if (!Callsite.StackIdIndices.empty())
        break;
      MapTailCallCalleeVIToCallsite.insert({Callsite.Callee, Callsite});
    }

    // Keeps track of needed ICP for the function.
    SmallVector<ICallAnalysisData> ICallAnalysisInfo;

    // Assume for now that the instructions are in the exact same order
    // as when the summary was created, but confirm this is correct by
    // matching the stack ids.
    for (auto &BB : F) {
      for (auto &I : BB) {
        auto *CB = dyn_cast<CallBase>(&I);
        // Same handling as when creating module summary.
        if (!mayHaveMemprofSummary(CB))
          continue;

        auto *CalledValue = CB->getCalledOperand();
        auto *CalledFunction = CB->getCalledFunction();
        if (CalledValue && !CalledFunction) {
          CalledValue = CalledValue->stripPointerCasts();
          // Stripping pointer casts can reveal a called function.
          CalledFunction = dyn_cast<Function>(CalledValue);
        }
        // Check if this is an alias to a function. If so, get the
        // called aliasee for the checks below.
        if (auto *GA = dyn_cast<GlobalAlias>(CalledValue)) {
          assert(!CalledFunction &&
                 "Expected null called function in callsite for alias");
          CalledFunction = dyn_cast<Function>(GA->getAliaseeObject());
        }

        CallStack<MDNode, MDNode::op_iterator> CallsiteContext(
            I.getMetadata(LLVMContext::MD_callsite));
        auto *MemProfMD = I.getMetadata(LLVMContext::MD_memprof);

        // Include allocs that were already assigned a memprof function
        // attribute in the statistics.
        if (CB->getAttributes().hasFnAttr("memprof")) {
          assert(!MemProfMD);
          CB->getAttributes().getFnAttr("memprof").getValueAsString() == "cold"
              ? AllocTypeColdThinBackend++
              : AllocTypeNotColdThinBackend++;
          OrigAllocsThinBackend++;
          AllocVersionsThinBackend++;
          if (!MaxAllocVersionsThinBackend)
            MaxAllocVersionsThinBackend = 1;
          continue;
        }

        if (MemProfMD) {
          // Consult the next alloc node.
          assert(AI != FS->allocs().end());
          auto &AllocNode = *(AI++);

#ifndef NDEBUG
          checkAllocContextIds(AllocNode, MemProfMD, CallsiteContext,
                               ImportSummary);
#endif

          // Perform cloning if not yet done.
          CloneFuncIfNeeded(/*NumClones=*/AllocNode.Versions.size());

          OrigAllocsThinBackend++;
          AllocVersionsThinBackend += AllocNode.Versions.size();
          if (MaxAllocVersionsThinBackend < AllocNode.Versions.size())
            MaxAllocVersionsThinBackend = AllocNode.Versions.size();

          // If there is only one version that means we didn't end up
          // considering this function for cloning, and in that case the alloc
          // will still be none type or should have gotten the default NotCold.
          // Skip that after calling clone helper since that does some sanity
          // checks that confirm we haven't decided yet that we need cloning.
          // We might have a single version that is cold due to the
          // MinClonedColdBytePercent heuristic, make sure we don't skip in that
          // case.
          if (AllocNode.Versions.size() == 1 &&
              (AllocationType)AllocNode.Versions[0] != AllocationType::Cold) {
            assert((AllocationType)AllocNode.Versions[0] ==
                       AllocationType::NotCold ||
                   (AllocationType)AllocNode.Versions[0] ==
                       AllocationType::None);
            UnclonableAllocsThinBackend++;
            continue;
          }

          // All versions should have a singular allocation type.
          assert(llvm::none_of(AllocNode.Versions, [](uint8_t Type) {
            return Type == ((uint8_t)AllocationType::NotCold |
                            (uint8_t)AllocationType::Cold);
          }));

          // Update the allocation types per the summary info.
          for (unsigned J = 0; J < AllocNode.Versions.size(); J++) {
            // Ignore any that didn't get an assigned allocation type.
            if (AllocNode.Versions[J] == (uint8_t)AllocationType::None)
              continue;
            AllocationType AllocTy = (AllocationType)AllocNode.Versions[J];
            AllocTy == AllocationType::Cold ? AllocTypeColdThinBackend++
                                            : AllocTypeNotColdThinBackend++;
            std::string AllocTypeString = getAllocTypeAttributeString(AllocTy);
            auto A = llvm::Attribute::get(F.getContext(), "memprof",
                                          AllocTypeString);
            CallBase *CBClone;
            // Copy 0 is the original function.
            if (!J)
              CBClone = CB;
            else
              // Since VMaps are only created for new clones, we index with
              // clone J-1 (J==0 is the original clone and does not have a VMaps
              // entry).
              CBClone = cast<CallBase>((*VMaps[J - 1])[CB]);
            CBClone->addFnAttr(A);
            ORE.emit(OptimizationRemark(DEBUG_TYPE, "MemprofAttribute", CBClone)
                     << ore::NV("AllocationCall", CBClone) << " in clone "
                     << ore::NV("Caller", CBClone->getFunction())
                     << " marked with memprof allocation attribute "
                     << ore::NV("Attribute", AllocTypeString));
          }
        } else if (!CallsiteContext.empty()) {
          if (!CalledFunction) {
#ifndef NDEBUG
            // We should have skipped inline assembly calls.
            auto *CI = dyn_cast<CallInst>(CB);
            assert(!CI || !CI->isInlineAsm());
#endif
            // We should have skipped direct calls via a Constant.
            assert(CalledValue && !isa<Constant>(CalledValue));

            // This is an indirect call, see if we have profile information and
            // whether any clones were recorded for the profiled targets (that
            // we synthesized CallsiteInfo summary records for when building the
            // index).
            auto NumClones =
                recordICPInfo(CB, FS->callsites(), SI, ICallAnalysisInfo);

            // Perform cloning if not yet done. This is done here in case
            // we don't need to do ICP, but might need to clone this
            // function as it is the target of other cloned calls.
            if (NumClones)
              CloneFuncIfNeeded(NumClones);
          }

          else {
            // Consult the next callsite node.
            assert(SI != FS->callsites().end());
            auto &StackNode = *(SI++);

#ifndef NDEBUG
            // Sanity check that the stack ids match between the summary and
            // instruction metadata.
            auto StackIdIndexIter = StackNode.StackIdIndices.begin();
            for (auto StackId : CallsiteContext) {
              assert(StackIdIndexIter != StackNode.StackIdIndices.end());
              assert(ImportSummary->getStackIdAtIndex(*StackIdIndexIter) ==
                     StackId);
              StackIdIndexIter++;
            }
#endif

            CloneCallsite(StackNode, CB, CalledFunction);
          }
        } else if (CB->isTailCall() && CalledFunction) {
          // Locate the synthesized callsite info for the callee VI, if any was
          // created, and use that for cloning.
          ValueInfo CalleeVI =
              findValueInfoForFunc(*CalledFunction, M, ImportSummary, &F);
          if (CalleeVI && MapTailCallCalleeVIToCallsite.count(CalleeVI)) {
            auto Callsite = MapTailCallCalleeVIToCallsite.find(CalleeVI);
            assert(Callsite != MapTailCallCalleeVIToCallsite.end());
            CloneCallsite(Callsite->second, CB, CalledFunction);
          }
        }
      }
    }

    // Now do any promotion required for cloning.
    performICP(M, FS->callsites(), VMaps, ICallAnalysisInfo, ORE);
  }

  // We skip some of the functions and instructions above, so remove all the
  // metadata in a single sweep here.
  for (auto &F : M) {
    // We can skip memprof clones because createFunctionClones already strips
    // the metadata from the newly created clones.
    if (F.isDeclaration() || isMemProfClone(F))
      continue;
    for (auto &BB : F) {
      for (auto &I : BB) {
        if (!isa<CallBase>(I))
          continue;
        I.setMetadata(LLVMContext::MD_memprof, nullptr);
        I.setMetadata(LLVMContext::MD_callsite, nullptr);
      }
    }
  }

  return Changed;
}

unsigned MemProfContextDisambiguation::recordICPInfo(
    CallBase *CB, ArrayRef<CallsiteInfo> AllCallsites,
    ArrayRef<CallsiteInfo>::iterator &SI,
    SmallVector<ICallAnalysisData> &ICallAnalysisInfo) {
  // First see if we have profile information for this indirect call.
  uint32_t NumCandidates;
  uint64_t TotalCount;
  auto CandidateProfileData =
      ICallAnalysis->getPromotionCandidatesForInstruction(CB, TotalCount,
                                                          NumCandidates);
  if (CandidateProfileData.empty())
    return 0;

  // Iterate through all of the candidate profiled targets along with the
  // CallsiteInfo summary records synthesized for them when building the index,
  // and see if any are cloned and/or refer to clones.
  bool ICPNeeded = false;
  unsigned NumClones = 0;
  size_t CallsiteInfoStartIndex = std::distance(AllCallsites.begin(), SI);
  for (const auto &Candidate : CandidateProfileData) {
#ifndef NDEBUG
    auto CalleeValueInfo =
#endif
        ImportSummary->getValueInfo(Candidate.Value);
    // We might not have a ValueInfo if this is a distributed
    // ThinLTO backend and decided not to import that function.
    assert(!CalleeValueInfo || SI->Callee == CalleeValueInfo);
    assert(SI != AllCallsites.end());
    auto &StackNode = *(SI++);
    // See if any of the clones of the indirect callsite for this
    // profiled target should call a cloned version of the profiled
    // target. We only need to do the ICP here if so.
    ICPNeeded |= llvm::any_of(StackNode.Clones,
                              [](unsigned CloneNo) { return CloneNo != 0; });
    // Every callsite in the same function should have been cloned the same
    // number of times.
    assert(!NumClones || NumClones == StackNode.Clones.size());
    NumClones = StackNode.Clones.size();
  }
  if (!ICPNeeded)
    return NumClones;
  // Save information for ICP, which is performed later to avoid messing up the
  // current function traversal.
  ICallAnalysisInfo.push_back({CB, CandidateProfileData.vec(), NumCandidates,
                               TotalCount, CallsiteInfoStartIndex});
  return NumClones;
}

void MemProfContextDisambiguation::performICP(
    Module &M, ArrayRef<CallsiteInfo> AllCallsites,
    ArrayRef<std::unique_ptr<ValueToValueMapTy>> VMaps,
    ArrayRef<ICallAnalysisData> ICallAnalysisInfo,
    OptimizationRemarkEmitter &ORE) {
  // Now do any promotion required for cloning. Specifically, for each
  // recorded ICP candidate (which was only recorded because one clone of that
  // candidate should call a cloned target), we perform ICP (speculative
  // devirtualization) for each clone of the callsite, and update its callee
  // to the appropriate clone. Note that the ICP compares against the original
  // version of the target, which is what is in the vtable.
  for (auto &Info : ICallAnalysisInfo) {
    auto *CB = Info.CB;
    auto CallsiteIndex = Info.CallsiteInfoStartIndex;
    auto TotalCount = Info.TotalCount;
    unsigned NumPromoted = 0;
    unsigned NumClones = 0;

    for (auto &Candidate : Info.CandidateProfileData) {
      auto &StackNode = AllCallsites[CallsiteIndex++];

      // All calls in the same function must have the same number of clones.
      assert(!NumClones || NumClones == StackNode.Clones.size());
      NumClones = StackNode.Clones.size();

      // See if the target is in the module. If it wasn't imported, it is
      // possible that this profile could have been collected on a different
      // target (or version of the code), and we need to be conservative
      // (similar to what is done in the ICP pass).
      Function *TargetFunction = Symtab->getFunction(Candidate.Value);
      if (TargetFunction == nullptr ||
          // Any ThinLTO global dead symbol removal should have already
          // occurred, so it should be safe to promote when the target is a
          // declaration.
          // TODO: Remove internal option once more fully tested.
          (MemProfRequireDefinitionForPromotion &&
           TargetFunction->isDeclaration())) {
        ORE.emit([&]() {
          return OptimizationRemarkMissed(DEBUG_TYPE, "UnableToFindTarget", CB)
                 << "Memprof cannot promote indirect call: target with md5sum "
                 << ore::NV("target md5sum", Candidate.Value) << " not found";
        });
        // FIXME: See if we can use the new declaration importing support to
        // at least get the declarations imported for this case. Hot indirect
        // targets should have been imported normally, however.
        continue;
      }

      // Check if legal to promote
      const char *Reason = nullptr;
      if (!isLegalToPromote(*CB, TargetFunction, &Reason)) {
        ORE.emit([&]() {
          return OptimizationRemarkMissed(DEBUG_TYPE, "UnableToPromote", CB)
                 << "Memprof cannot promote indirect call to "
                 << ore::NV("TargetFunction", TargetFunction)
                 << " with count of " << ore::NV("TotalCount", TotalCount)
                 << ": " << Reason;
        });
        continue;
      }

      assert(!isMemProfClone(*TargetFunction));

      // Handle each call clone, applying ICP so that each clone directly
      // calls the specified callee clone, guarded by the appropriate ICP
      // check.
      CallBase *CBClone = CB;
      for (unsigned J = 0; J < NumClones; J++) {
        // Copy 0 is the original function.
        if (J > 0)
          CBClone = cast<CallBase>((*VMaps[J - 1])[CB]);
        // We do the promotion using the original name, so that the comparison
        // is against the name in the vtable. Then just below, change the new
        // direct call to call the cloned function.
        auto &DirectCall =
            pgo::promoteIndirectCall(*CBClone, TargetFunction, Candidate.Count,
                                     TotalCount, isSamplePGO, &ORE);
        auto *TargetToUse = TargetFunction;
        // Call original if this version calls the original version of its
        // callee.
        if (StackNode.Clones[J]) {
          TargetToUse =
              cast<Function>(M.getOrInsertFunction(
                                  getMemProfFuncName(TargetFunction->getName(),
                                                     StackNode.Clones[J]),
                                  TargetFunction->getFunctionType())
                                 .getCallee());
        }
        DirectCall.setCalledFunction(TargetToUse);
        // During matching we generate synthetic VP metadata for indirect calls
        // not already having any, from the memprof profile's callee GUIDs. If
        // we subsequently promote and inline those callees, we currently lose
        // the ability to generate this synthetic VP metadata. Optionally apply
        // a noinline attribute to promoted direct calls, where the threshold is
        // set to capture synthetic VP metadata targets which get a count of 1.
        if (MemProfICPNoInlineThreshold &&
            Candidate.Count < MemProfICPNoInlineThreshold)
          DirectCall.setIsNoInline();
        ORE.emit(OptimizationRemark(DEBUG_TYPE, "MemprofCall", CBClone)
                 << ore::NV("Call", CBClone) << " in clone "
                 << ore::NV("Caller", CBClone->getFunction())
                 << " promoted and assigned to call function clone "
                 << ore::NV("Callee", TargetToUse));
      }

      // Update TotalCount (all clones should get same count above)
      TotalCount -= Candidate.Count;
      NumPromoted++;
    }
    // Adjust the MD.prof metadata for all clones, now that we have the new
    // TotalCount and the number promoted.
    CallBase *CBClone = CB;
    for (unsigned J = 0; J < NumClones; J++) {
      // Copy 0 is the original function.
      if (J > 0)
        CBClone = cast<CallBase>((*VMaps[J - 1])[CB]);
      // First delete the old one.
      CBClone->setMetadata(LLVMContext::MD_prof, nullptr);
      // If all promoted, we don't need the MD.prof metadata.
      // Otherwise we need update with the un-promoted records back.
      if (TotalCount != 0)
        annotateValueSite(
            M, *CBClone, ArrayRef(Info.CandidateProfileData).slice(NumPromoted),
            TotalCount, IPVK_IndirectCallTarget, Info.NumCandidates);
    }
  }
}

template <typename DerivedCCG, typename FuncTy, typename CallTy>
bool CallsiteContextGraph<DerivedCCG, FuncTy, CallTy>::process() {
  if (DumpCCG) {
    dbgs() << "CCG before cloning:\n";
    dbgs() << *this;
  }
  if (ExportToDot)
    exportToDot("postbuild");

  if (VerifyCCG) {
    check();
  }

  identifyClones();

  if (VerifyCCG) {
    check();
  }

  if (DumpCCG) {
    dbgs() << "CCG after cloning:\n";
    dbgs() << *this;
  }
  if (ExportToDot)
    exportToDot("cloned");

  bool Changed = assignFunctions();

  if (DumpCCG) {
    dbgs() << "CCG after assigning function clones:\n";
    dbgs() << *this;
  }
  if (ExportToDot)
    exportToDot("clonefuncassign");

  if (MemProfReportHintedSizes)
    printTotalSizes(errs());

  return Changed;
}

bool MemProfContextDisambiguation::processModule(
    Module &M,
    llvm::function_ref<OptimizationRemarkEmitter &(Function *)> OREGetter) {

  // If we have an import summary, then the cloning decisions were made during
  // the thin link on the index. Apply them and return.
  if (ImportSummary)
    return applyImport(M);

  // TODO: If/when other types of memprof cloning are enabled beyond just for
  // hot and cold, we will need to change this to individually control the
  // AllocationType passed to addStackNodesForMIB during CCG construction.
  // Note that we specifically check this after applying imports above, so that
  // the option isn't needed to be passed to distributed ThinLTO backend
  // clang processes, which won't necessarily have visibility into the linker
  // dependences. Instead the information is communicated from the LTO link to
  // the backends via the combined summary index.
  if (!SupportsHotColdNew)
    return false;

  ModuleCallsiteContextGraph CCG(M, OREGetter);
  return CCG.process();
}

MemProfContextDisambiguation::MemProfContextDisambiguation(
    const ModuleSummaryIndex *Summary, bool isSamplePGO)
    : ImportSummary(Summary), isSamplePGO(isSamplePGO) {
  // Check the dot graph printing options once here, to make sure we have valid
  // and expected combinations.
  if (DotGraphScope == DotScope::Alloc && !AllocIdForDot.getNumOccurrences())
    llvm::report_fatal_error(
        "-memprof-dot-scope=alloc requires -memprof-dot-alloc-id");
  if (DotGraphScope == DotScope::Context &&
      !ContextIdForDot.getNumOccurrences())
    llvm::report_fatal_error(
        "-memprof-dot-scope=context requires -memprof-dot-context-id");
  if (DotGraphScope == DotScope::All && AllocIdForDot.getNumOccurrences() &&
      ContextIdForDot.getNumOccurrences())
    llvm::report_fatal_error(
        "-memprof-dot-scope=all can't have both -memprof-dot-alloc-id and "
        "-memprof-dot-context-id");
  if (ImportSummary) {
    // The MemProfImportSummary should only be used for testing ThinLTO
    // distributed backend handling via opt, in which case we don't have a
    // summary from the pass pipeline.
    assert(MemProfImportSummary.empty());
    return;
  }
  if (MemProfImportSummary.empty())
    return;

  auto ReadSummaryFile =
      errorOrToExpected(MemoryBuffer::getFile(MemProfImportSummary));
  if (!ReadSummaryFile) {
    logAllUnhandledErrors(ReadSummaryFile.takeError(), errs(),
                          "Error loading file '" + MemProfImportSummary +
                              "': ");
    return;
  }
  auto ImportSummaryForTestingOrErr = getModuleSummaryIndex(**ReadSummaryFile);
  if (!ImportSummaryForTestingOrErr) {
    logAllUnhandledErrors(ImportSummaryForTestingOrErr.takeError(), errs(),
                          "Error parsing file '" + MemProfImportSummary +
                              "': ");
    return;
  }
  ImportSummaryForTesting = std::move(*ImportSummaryForTestingOrErr);
  ImportSummary = ImportSummaryForTesting.get();
}

PreservedAnalyses MemProfContextDisambiguation::run(Module &M,
                                                    ModuleAnalysisManager &AM) {
  auto &FAM = AM.getResult<FunctionAnalysisManagerModuleProxy>(M).getManager();
  auto OREGetter = [&](Function *F) -> OptimizationRemarkEmitter & {
    return FAM.getResult<OptimizationRemarkEmitterAnalysis>(*F);
  };
  if (!processModule(M, OREGetter))
    return PreservedAnalyses::all();
  return PreservedAnalyses::none();
}

void MemProfContextDisambiguation::run(
    ModuleSummaryIndex &Index,
    llvm::function_ref<bool(GlobalValue::GUID, const GlobalValueSummary *)>
        isPrevailing) {
  // TODO: If/when other types of memprof cloning are enabled beyond just for
  // hot and cold, we will need to change this to individually control the
  // AllocationType passed to addStackNodesForMIB during CCG construction.
  // The index was set from the option, so these should be in sync.
  assert(Index.withSupportsHotColdNew() == SupportsHotColdNew);
  if (!SupportsHotColdNew)
    return;

  IndexCallsiteContextGraph CCG(Index, isPrevailing);
  CCG.process();
}
