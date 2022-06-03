/* Copyright 2022 Stanford University, NVIDIA Corporation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef __LEGION_INSTANCES_H__
#define __LEGION_INSTANCES_H__

#include "legion/runtime.h"
#include "legion/legion_types.h"
#include "legion/legion_utilities.h"
#include "legion/legion_allocation.h"
#include "legion/garbage_collection.h"

namespace Legion {
  namespace Internal {

    /**
     * \class LayoutDescription
     * This class is for deduplicating the meta-data
     * associated with describing the layouts of physical
     * instances. Often times this meta data is rather 
     * large (~100K) and since we routinely create up
     * to 100K instances, it is important to deduplicate
     * the data.  Since many instances will have the
     * same layout then they can all share the same
     * description object.
     */
    class LayoutDescription : public Collectable,
                              public LegionHeapify<LayoutDescription> {
    public:
      LayoutDescription(FieldSpaceNode *owner,
                        const FieldMask &mask,
                        const unsigned total_dims,
                        LayoutConstraints *constraints,
                        const std::vector<unsigned> &mask_index_map,
                        const std::vector<FieldID> &fids,
                        const std::vector<size_t> &field_sizes,
                        const std::vector<CustomSerdezID> &serdez);
      // Used only by the virtual manager
      LayoutDescription(const FieldMask &mask, LayoutConstraints *constraints);
      LayoutDescription(const LayoutDescription &rhs);
      ~LayoutDescription(void);
    public:
      LayoutDescription& operator=(const LayoutDescription &rhs);
    public:
      void log_instance_layout(ApEvent inst_event) const;
    public:
      void compute_copy_offsets(const FieldMask &copy_mask, 
                                const PhysicalInstance instance,  
#ifdef LEGION_SPY
                                const ApEvent inst_event, 
#endif
                                std::vector<CopySrcDstField> &fields);
      void compute_copy_offsets(const std::vector<FieldID> &copy_fields,
                                const PhysicalInstance instance,
#ifdef LEGION_SPY
                                const ApEvent inst_event,
#endif
                                std::vector<CopySrcDstField> &fields);
    public:
      void get_fields(std::set<FieldID> &fields) const;
      bool has_field(FieldID fid) const;
      void has_fields(std::map<FieldID,bool> &fields) const;
      void remove_space_fields(std::set<FieldID> &fields) const;
    public:
      const CopySrcDstField& find_field_info(FieldID fid) const;
      size_t get_total_field_size(void) const;
      void get_fields(std::vector<FieldID>& fields) const;
      void compute_destroyed_fields(
          std::vector<PhysicalInstance::DestroyedField> &serdez_fields) const;
    public:
      bool match_layout(const LayoutConstraintSet &constraints,
                        unsigned num_dims) const;
      bool match_layout(const LayoutDescription *layout,
                        unsigned num_dims) const;
    public:
      void pack_layout_description(Serializer &rez, AddressSpaceID target);
      static LayoutDescription* handle_unpack_layout_description(
                            LayoutConstraints *constraints,
                            FieldSpaceNode *field_space, size_t total_dims);
    public:
      const FieldMask allocated_fields;
      LayoutConstraints *const constraints;
      FieldSpaceNode *const owner;
      const unsigned total_dims;
    protected:
      // In order by index of bit mask
      std::vector<CopySrcDstField> field_infos;
      // A mapping from FieldIDs to indexes into our field_infos
      std::map<FieldID,unsigned/*index*/> field_indexes;
    protected:
      mutable LocalLock layout_lock; 
      std::map<LEGION_FIELD_MASK_FIELD_TYPE,
               LegionList<std::pair<FieldMask,FieldMask> > > comp_cache;
    }; 

    /**
     * \class CollectiveMapping
     * A collective mapping is an ordering of unique address spaces
     * and can be used to construct broadcast and reduction trees.
     * This is especialy useful for collective instances and for
     * parts of control replication.
     */
    class CollectiveMapping : public Collectable {
    public:
      CollectiveMapping(const std::vector<AddressSpaceID> &spaces,size_t radix);
      CollectiveMapping(const ShardMapping &shard_mapping, size_t radix);
      CollectiveMapping(Deserializer &derez, size_t total_spaces);
      CollectiveMapping(const CollectiveMapping &rhs);
    public:
      inline AddressSpaceID operator[](unsigned idx) const
#ifdef DEBUG_LEGION
        { assert(idx < size()); return unique_sorted_spaces.get_index(idx); }
#else
        { return unique_sorted_spaces.get_index(idx); }
#endif
      inline unsigned find_index(const AddressSpaceID space) const
        { return unique_sorted_spaces.find_index(space); }
      inline const NodeSet& get_unique_spaces(void) const 
        { return unique_sorted_spaces; }
      inline size_t size(void) const { return total_spaces; }
      inline AddressSpaceID get_origin(void) const 
#ifdef DEBUG_LEGION
        { assert(size() > 0); return unique_sorted_spaces.find_first_set(); }
#else
        { return unique_sorted_spaces.find_first_set(); }
#endif
      bool operator==(const CollectiveMapping &rhs) const;
      bool operator!=(const CollectiveMapping &rhs) const;
    public:
      AddressSpaceID get_parent(const AddressSpaceID origin, 
                                const AddressSpaceID local) const;
      size_t count_children(const AddressSpaceID origin,
                            const AddressSpaceID local) const;
      void get_children(const AddressSpaceID origin, const AddressSpaceID local,
                        std::vector<AddressSpaceID> &children) const;
      AddressSpaceID find_nearest(AddressSpaceID start) const;
      inline bool contains(const AddressSpaceID space) const
        { return unique_sorted_spaces.contains(space); }
      bool contains(const CollectiveMapping &rhs) const;
      CollectiveMapping* clone_with(AddressSpace space) const;
      void pack(Serializer &rez) const;
    protected:
      unsigned convert_to_offset(unsigned index, unsigned origin) const;
      unsigned convert_to_index(unsigned offset, unsigned origin) const;
    protected:
      NodeSet unique_sorted_spaces;
      size_t total_spaces;
      size_t radix;
    };

    /**
     * \class InstanceManager
     * This is the abstract base class for all instances of a physical
     * resource manager for memory.
     */
    class InstanceManager : public DistributedCollectable {
    public:
      enum {
        EXTERNAL_CODE = 0x10,
        REDUCTION_CODE = 0x20,
        COLLECTIVE_CODE = 0x40,
      };
    public:
      InstanceManager(RegionTreeForest *forest, AddressSpaceID owner, 
                      DistributedID did, LayoutDescription *layout,
                      FieldSpaceNode *node, IndexSpaceExpression *domain,
                      RegionTreeID tree_id, bool register_now,
                      CollectiveMapping *mapping = NULL);
      virtual ~InstanceManager(void);
    public:
      virtual PointerConstraint 
                     get_pointer_constraint(const DomainPoint &point) const = 0;
      virtual LegionRuntime::Accessor::RegionAccessor<
        LegionRuntime::Accessor::AccessorType::Generic>
          get_accessor(void) const = 0;
      virtual LegionRuntime::Accessor::RegionAccessor<
        LegionRuntime::Accessor::AccessorType::Generic>
          get_field_accessor(FieldID fid) const = 0; 
    public:
      inline bool is_reduction_manager(void) const;
      inline bool is_physical_manager(void) const;
      inline bool is_virtual_manager(void) const;
      inline bool is_external_instance(void) const;
      inline bool is_collective_manager(void) const;
      inline PhysicalManager* as_physical_manager(void) const;
      inline VirtualManager* as_virtual_manager(void) const;
      inline IndividualManager* as_individual_manager(void) const;
      inline CollectiveManager* as_collective_manager(void) const;
    public:
      static inline DistributedID encode_instance_did(DistributedID did,
                        bool external, bool reduction, bool collective);
      static inline bool is_physical_did(DistributedID did);
      static inline bool is_reduction_did(DistributedID did);
      static inline bool is_external_did(DistributedID did);
      static inline bool is_collective_did(DistributedID did);
    public:
      // Interface to the mapper for layouts
      inline void get_fields(std::set<FieldID> &fields) const
        { if (layout != NULL) layout->get_fields(fields); }
      inline bool has_field(FieldID fid) const
        { if (layout != NULL) return layout->has_field(fid); return false; }
      inline void has_fields(std::map<FieldID,bool> &fields) const
        { if (layout != NULL) layout->has_fields(fields); 
          else for (std::map<FieldID,bool>::iterator it = fields.begin();
                    it != fields.end(); it++) it->second = false; } 
      inline void remove_space_fields(std::set<FieldID> &fields) const
        { if (layout != NULL) layout->remove_space_fields(fields);
          else fields.clear(); } 
    public:
      bool entails(LayoutConstraints *constraints, const DomainPoint &key,
                   const LayoutConstraint **failed_constraint) const;
      bool entails(const LayoutConstraintSet &constraints, 
                   const DomainPoint &key,
                   const LayoutConstraint **failed_constraint) const;
      bool conflicts(LayoutConstraints *constraints, const DomainPoint &key,
                     const LayoutConstraint **conflict_constraint) const;
      bool conflicts(const LayoutConstraintSet &constraints,
                     const DomainPoint &key,
                     const LayoutConstraint **conflict_constraint) const;
    public:
      RegionTreeForest *const context;
      LayoutDescription *const layout;
      FieldSpaceNode *const field_space_node;
      IndexSpaceExpression *instance_domain;
      const RegionTreeID tree_id;
    };

    /**
     * \class PhysicalManager 
     * This is an abstract intermediate class for representing an allocation
     * of data; this includes both individual instances and collective instances
     */
    class PhysicalManager : public InstanceManager {
      struct RemoteCreateViewArgs : public LgTaskArgs<RemoteCreateViewArgs> {
      public:
        static const LgTaskID TASK_ID = LG_REMOTE_VIEW_CREATION_TASK_ID;
      public:
        RemoteCreateViewArgs(PhysicalManager *man, InnerContext *ctx, 
                             AddressSpaceID log, CollectiveMapping *map,
                             std::atomic<DistributedID> *tar, 
                             AddressSpaceID src, RtUserEvent done)
          : LgTaskArgs<RemoteCreateViewArgs>(implicit_provenance),
            manager(man), context(ctx), logical_owner(log), mapping(map),
            target(tar), source(src), done_event(done) { }
      public:
        PhysicalManager *const manager;
        InnerContext *const context;
        const AddressSpaceID logical_owner;
        CollectiveMapping *const mapping;
        std::atomic<DistributedID> *const target;
        const AddressSpaceID source;
        const RtUserEvent done_event;
      };
    public:
      enum InstanceKind {
        // Normal Realm allocations
        INTERNAL_INSTANCE_KIND,
        // External allocations imported by attach operations
        EXTERNAL_ATTACHED_INSTANCE_KIND,
        // External allocations from output regions, owned by the runtime
        EXTERNAL_OWNED_INSTANCE_KIND,
        // Allocations drawn from the eager pool
        EAGER_INSTANCE_KIND,
        // Instance not yet bound
        UNBOUND_INSTANCE_KIND,
      };
    public:
      struct GarbageCollectionArgs : public LgTaskArgs<GarbageCollectionArgs> {
      public:
        static const LgTaskID TASK_ID = LG_DEFERRED_COLLECT_ID;
      public:
        GarbageCollectionArgs(CollectableView *v, std::set<ApEvent> *collect)
          : LgTaskArgs<GarbageCollectionArgs>(implicit_provenance), 
            view(v), to_collect(collect) { }
      public:
        CollectableView *const view;
        std::set<ApEvent> *const to_collect;
      };
    public:
      struct CollectableInfo {
      public:
        CollectableInfo(void) : events_added(0) { }
      public:
        std::set<ApEvent> view_events;
        // This event tracks when tracing is completed and it is safe
        // to resume pruning of users from this view
        RtEvent collect_event;
        // Events added since the last collection of view events
        unsigned events_added;
      };
      enum GarbageCollectionState {
        VALID_GC_STATE,
        ACQUIRED_GC_STATE,
        COLLECTABLE_GC_STATE,
        PENDING_COLLECTED_GC_STATE,
        COLLECTED_GC_STATE,
      };
    public:
      // This structure acts as a key for performing rendezvous
      // between collective user registrations
      struct RendezvousKey {
      public:
        RendezvousKey(void)
          : view_did(0), op_context_index(0), index(0) { }
        RendezvousKey(DistributedID did, size_t ctx, unsigned idx)
          : view_did(did), op_context_index(ctx), index(idx) { }
      public:
        inline bool operator<(const RendezvousKey &rhs) const
        {
          if (view_did < rhs.view_did) return true;
          if (view_did > rhs.view_did) return false;
          if (op_context_index < rhs.op_context_index) return true;
          if (op_context_index > rhs.op_context_index) return false;
          return (index < rhs.index);
        }

      public:
        DistributedID view_did; // uniquely names context
        size_t op_context_index; // unique name operation in context
        unsigned index; // uniquely name analysis for op by region req index
      };
    public:
      PhysicalManager(RegionTreeForest *ctx, LayoutDescription *layout, 
                      DistributedID did, AddressSpaceID owner_space, 
                      const size_t footprint, ReductionOpID redop_id, 
                      const ReductionOp *rop, FieldSpaceNode *node,
                      IndexSpaceExpression *index_domain, 
                      const void *piece_list, size_t piece_list_size,
                      RegionTreeID tree_id, bool register_now,
                      bool output_instance = false,
                      CollectiveMapping *mapping = NULL);
      virtual ~PhysicalManager(void); 
    public:
      void log_instance_creation(UniqueID creator_id, Processor proc,
                                 const std::vector<LogicalRegion> &regions,
                                 const DomainPoint &collective_point) const; 
    public: 
      virtual ApEvent get_use_event(ApEvent e = ApEvent::NO_AP_EVENT) const = 0;
      virtual ApEvent get_unique_event(const DomainPoint &point) const = 0;
      virtual PhysicalInstance get_instance(const DomainPoint &point,
                                            bool from_mapper = false) const = 0;
      virtual PointerConstraint 
                     get_pointer_constraint(const DomainPoint &point) const = 0;
      virtual Memory get_memory(const DomainPoint &point, 
                                bool from_mapper = false) const = 0;
    public:
      virtual ApEvent fill_from(FillView *fill_view, InstanceView *dst_view,
                                ApEvent precondition, PredEvent predicate_guard,
                                IndexSpaceExpression *expression,
                                Operation *op, const unsigned index,
                                const FieldMask &fill_mask,
                                const PhysicalTraceInfo &trace_info,
                                std::set<RtEvent> &recorded_events,
                                std::set<RtEvent> &applied_events,
                                CopyAcrossHelper *across_helper,
                                const bool manage_dst_events,
                                const bool fill_restricted,
                                const bool need_valid_return) = 0;
      virtual ApEvent copy_from(InstanceView *src_view, InstanceView *dst_view,
                                PhysicalManager *manager, ApEvent precondition,
                                PredEvent predicate_guard, ReductionOpID redop,
                                IndexSpaceExpression *expression,
                                Operation *op, const unsigned index,
                                const FieldMask &copy_mask,
                                const DomainPoint &src_point,
                                const PhysicalTraceInfo &trace_info,
                                std::set<RtEvent> &recorded_events,
                                std::set<RtEvent> &applied_events,
                                CopyAcrossHelper *across_helper,
                                const bool manage_dst_events,
                                const bool copy_restricted,
                                const bool need_valid_return) = 0;
      virtual void compute_copy_offsets(const FieldMask &copy_mask,
                                std::vector<CopySrcDstField> &fields,
                                const DomainPoint *collective_point = NULL) = 0;
      virtual ApEvent register_collective_user(InstanceView *view, 
                                const RegionUsage &usage,
                                const FieldMask &user_mask,
                                IndexSpaceNode *expr,
                                const UniqueID op_id,
                                const size_t op_ctx_index,
                                const unsigned index,
                                ApEvent term_event,
                                RtEvent collect_event,
                                std::set<RtEvent> &applied_events,
                                const CollectiveMapping *mapping,
                                Operation *local_collective_op,
                                const PhysicalTraceInfo &trace_info,
                                const bool symbolic) = 0;
    public:
      virtual RtEvent find_field_reservations(const FieldMask &mask,
                                DistributedID view_did,const DomainPoint &point,
                                std::vector<Reservation> *reservations,
                                AddressSpaceID source,
                                RtUserEvent to_trigger) = 0;
      virtual void update_field_reservations(const FieldMask &mask,
                                DistributedID view_did,const DomainPoint &point,
                                const std::vector<Reservation> &rsrvs) = 0;
      virtual void reclaim_field_reservations(DistributedID view_did,
                                std::vector<Reservation> &to_delete) = 0;
    public:
      virtual void send_manager(AddressSpaceID target) = 0; 
      static void handle_manager_request(Deserializer &derez, 
                          Runtime *runtime, AddressSpaceID source);
    public:
      virtual void notify_active(ReferenceMutator *mutator);
      virtual void notify_inactive(ReferenceMutator *mutator);
      virtual void notify_valid(ReferenceMutator *mutator);
      virtual void notify_invalid(ReferenceMutator *mutator);
    public:
      bool acquire_instance(ReferenceSource source, ReferenceMutator *mutator);
      bool can_collect(AddressSpaceID source, bool &already_collected);
      bool collect(RtEvent &collected);
      RtEvent set_garbage_collection_priority(MapperID mapper_id, Processor p, 
                                  AddressSpaceID source, GCPriority priority);
      virtual void get_instance_pointers(Memory memory, 
                                    std::vector<uintptr_t> &pointers) const = 0;
      virtual RtEvent perform_deletion(AddressSpaceID source, 
                                       AutoLock *i_lock = NULL) = 0;
      virtual void force_deletion(void) = 0;
      virtual RtEvent update_garbage_collection_priority(AddressSpaceID source,
                                                       GCPriority priority) = 0;
      virtual RtEvent attach_external_instance(void) = 0;
      virtual RtEvent detach_external_instance(void) = 0;
      virtual bool has_visible_from(const std::set<Memory> &memories) const = 0;
      size_t get_instance_size(void) const;
      void update_instance_footprint(size_t footprint)
        { instance_footprint = footprint; }
    public:
      // Methods for creating/finding/destroying logical top views
      InstanceView* find_or_create_instance_top_view(InnerContext *context,
          AddressSpaceID logical_owner, CollectiveMapping *mapping);
      InstanceView* construct_top_view(AddressSpaceID logical_owner,
                                       DistributedID did, UniqueID uid,
                                       CollectiveMapping *mapping);
      void unregister_active_context(InnerContext *context); 
    public:
      PieceIteratorImpl* create_piece_iterator(IndexSpaceNode *privilege_node);
      void defer_collect_user(CollectableView *view, ApEvent term_event,
                              RtEvent collect, std::set<ApEvent> &to_collect, 
                              bool &add_ref, bool &remove_ref);
      void find_shutdown_preconditions(std::set<ApEvent> &preconditions);
    public:
      bool meets_regions(const std::vector<LogicalRegion> &regions,
                         bool tight_region_bounds = false) const;
      bool meets_expression(IndexSpaceExpression *expr, 
                            bool tight_bounds = false) const;
    protected:
      void prune_gc_events(void);
      void pack_garbage_collection_state(Serializer &rez,
                                         AddressSpaceID target, bool need_lock);
      void initialize_remote_gc_state(GarbageCollectionState state);
    public: 
      static ApEvent fetch_metadata(PhysicalInstance inst, ApEvent use_event);
      static void process_top_view_request(PhysicalManager *manager,
          InnerContext *context, AddressSpaceID logical_owner,
          CollectiveMapping *mapping, std::atomic<DistributedID> *target,
          AddressSpaceID source, RtUserEvent done_event, Runtime *runtime);
      static void handle_top_view_request(Deserializer &derez, Runtime *runtime,
                                          AddressSpaceID source);
      static void handle_top_view_response(Deserializer &derez);
      static void handle_top_view_creation(const void *args, Runtime *runtime);
      static void handle_acquire_request(Runtime *runtime,
          Deserializer &derez, AddressSpaceID source);
      static void handle_acquire_response(Deserializer &derez, 
          AddressSpaceID source);
      static void handle_garbage_collection_request(Runtime *runtime,
          Deserializer &derez, AddressSpaceID source);
      static void handle_garbage_collection_response(Deserializer &derez);
      static void handle_garbage_collection_acquire(Runtime *runtime,
          Deserializer &derez);
      static void handle_garbage_collection_failed(Deserializer &derez);
      static void handle_garbage_collection_priority_update(Runtime *runtime,
          Deserializer &derez, AddressSpaceID source);
      static void handle_garbage_collection_debug_request(Runtime *runtime,
          Deserializer &derez, AddressSpaceID source);
      static void handle_garbage_collection_debug_response(Deserializer &derez);
    public:
      static void handle_atomic_reservation_request(Runtime *runtime,
                                                    Deserializer &derez);
      static void handle_atomic_reservation_response(Runtime *runtime,
                                                     Deserializer &derez);
    public:
      size_t instance_footprint;
      const ReductionOp *reduction_op;
      const ReductionOpID redop; 
      const void *const piece_list;
      const size_t piece_list_size;
    protected:
      mutable LocalLock inst_lock;
      std::set<InnerContext*> active_contexts;
      typedef std::pair<ReplicationID,UniqueID> ContextKey;
      typedef std::pair<InstanceView*,unsigned> ViewEntry;
      std::map<ContextKey,ViewEntry> context_views;
      std::map<ReplicationID,RtUserEvent> pending_views;
    protected:
      // Stuff for garbage collection
      GarbageCollectionState gc_state; 
      unsigned pending_changes;
      std::atomic<unsigned> failed_collection_count;
      RtEvent collection_ready;
      RtUserEvent deferred_deletion;
      bool currently_active;
      // Garbage collection priorities
      GCPriority min_gc_priority;
      RtEvent priority_update_done;
      std::map<std::pair<MapperID,Processor>,GCPriority> mapper_gc_priorities;
    private:
      // Events that have to trigger before we can remove our GC reference
      std::map<CollectableView*,CollectableInfo> gc_events;
    };

    /**
     * \class CopyAcrossHelper
     * A small helper class for performing copies between regions
     * from diferrent region trees
     */
    class CopyAcrossHelper {
    public:
      CopyAcrossHelper(const FieldMask &full,
                       const std::vector<unsigned> &src,
                       const std::vector<unsigned> &dst)
        : full_mask(full), src_indexes(src), dst_indexes(dst) { }
    public:
      const FieldMask &full_mask;
      const std::vector<unsigned> &src_indexes;
      const std::vector<unsigned> &dst_indexes;
      std::map<unsigned,unsigned> forward_map;
      std::map<unsigned,unsigned> backward_map;
    public:
      void compute_across_offsets(const FieldMask &src_mask,
                   std::vector<CopySrcDstField> &dst_fields);
      FieldMask convert_src_to_dst(const FieldMask &src_mask);
      FieldMask convert_dst_to_src(const FieldMask &dst_mask);
    public:
      unsigned convert_src_to_dst(unsigned index);
      unsigned convert_dst_to_src(unsigned index);
    public:
      std::vector<CopySrcDstField> offsets; 
      LegionDeque<std::pair<FieldMask,FieldMask> > compressed_cache;
    };

    /**
     * \class IndividualManager 
     * The individual manager class represents a single physical instance
     * that lives in memory in a given location in the system. This is the
     * most common kind of instance that gets made.
     */
    class IndividualManager : public PhysicalManager,
                              public LegionHeapify<IndividualManager> {
    public:
      static const AllocationType alloc_type = INDIVIDUAL_INST_MANAGER_ALLOC;
    public:
      struct DeferIndividualManagerArgs : 
        public LgTaskArgs<DeferIndividualManagerArgs> {
      public:
        static const LgTaskID TASK_ID = LG_DEFER_INDIVIDUAL_MANAGER_TASK_ID;
      public:
        DeferIndividualManagerArgs(DistributedID d, AddressSpaceID own, 
            Memory m, PhysicalInstance i, size_t f, IndexSpaceExpression *lx,
            const PendingRemoteExpression &pending, FieldSpace h, 
            RegionTreeID tid, LayoutConstraintID l, ApEvent use,
            InstanceKind kind, ReductionOpID redop, const void *piece_list,
            size_t piece_list_size, GarbageCollectionState state);
      public:
        const DistributedID did;
        const AddressSpaceID owner;
        const Memory mem;
        const PhysicalInstance inst;
        const size_t footprint;
        const PendingRemoteExpression pending;
        IndexSpaceExpression *local_expr;
        const FieldSpace handle;
        const RegionTreeID tree_id;
        const LayoutConstraintID layout_id;
        const ApEvent use_event;
        const InstanceKind kind;
        const ReductionOpID redop;
        const void *const piece_list;
        const size_t piece_list_size;
        const GarbageCollectionState state;
      };
    public:
      struct DeferDeleteIndividualManager :
        public LgTaskArgs<DeferDeleteIndividualManager> {
      public:
        static const LgTaskID TASK_ID =
          LG_DEFER_DELETE_INDIVIDUAL_MANAGER_TASK_ID;
      public:
        DeferDeleteIndividualManager(IndividualManager *manager_);
      public:
        IndividualManager *manager;
        const RtUserEvent done;
      };
    private:
      struct BroadcastFunctor {
        BroadcastFunctor(Runtime *rt, Serializer &r) : runtime(rt), rez(r) { }
        inline void apply(AddressSpaceID target)
          { runtime->send_manager_update(target, rez); }
        Runtime *runtime;
        Serializer &rez;
      };
    public:
      IndividualManager(RegionTreeForest *ctx, DistributedID did,
                        AddressSpaceID owner_space,
                        MemoryManager *memory, PhysicalInstance inst, 
                        IndexSpaceExpression *instance_domain,
                        const void *piece_list, size_t piece_list_size,
                        FieldSpaceNode *node, RegionTreeID tree_id,
                        LayoutDescription *desc, ReductionOpID redop, 
                        bool register_now, size_t footprint,
                        ApEvent use_event, InstanceKind kind,
                        const ReductionOp *op = NULL,
                        CollectiveMapping *collective_mapping = NULL,
                        ApEvent producer_event = ApEvent::NO_AP_EVENT);
      IndividualManager(const IndividualManager &rhs) = delete;
      virtual ~IndividualManager(void);
    public:
      IndividualManager& operator=(const IndividualManager &rhs) = delete;
    public:
      virtual LegionRuntime::Accessor::RegionAccessor<
        LegionRuntime::Accessor::AccessorType::Generic>
          get_accessor(void) const;
      virtual LegionRuntime::Accessor::RegionAccessor<
        LegionRuntime::Accessor::AccessorType::Generic>
          get_field_accessor(FieldID fid) const;
    public:
      virtual ApEvent get_use_event(ApEvent user = ApEvent::NO_AP_EVENT) const;
      virtual PhysicalInstance get_instance(const DomainPoint &key,
                                            bool from_mapper = false) const 
                                                   { return instance; }
      virtual ApEvent get_unique_event(const DomainPoint &point) const 
        { return unique_event; }
      virtual PointerConstraint
                     get_pointer_constraint(const DomainPoint &key) const;
      virtual Memory get_memory(const DomainPoint &point, 
                                bool from_mapper = false) const
        { return memory_manager->memory; }
      inline Memory get_memory(void) const { return memory_manager->memory; }
    public:
      virtual ApEvent fill_from(FillView *fill_view, InstanceView *dst_view,
                                ApEvent precondition, PredEvent predicate_guard,
                                IndexSpaceExpression *expression,
                                Operation *op, const unsigned index,
                                const FieldMask &fill_mask,
                                const PhysicalTraceInfo &trace_info,
                                std::set<RtEvent> &recorded_events,
                                std::set<RtEvent> &applied_events,
                                CopyAcrossHelper *across_helper,
                                const bool manage_dst_events,
                                const bool fill_restricted,
                                const bool need_valid_return);
      virtual ApEvent copy_from(InstanceView *src_view, InstanceView *dst_view,
                                PhysicalManager *manager, ApEvent precondition,
                                PredEvent predicate_guard, ReductionOpID redop,
                                IndexSpaceExpression *expression,
                                Operation *op, const unsigned index,
                                const FieldMask &copy_mask,
                                const DomainPoint &src_point,
                                const PhysicalTraceInfo &trace_info,
                                std::set<RtEvent> &recorded_events,
                                std::set<RtEvent> &applied_events,
                                CopyAcrossHelper *across_helper,
                                const bool manage_dst_events,
                                const bool copy_restricted,
                                const bool need_valid_return);
      virtual void compute_copy_offsets(const FieldMask &copy_mask,
                                std::vector<CopySrcDstField> &fields,
                                const DomainPoint *collective_point = NULL);
      virtual ApEvent register_collective_user(InstanceView *view, 
                                const RegionUsage &usage,
                                const FieldMask &user_mask,
                                IndexSpaceNode *expr,
                                const UniqueID op_id,
                                const size_t op_ctx_index,
                                const unsigned index,
                                ApEvent term_event,
                                RtEvent collect_event,
                                std::set<RtEvent> &applied_events,
                                const CollectiveMapping *mapping,
                                Operation *local_collective_op,
                                const PhysicalTraceInfo &trace_info,
                                const bool symbolic);
    public:
      virtual RtEvent find_field_reservations(const FieldMask &mask,
                                DistributedID view_did,const DomainPoint &point,
                                std::vector<Reservation> *reservations,
                                AddressSpaceID source,
                                RtUserEvent to_trigger);
      virtual void update_field_reservations(const FieldMask &mask,
                                DistributedID view_did,const DomainPoint &point,
                                const std::vector<Reservation> &rsrvs);
      virtual void reclaim_field_reservations(DistributedID view_did,
                                std::vector<Reservation> &to_delete);
    public:
      void process_collective_user_registration(const DistributedID view_did,
                                            const size_t op_ctx_index,
                                            const unsigned index,
                                            const AddressSpaceID origin,
                                            const CollectiveMapping *mapping,
                                            const PhysicalTraceInfo &trace_info,
                                            ApEvent remote_term_event,
                                            ApUserEvent remote_ready_event,
                                            RtUserEvent remote_registered);
      void initialize_across_helper(CopyAcrossHelper *across_helper,
                                    const FieldMask &mask,
                                    const std::vector<unsigned> &src_indexes,
                                    const std::vector<unsigned> &dst_indexes);
    public:
      virtual void send_manager(AddressSpaceID target);
      static void handle_send_manager(Runtime *runtime, 
                                      AddressSpaceID source,
                                      Deserializer &derez); 
      static void handle_defer_manager(const void *args, Runtime *runtime);
      static void handle_defer_perform_deletion(const void *args,
                                                Runtime *runtime);
      static void create_remote_manager(Runtime *runtime, DistributedID did,
          AddressSpaceID owner_space, Memory mem, PhysicalInstance inst,
          size_t inst_footprint, IndexSpaceExpression *inst_domain,
          const void *piece_list, size_t piece_list_size,
          FieldSpaceNode *space_node, RegionTreeID tree_id,
          LayoutConstraints *constraints, ApEvent use_event,
          InstanceKind kind, ReductionOpID redop, GarbageCollectionState state);
      static void handle_collective_user_registration(Runtime *runtime,
                                                      Deserializer &derez);
    public:
      virtual void get_instance_pointers(Memory memory, 
                                    std::vector<uintptr_t> &pointers) const;
      virtual RtEvent perform_deletion(AddressSpaceID source, 
                                       AutoLock *i_lock = NULL);
      virtual void force_deletion(void);
      virtual RtEvent update_garbage_collection_priority(AddressSpaceID source,
                                                         GCPriority priority);
      virtual RtEvent attach_external_instance(void);
      virtual RtEvent detach_external_instance(void);
      virtual bool has_visible_from(const std::set<Memory> &memories) const;
    public:
      inline bool is_unbound() const 
        { return kind == UNBOUND_INSTANCE_KIND; }
      bool update_physical_instance(PhysicalInstance new_instance,
                                    InstanceKind new_kind,
                                    size_t new_footprint,
                                    uintptr_t new_pointer = 0);
      void broadcast_manager_update(void);
      static void handle_send_manager_update(Runtime *runtime,
                                             AddressSpaceID source,
                                             Deserializer &derez);
      void pack_fields(Serializer &rez, 
                       const std::vector<CopySrcDstField> &fields) const;
    public:
      MemoryManager *const memory_manager;
      // Unique identifier event that is common across nodes
      const ApEvent unique_event;
      PhysicalInstance instance;
      // Event that needs to trigger before we can start using
      // this physical instance.
      ApUserEvent use_event;
      // Event that signifies if the instance name is available
      RtUserEvent instance_ready;
      InstanceKind kind;
      // Keep the pointer for owned external instances
      uintptr_t external_pointer;
      // Completion event of the task that sets a realm instance
      // to this manager. Valid only when the kind is UNBOUND
      // initially, otherwise NO_AP_EVENT.
      const ApEvent producer_event;
    protected:
      std::map<DistributedID,std::map<unsigned,Reservation> > view_reservations;
    protected:
      // This is an infrequently used data structure for handling collective
      // register user calls on individual managers that occurs with certain
      // operation in control replicated contexts
      struct UserRendezvous {
        UserRendezvous(void) 
          : remaining_local_arrivals(0), remaining_remote_arrivals(0),
            view(NULL), mask(NULL), expr(NULL), op_id(0), trace_info(NULL),
            symbolic(false), local_initialized(false) { }
        // event for when local instances can be used
        ApUserEvent ready_event; 
        // remote ready events to trigger
        std::map<ApUserEvent,PhysicalTraceInfo*> remote_ready_events;
        // all the local term events
        std::vector<ApEvent> term_events;
        // event that marks when all registrations are done
        RtUserEvent registered;
        // event for when any local effects are applied
        RtUserEvent applied;
        // Counts of remaining notficiations before registration
        unsigned remaining_local_arrivals;
        unsigned remaining_remote_arrivals;
        // Arguments for performing the local registration
        InstanceView *view;
        RegionUsage usage;
        FieldMask *mask;
        IndexSpaceNode *expr;
        UniqueID op_id;
        RtEvent collect_event;
        PhysicalTraceInfo *trace_info;
        bool symbolic;
        bool local_initialized;
      };
      std::map<RendezvousKey,UserRendezvous> rendezvous_users;
    };

    /**
     * \class CollectiveManager
     * The collective instance manager class supports the interface
     * of a single instance but is actually contains N distributed 
     * copies of the same data and will perform collective operations
     * as part of any reads, writes, or reductions performed to it.
     */
    class CollectiveManager : public PhysicalManager,
              public LegionHeapify<CollectiveManager> {
    public:
      static const AllocationType alloc_type = COLLECTIVE_INST_MANAGER_ALLOC;
    public:
      struct DeferCollectiveManagerArgs : 
        public LgTaskArgs<DeferCollectiveManagerArgs> {
      public:
        static const LgTaskID TASK_ID = LG_DEFER_COLLECTIVE_MANAGER_TASK_ID;
      public:
        DeferCollectiveManagerArgs(DistributedID d, AddressSpaceID own, 
            IndexSpace p, size_t tp, CollectiveMapping *map, size_t f,
            IndexSpaceExpression *lx, const PendingRemoteExpression &pending,
            FieldSpace h, RegionTreeID tid, LayoutConstraintID l,
            ReductionOpID redop, const void *piece_list,size_t piece_list_size,
            const AddressSpaceID source, GarbageCollectionState state,
            bool multi_instace);
      public:
        const DistributedID did;
        const AddressSpaceID owner;
        IndexSpace point_space;
        const size_t total_points;
        CollectiveMapping *const mapping;
        const size_t footprint;
        IndexSpaceExpression *const local_expr;
        const PendingRemoteExpression pending;
        const FieldSpace handle;
        const RegionTreeID tree_id;
        const LayoutConstraintID layout_id;
        const ReductionOpID redop;
        const void *const piece_list;
        const size_t piece_list_size;
        const AddressSpaceID source;
        const GarbageCollectionState state;
        const bool multi_instance;
      };
    protected:
      struct RemoteInstInfo {
        PhysicalInstance instance;
        ApEvent unique_event;
        unsigned index;
      public:
        inline bool operator==(const RemoteInstInfo &rhs) const
        {
          if (instance != rhs.instance) return false;
          if (unique_event != rhs.unique_event) return false;
          if (index != rhs.index) return false;
          return true;
        }
      };
    public:
      CollectiveManager(RegionTreeForest *ctx, DistributedID did,
                        AddressSpaceID owner_space, IndexSpaceNode *point_space,
                        size_t total_pts, CollectiveMapping *mapping,
                        IndexSpaceExpression *instance_domain,
                        const void *piece_list, size_t piece_list_size,
                        FieldSpaceNode *node, RegionTreeID tree_id,
                        LayoutDescription *desc, ReductionOpID redop, 
                        bool register_now, size_t footprint,
                        bool external_instance, bool multi_instance);
      CollectiveManager(const CollectiveManager &rhs) = delete;
      virtual ~CollectiveManager(void);
    public:
      CollectiveManager& operator=(const CollectiveManager &rh) = delete;
    public:
      // These methods can be slow in the case where there is not a point
      // space and the set of points are implicit so only use them for 
      // error checking code
      bool contains_point(const DomainPoint &point) const;
      bool contains_isomorphic_points(IndexSpaceNode *points) const;
    public:
      bool is_first_local_point(const DomainPoint &point) const;
    public:
      void record_point_instance(const DomainPoint &point,
                                 PhysicalInstance instance,
                                 ApEvent ready_event);
      bool finalize_point_instance(const DomainPoint &point,
                                   bool success, bool acquire, 
                                   bool remote = false);
    public:
      virtual ApEvent get_use_event(ApEvent user = ApEvent::NO_AP_EVENT) const;
      virtual ApEvent get_unique_event(const DomainPoint &point) const;
      virtual PhysicalInstance get_instance(const DomainPoint &point, 
                                            bool from_mapper = false) const;
      virtual PointerConstraint
                     get_pointer_constraint(const DomainPoint &key) const; 
      virtual Memory get_memory(const DomainPoint &point,
                                bool from_mapper = false) const
        { return get_instance(point, from_mapper).get_location(); }
    public:
      virtual LegionRuntime::Accessor::RegionAccessor<
        LegionRuntime::Accessor::AccessorType::Generic>
          get_accessor(void) const;
      virtual LegionRuntime::Accessor::RegionAccessor<
        LegionRuntime::Accessor::AccessorType::Generic>
          get_field_accessor(FieldID fid) const;
    public:
      virtual void get_instance_pointers(Memory memory, 
                                    std::vector<uintptr_t> &pointers) const;
      virtual RtEvent perform_deletion(AddressSpaceID source,
                                       AutoLock *i_lock = NULL);
      virtual void force_deletion(void);
      virtual RtEvent update_garbage_collection_priority(AddressSpaceID source,
                                                         GCPriority priority);
      virtual RtEvent attach_external_instance(void);
      virtual RtEvent detach_external_instance(void);
      virtual bool has_visible_from(const std::set<Memory> &memories) const;
    protected:
      void collective_deletion(RtEvent deferred_event);
      void collective_force(void);
      void collective_detach(std::set<RtEvent> &detach_events);
      RtEvent broadcast_point_request(const DomainPoint &point) const;
      void find_or_forward_physical_instance(
            AddressSpaceID source, AddressSpaceID origin,
            std::set<DomainPoint> &points, RtUserEvent to_trigger);
      void record_remote_physical_instances(
            const std::map<DomainPoint,RemoteInstInfo> &instances);
    public:
      virtual ApEvent fill_from(FillView *fill_view, InstanceView *dst_view,
                                ApEvent precondition, PredEvent predicate_guard,
                                IndexSpaceExpression *expression,
                                Operation *op, const unsigned index,
                                const FieldMask &fill_mask,
                                const PhysicalTraceInfo &trace_info,
                                std::set<RtEvent> &recorded_events,
                                std::set<RtEvent> &applied_events,
                                CopyAcrossHelper *across_helper,
                                const bool manage_dst_events,
                                const bool fill_restricted,
                                const bool need_valid_return);
      virtual ApEvent copy_from(InstanceView *src_view, InstanceView *dst_view,
                                PhysicalManager *manager, ApEvent precondition,
                                PredEvent predicate_guard, ReductionOpID redop,
                                IndexSpaceExpression *expression,
                                Operation *op, const unsigned index,
                                const FieldMask &copy_mask,
                                const DomainPoint &src_point,
                                const PhysicalTraceInfo &trace_info,
                                std::set<RtEvent> &recorded_events,
                                std::set<RtEvent> &applied_events,
                                CopyAcrossHelper *across_helper,
                                const bool manage_dst_events,
                                const bool copy_restricted,
                                const bool need_valid_return);
      virtual void compute_copy_offsets(const FieldMask &copy_mask,
                                std::vector<CopySrcDstField> &fields,
                                const DomainPoint *collective_point = NULL);
      virtual ApEvent register_collective_user(InstanceView *view, 
                                const RegionUsage &usage,
                                const FieldMask &user_mask,
                                IndexSpaceNode *expr,
                                const UniqueID op_id,
                                const size_t op_ctx_index,
                                const unsigned index,
                                ApEvent term_event,
                                RtEvent collect_event,
                                std::set<RtEvent> &applied_events,
                                const CollectiveMapping *mapping,
                                Operation *local_collective_op,
                                const PhysicalTraceInfo &trace_info,
                                const bool symbolic);
    public:
      virtual RtEvent find_field_reservations(const FieldMask &mask,
                                DistributedID view_did,const DomainPoint &point,
                                std::vector<Reservation> *reservations,
                                AddressSpaceID source,
                                RtUserEvent to_trigger);
      virtual void update_field_reservations(const FieldMask &mask,
                                DistributedID view_did,const DomainPoint &point,
                                const std::vector<Reservation> &rsrvs);
      virtual void reclaim_field_reservations(DistributedID view_did,
                                std::vector<Reservation> &to_delete);
    public:
      void find_points_in_memory(Memory memory, 
                                 std::vector<DomainPoint> &point) const;
      void find_points_nearest_memory(Memory memory,
                                 std::map<DomainPoint,Memory> &points,
                                 bool bandwidth) const;
      RtEvent find_points_nearest_memory(Memory, AddressSpaceID source, 
                                 std::map<DomainPoint,Memory> *points, 
                                 std::atomic<size_t> *target,
                                 AddressSpaceID origin, size_t best,
                                 bool bandwidth) const;
      void find_nearest_local_points(Memory memory, size_t &best,
                                 std::map<DomainPoint,Memory> &results,
                                 bool bandwidth) const;
    public:
      AddressSpaceID select_source_space(AddressSpaceID destination) const;
      AddressSpaceID select_origin_space(void) const
        { return (collective_mapping->contains(local_space) ? local_space :
                  collective_mapping->find_nearest(local_space)); }
      void register_collective_analysis(DistributedID view_did,
                                        CollectiveCopyFillAnalysis *analysis);
      RtEvent find_collective_analyses(DistributedID view_did,
                                       size_t context_index, unsigned index,
                     const std::vector<CollectiveCopyFillAnalysis*> *&analyses);
      void perform_collective_fill(FillView *fill_view, InstanceView *dst_view,
                                ApEvent precondition, PredEvent predicate_guard,
                                IndexSpaceExpression *expression,
                                Operation *op, const unsigned index,
                                const size_t op_context_index,
                                const FieldMask &fill_mask,
                                const PhysicalTraceInfo &trace_info,
                                std::set<RtEvent> &recorded_events,
                                std::set<RtEvent> &applied_events,
                                ApUserEvent result, AddressSpaceID origin,
                                const bool fill_restricted);
      ApEvent perform_collective_point(InstanceView *src_view,
                                const std::vector<CopySrcDstField> &dst_fields,
                                const std::vector<Reservation> &reservations,
                                ApEvent precondition,
                                PredEvent predicate_guard,
                                IndexSpaceExpression *copy_expresison,
                                Operation *op, const unsigned index,
                                const FieldMask &copy_mask,
                                const FieldMask &dst_mask,
                                const Memory location,
                                const DomainPoint &dst_point,
                                const DomainPoint &src_point,
                                const DistributedID dst_inst_did,
                                const PhysicalTraceInfo &trace_info,
                                std::set<RtEvent> &recorded_events,
                                std::set<RtEvent> &applied_events);
      void perform_collective_pointwise(CollectiveManager *source,
                                InstanceView *src_view,
                                InstanceView *dst_view,
                                ApEvent precondition,
                                PredEvent predicate_guard, 
                                IndexSpaceExpression *copy_expression,
                                Operation *op, const unsigned index,
                                const size_t op_ctx_index,
                                const FieldMask &copy_mask,
                                const DomainPoint &origin_point,
                                const DomainPoint &origin_src_point,
                                const PhysicalTraceInfo &trace_info,
                                std::set<RtEvent> &recorded_events,
                                std::set<RtEvent> &applied_events,
                                ApUserEvent all_done, ApBarrier all_bar,
                                ShardID owner_shard, AddressSpaceID origin,
                                const uint64_t allreduce_tag,
                                const bool copy_restricted);
      void perform_collective_reduction(InstanceView *src_view,
                                const std::vector<CopySrcDstField> &dst_fields,
                                const std::vector<Reservation> &reservations,
                                ApEvent precondition,
                                PredEvent predicate_guard,
                                IndexSpaceExpression *copy_expresison,
                                Operation *op, const unsigned index,
                                const FieldMask &copy_mask,
                                const FieldMask &dst_mask,
                                const DomainPoint &src_point,
                                const DistributedID dst_inst_did,
                                const PhysicalTraceInfo &trace_info,
                                std::set<RtEvent> &recorded_events,
                                std::set<RtEvent> &applied_events,
                                ApUserEvent result, AddressSpaceID origin);
      void perform_collective_broadcast(InstanceView *dst_view,
                                const std::vector<CopySrcDstField> &src_fields,
                                ApEvent precondition,
                                PredEvent predicate_guard,
                                IndexSpaceExpression *copy_expresison,
                                Operation *op, const unsigned index,
                                const size_t op_ctx_index,
                                const FieldMask &copy_mask,
                                const PhysicalTraceInfo &trace_info,
                                std::set<RtEvent> &recorded_events,
                                std::set<RtEvent> &applied_events,
                                ApUserEvent copy_done, ApUserEvent all_done,
                                ApBarrier all_bar, ShardID owner_shard,
                                AddressSpaceID origin,
                                const bool copy_restricted);
      void perform_collective_reducecast(IndividualManager *source,
                                InstanceView *dst_view,
                                const std::vector<CopySrcDstField> &src_fields,
                                ApEvent precondition,
                                PredEvent predicate_guard,
                                IndexSpaceExpression *copy_expresison,
                                Operation *op, const unsigned index,
                                const size_t op_ctx_index,
                                const FieldMask &copy_mask,
                                const PhysicalTraceInfo &trace_info,
                                std::set<RtEvent> &recorded_events,
                                std::set<RtEvent> &applied_events,
                                ApUserEvent copy_done,
                                ApBarrier all_bar, ShardID owner_shard,
                                AddressSpaceID origin,
                                const bool copy_restricted);
      void perform_collective_hourglass(CollectiveManager *source,
                                InstanceView *src_view, InstanceView *dst_view,
                                ApEvent precondition,
                                PredEvent predicate_guard,
                                IndexSpaceExpression *copy_expresison,
                                Operation *op, const unsigned index,
                                const FieldMask &copy_mask,
                                const DomainPoint &src_point,
                                const PhysicalTraceInfo &trace_info,
                                std::set<RtEvent> &recorded_events,
                                std::set<RtEvent> &applied_events,
                                ApUserEvent all_done,
                                AddressSpaceID target,
                                const bool copy_restricted);
      void perform_collective_allreduce(ReductionView *src_view,
                                ApEvent precondition,
                                PredEvent predicate_guard,
                                IndexSpaceExpression *copy_expresison,
                                Operation *op, const unsigned index,
                                const FieldMask &copy_mask,
                                const PhysicalTraceInfo &trace_info,
                       const std::vector<CollectiveCopyFillAnalysis*> *analyses,
                                std::set<RtEvent> &recorded_events,
                                std::set<RtEvent> &applied_events,
                                const uint64_t allreduce_tag);
      // Degenerate case
      ApEvent perform_hammer_reduction(InstanceView *src_view,
                                const std::vector<CopySrcDstField> &dst_fields,
                                const std::vector<Reservation> &reservations,
                                ApEvent precondition,
                                PredEvent predicate_guard,
                                IndexSpaceExpression *copy_expresison,
                                Operation *op, const unsigned index,
                                const FieldMask &copy_mask,
                                const PhysicalTraceInfo &trace_info,
                                std::set<RtEvent> &recorded_events,
                                std::set<RtEvent> &applied_events,
                                AddressSpaceID origin);
    protected:
      void perform_single_allreduce(FillView *fill_view,
                                const uint64_t allreduce_tag,
                                Operation *op, PredEvent predicate_guard,
                                IndexSpaceExpression *copy_expression,
                                const FieldMask &copy_mask,
                                const PhysicalTraceInfo &trace_info,
                                std::set<RtEvent> &applied_events,
                                std::vector<ApEvent> &instance_preconditions,
                    std::vector<std::vector<CopySrcDstField> > &local_fields,
              const std::vector<std::vector<Reservation> > &reservations,
                                std::vector<ApEvent> &local_init_events,
                                std::vector<ApEvent> &local_final_events);
      unsigned perform_multi_allreduce(FillView *fill_view,
                                const uint64_t allreduce_tag,
                                Operation *op, PredEvent predicate_guard,
                                IndexSpaceExpression *copy_expression,
                                const FieldMask &copy_mask,
                                const PhysicalTraceInfo &trace_info,
                    const std::vector<CollectiveCopyFillAnalysis*> *analyses,
                                std::set<RtEvent> &applied_events,
                                std::vector<ApEvent> &instance_preconditions,
                    std::vector<std::vector<CopySrcDstField> > &local_fields,
              const std::vector<std::vector<Reservation> > &reservations,
                                std::vector<ApEvent> &local_init_events,
                                std::vector<ApEvent> &local_final_events);
      void send_allreduce_stage(const uint64_t allreduce_tag, const int stage,
                                const int local_rank, ApEvent src_precondition,
                                PredEvent predicate_guard,
                                IndexSpaceExpression *copy_expression,
                                const PhysicalTraceInfo &trace_info,
                                const std::vector<CopySrcDstField> &src_fields,
                                const AddressSpaceID *targets, size_t total,
                                std::vector<ApEvent> &src_events);
      void receive_allreduce_stage(const uint64_t allreduce_tag,
                                const int stage, Operation *op,
                                ApEvent dst_precondition,
                                PredEvent predicate_guard,
                                IndexSpaceExpression *copy_expression,
                                const PhysicalTraceInfo &trace_info,
                                std::set<RtEvent> &applied_events,
                                const std::vector<CopySrcDstField> &dst_fields,
                                const std::vector<Reservation> &reservations,
                                const int *expected_ranks, size_t total_ranks,
                                std::vector<ApEvent> &dst_events);
      void process_distribute_allreduce(const uint64_t allreduce_tag,
                                const int src_rank, const int stage,
                                std::vector<CopySrcDstField> &src_fields,
                                const ApEvent src_precondition,
                                ApUserEvent src_postcondition,
                                ApBarrier src_barrier, ShardID bar_shard);
      void process_register_user_request(const DistributedID view_did,
                                const size_t op_ctx_index, const unsigned index,
                                const RtEvent registered);
      void process_register_user_response(const DistributedID view_did,
                                const size_t op_ctx_index, const unsigned index,
                                const RtEvent registered);
      void finalize_collective_user(InstanceView *view,
                                const RegionUsage &usage,
                                const FieldMask &user_mask,
                                IndexSpaceNode *expr,
                                const UniqueID op_id,
                                const size_t op_ctx_index,
                                const unsigned index,
                                RtEvent collect_event,
                                RtUserEvent local_registered,
                                RtEvent global_registered,
                                ApUserEvent ready_event,
                                ApEvent term_event,
                                const PhysicalTraceInfo &trace_info,
                                std::vector<CollectiveCopyFillAnalysis*> &ses,
                                const bool symbolic) const;
      inline void set_redop(std::vector<CopySrcDstField> &fields) const
      {
#ifdef DEBUG_LEGION
        assert(redop > 0);
#endif
        for (std::vector<CopySrcDstField>::iterator it =
              fields.begin(); it != fields.end(); it++)
          it->set_redop(redop, true/*fold*/, true/*exclusive*/);
      }
      inline void clear_redop(std::vector<CopySrcDstField> &fields) const 
      {
        for (std::vector<CopySrcDstField>::iterator it =
              fields.begin(); it != fields.end(); it++)
          it->set_redop(0/*redop*/, false/*fold*/);
      }
    public:
      virtual void send_manager(AddressSpaceID target);
    public:
      static void handle_send_manager(Runtime *runtime, 
                                      AddressSpaceID source,
                                      Deserializer &derez);
      static void handle_instance_creation(Runtime *runtime, 
                                           Deserializer &derez);
      static void handle_defer_manager(const void *args, Runtime *runtime);
      static void handle_distribute_fill(Runtime *runtime, 
                                    AddressSpaceID source, Deserializer &derez);
      static void handle_distribute_point(Runtime *runtime,
                                    AddressSpaceID source, Deserializer &derez);
      static void handle_distribute_pointwise(Runtime *runtime,
                                    AddressSpaceID source, Deserializer &derez);
      static void handle_distribute_reduction(Runtime *runtime, 
                                    AddressSpaceID source, Deserializer &derez);
      static void handle_distribute_broadcast(Runtime *runtime, 
                                    AddressSpaceID source, Deserializer &derez);
      static void handle_distribute_reducecast(Runtime *runtime,
                                    AddressSpaceID source, Deserializer &derez);
      static void handle_distribute_hourglass(Runtime *runtime,
                                    AddressSpaceID source, Deserializer &derez);
      static void handle_distribute_allreduce(Runtime *runtime,
                                    AddressSpaceID source, Deserializer &derez);
      static void handle_hammer_reduction(Runtime *runtime, 
                                    AddressSpaceID source, Deserializer &derez);
      static void handle_register_user_request(Runtime *runtime,
                                    Deserializer &derez);
      static void handle_register_user_response(Runtime *runtime,
                                    Deserializer &derez);
      static void handle_point_request(Runtime *runtime, Deserializer &derez);
      static void handle_point_response(Runtime *runtime, Deserializer &derez);
      static void handle_find_points_request(Runtime *runtime,
                                    Deserializer &derez, AddressSpaceID source);
      static void handle_find_points_response(Deserializer &derez);
      static void handle_nearest_points_request(Runtime *runtime,
                                                Deserializer &derez);
      static void handle_nearest_points_response(Deserializer &derez);
      static void handle_remote_registration(Runtime *runtime,
                                             Deserializer &derez);
      static void handle_deletion(Runtime *runtime, Deserializer &derez);
      static void create_collective_manager(Runtime *runtime, DistributedID did,
          AddressSpaceID owner_space, IndexSpaceNode *point_space,
          size_t points, CollectiveMapping *collective_mapping,
          size_t inst_footprint, IndexSpaceExpression *inst_domain,
          const void *piece_list, size_t piece_list_size, 
          FieldSpaceNode *space_node, RegionTreeID tree_id, 
          LayoutConstraints *constraints, ReductionOpID redop, 
          GarbageCollectionState state, bool multi_instance);
      void pack_fields(Serializer &rez, 
                       const std::vector<CopySrcDstField> &fields) const;
      void log_remote_point_instances(
                       const std::vector<CopySrcDstField> &fields,
                       const std::vector<unsigned> &indexes,
                       const std::vector<DomainPoint> &points,
                       const std::vector<ApEvent> &events);
      static void unpack_fields(std::vector<CopySrcDstField> &fields,
          Deserializer &derez, std::set<RtEvent> &ready_events,
          CollectiveManager *manager, RtEvent man_ready, Runtime *runtime);
    public:
      const size_t total_points;
      // This can be NULL if the point set is implicit
      IndexSpaceNode *const point_space;
      static constexpr size_t GUARD_SIZE = std::numeric_limits<size_t>::max();
    protected:
      // Note that there is a collective mapping from DistributedCollectable
      //CollectiveMapping *collective_mapping;
      std::vector<MemoryManager*> memories; // local memories
      std::vector<PhysicalInstance> instances; // local instances
      std::vector<DomainPoint> instance_points; // points for local instances
      std::vector<ApEvent> instance_events; // ready events for each instance 
      std::map<DomainPoint,RemoteInstInfo> remote_points;
    protected:
      struct UserRendezvous {
        UserRendezvous(void) 
          : remaining_local_arrivals(0), remaining_remote_arrivals(0),
            valid_analyses(0), view(NULL), mask(NULL), expr(NULL), op_id(0),
            trace_info(NULL), symbolic(false), local_initialized(false) { }
        // event for when local instances can be used
        ApUserEvent ready_event; 
        // all the local term events
        std::vector<ApEvent> local_term_events;
        // events from remote nodes indicating they are registered
        std::vector<RtEvent> remote_registered;
        // the local set of analyses
        std::vector<CollectiveCopyFillAnalysis*> analyses;
        // event for when the analyses are all registered
        RtUserEvent analyses_ready;
        // event to trigger when local registration is done
        RtUserEvent local_registered; 
        // event that marks when all registrations are done
        RtUserEvent global_registered;
        // Counts of remaining notficiations before registration
        unsigned remaining_local_arrivals;
        unsigned remaining_remote_arrivals;
        unsigned valid_analyses;
        // Arguments for performing the local registration
        InstanceView *view;
        RegionUsage usage;
        FieldMask *mask;
        IndexSpaceNode *expr;
        UniqueID op_id;
        RtEvent collect_event;
        PhysicalTraceInfo *trace_info;
        bool symbolic;
        bool local_initialized;
      };
      std::map<RendezvousKey,UserRendezvous> rendezvous_users;
    protected:
      struct AllReduceCopy {
        std::vector<CopySrcDstField> src_fields;
        ApEvent src_precondition;
        ApUserEvent src_postcondition;
        ApBarrier barrier_postcondition;
        ShardID barrier_shard;
      };
      std::map<std::pair<uint64_t,int>,AllReduceCopy> all_reduce_copies;
      struct AllReduceStage {
        Operation *op;
        IndexSpaceExpression *copy_expression;
        std::vector<CopySrcDstField> dst_fields;
        std::vector<Reservation> reservations;
        PhysicalTraceInfo *trace_info;
        ApEvent dst_precondition;
        PredEvent predicate_guard;
        std::vector<ApUserEvent> remaining_postconditions;
        std::set<RtEvent> applied_events;
        RtUserEvent applied_event;
      };
      std::map<std::pair<uint64_t,int>,AllReduceStage> remaining_stages;
    protected:
      std::map<std::pair<DistributedID,DomainPoint>,
                std::map<unsigned,Reservation> > view_reservations;
    protected:
      std::atomic<uint64_t> unique_allreduce_tag;
    public:
      // A boolean flag that says whether this collective instance
      // has multiple instances on every node. This is primarily
      // useful for reduction instances where we want to pick an
      // algorithm for performing an in-place all-reduce
      const bool multi_instance;
    };

    /**
     * \class VirtualManager
     * This is a singleton class of which there will be exactly one
     * on every node in the machine. The virtual manager class will
     * represent all the virtual instances.
     */
    class VirtualManager : public InstanceManager,
                           public LegionHeapify<VirtualManager> {
    public:
      VirtualManager(Runtime *runtime, DistributedID did, 
                     LayoutDescription *layout);
      VirtualManager(const VirtualManager &rhs);
      virtual ~VirtualManager(void);
    public:
      VirtualManager& operator=(const VirtualManager &rhs);
    public:
      virtual LegionRuntime::Accessor::RegionAccessor<
        LegionRuntime::Accessor::AccessorType::Generic>
          get_accessor(void) const;
      virtual LegionRuntime::Accessor::RegionAccessor<
        LegionRuntime::Accessor::AccessorType::Generic>
          get_field_accessor(FieldID fid) const;
    public:
      virtual void notify_active(ReferenceMutator *mutator);
      virtual void notify_inactive(ReferenceMutator *mutator);
      virtual void notify_valid(ReferenceMutator *mutator);
      virtual void notify_invalid(ReferenceMutator *mutator);
      virtual PointerConstraint 
                     get_pointer_constraint(const DomainPoint &point) const;
      virtual void send_manager(AddressSpaceID target);
    };

    /**
     * \class PendingCollectiveManager
     * This data structure stores the necessary meta-data required
     * for constructing a CollectiveManager by an InstanceBuilder
     * when creating a physical instance for a collective instance
     */
    class PendingCollectiveManager : public Collectable {
    public:
      PendingCollectiveManager(DistributedID did, size_t total_points,
                               IndexSpace point_space,
                               CollectiveMapping *mapping, bool multi_instance);
      PendingCollectiveManager(const PendingCollectiveManager &rhs) = delete;
      ~PendingCollectiveManager(void);
      PendingCollectiveManager& operator=(
          const PendingCollectiveManager&) = delete;
    public:
      const DistributedID did;
      const size_t total_points;
      const IndexSpace point_space;
      CollectiveMapping *const collective_mapping;
      const bool multi_instance;
    public:
      void pack(Serializer &rez) const;
      static PendingCollectiveManager* unpack(Deserializer &derez);
    };

    /**
     * \class InstanceBuilder 
     * A helper for building physical instances of logical regions
     */
    class InstanceBuilder : public ProfilingResponseHandler {
    public:
      InstanceBuilder(const std::vector<LogicalRegion> &regs,
                      const LayoutConstraintSet &cons, Runtime *rt,
                      MemoryManager *memory = NULL, UniqueID cid = 0)
        : regions(regs), constraints(cons), runtime(rt), memory_manager(memory),
          creator_id(cid), instance(PhysicalInstance::NO_INST), 
          field_space_node(NULL), instance_domain(NULL), tree_id(0),
          redop_id(0), reduction_op(NULL), realm_layout(NULL), piece_list(NULL),
          piece_list_size(0), valid(false) { }
      InstanceBuilder(const std::vector<LogicalRegion> &regs,
                      IndexSpaceExpression *expr, FieldSpaceNode *node,
                      RegionTreeID tree_id, const LayoutConstraintSet &cons, 
                      Runtime *rt, MemoryManager *memory, UniqueID cid,
                      const void *piece_list, size_t piece_list_size); 
      virtual ~InstanceBuilder(void);
    public:
      void initialize(RegionTreeForest *forest);
      PhysicalManager* create_physical_instance(RegionTreeForest *forest,
            PendingCollectiveManager *collective, const DomainPoint *point,
            LayoutConstraintKind *unsat_kind,
                        unsigned *unsat_index, size_t *footprint = NULL,
                        RtEvent collection_done = RtEvent::NO_RT_EVENT);
    public:
      virtual void handle_profiling_response(const ProfilingResponseBase *base,
                                      const Realm::ProfilingResponse &response,
                                      const void *orig, size_t orig_length);
    protected:
      void compute_space_and_domain(RegionTreeForest *forest);
    protected:
      void compute_layout_parameters(void);
    protected:
      const std::vector<LogicalRegion> &regions;
      LayoutConstraintSet constraints;
      Runtime *const runtime;
      MemoryManager *const memory_manager;
      const UniqueID creator_id;
    protected:
      PhysicalInstance instance;
      RtUserEvent profiling_ready;
    protected:
      FieldSpaceNode *field_space_node;
      IndexSpaceExpression *instance_domain;
      RegionTreeID tree_id;
      // Mapping from logical field order to layout order
      std::vector<unsigned> mask_index_map;
      std::vector<size_t> field_sizes;
      std::vector<CustomSerdezID> serdez;
      FieldMask instance_mask;
      ReductionOpID redop_id;
      const ReductionOp *reduction_op;
      Realm::InstanceLayoutGeneric *realm_layout;
      void *piece_list;
      size_t piece_list_size;
    public:
      bool valid;
    };

    //--------------------------------------------------------------------------
    /*static*/ inline DistributedID InstanceManager::encode_instance_did(
              DistributedID did, bool external, bool reduction, bool collective)
    //--------------------------------------------------------------------------
    {
      return LEGION_DISTRIBUTED_HELP_ENCODE(did, PHYSICAL_MANAGER_DC | 
                                        (external ? EXTERNAL_CODE : 0) | 
                                        (reduction ? REDUCTION_CODE : 0) |
                                        (collective ? COLLECTIVE_CODE : 0));
    }

    //--------------------------------------------------------------------------
    /*static*/ inline bool InstanceManager::is_physical_did(DistributedID did)
    //--------------------------------------------------------------------------
    {
      return ((LEGION_DISTRIBUTED_HELP_DECODE(did) & 0xF) == 
                                                        PHYSICAL_MANAGER_DC);
    }

    //--------------------------------------------------------------------------
    /*static*/ inline bool InstanceManager::is_reduction_did(DistributedID did)
    //--------------------------------------------------------------------------
    {
      const unsigned decode = LEGION_DISTRIBUTED_HELP_DECODE(did);
      if ((decode & 0xF) != PHYSICAL_MANAGER_DC)
        return false;
      return ((decode & REDUCTION_CODE) != 0);
    }

    //--------------------------------------------------------------------------
    /*static*/ inline bool InstanceManager::is_external_did(DistributedID did)
    //--------------------------------------------------------------------------
    {
      const unsigned decode = LEGION_DISTRIBUTED_HELP_DECODE(did);
      if ((decode & 0xF) != PHYSICAL_MANAGER_DC)
        return false;
      return ((decode & EXTERNAL_CODE) != 0);
    }

    //--------------------------------------------------------------------------
    /*static*/ inline bool InstanceManager::is_collective_did(DistributedID did)
    //--------------------------------------------------------------------------
    {
      const unsigned decode = LEGION_DISTRIBUTED_HELP_DECODE(did);
      if ((decode & 0xF) != PHYSICAL_MANAGER_DC)
        return false;
      return ((decode & COLLECTIVE_CODE) != 0);
    }

    //--------------------------------------------------------------------------
    inline bool InstanceManager::is_reduction_manager(void) const
    //--------------------------------------------------------------------------
    {
      return is_reduction_did(did);
    }

    //--------------------------------------------------------------------------
    inline bool InstanceManager::is_physical_manager(void) const
    //--------------------------------------------------------------------------
    {
      return is_physical_did(did);
    }

    //--------------------------------------------------------------------------
    inline bool InstanceManager::is_virtual_manager(void) const
    //--------------------------------------------------------------------------
    {
      return (did == 0);
    }

    //--------------------------------------------------------------------------
    inline bool InstanceManager::is_external_instance(void) const
    //--------------------------------------------------------------------------
    {
      return is_external_did(did);
    }

    //--------------------------------------------------------------------------
    inline bool InstanceManager::is_collective_manager(void) const
    //--------------------------------------------------------------------------
    {
      return is_collective_did(did);
    }

    //--------------------------------------------------------------------------
    inline PhysicalManager* InstanceManager::as_physical_manager(void) const
    //--------------------------------------------------------------------------
    {
#ifdef DEBUG_LEGION
      assert(is_physical_manager());
#endif
      return static_cast<PhysicalManager*>(const_cast<InstanceManager*>(this));
    }

    //--------------------------------------------------------------------------
    inline VirtualManager* InstanceManager::as_virtual_manager(void) const
    //--------------------------------------------------------------------------
    {
#ifdef DEBUG_LEGION
      assert(is_virtual_manager());
#endif
      return static_cast<VirtualManager*>(const_cast<InstanceManager*>(this));
    }

    //--------------------------------------------------------------------------
    inline IndividualManager* InstanceManager::as_individual_manager(void) const
    //--------------------------------------------------------------------------
    {
#ifdef DEBUG_LEGION
      assert(!is_collective_manager());
#endif
      return 
        static_cast<IndividualManager*>(const_cast<InstanceManager*>(this));
    }

    //--------------------------------------------------------------------------
    inline CollectiveManager* InstanceManager::as_collective_manager(void) const
    //--------------------------------------------------------------------------
    {
#ifdef DEBUG_LEGION
      assert(is_collective_manager());
#endif
      return 
        static_cast<CollectiveManager*>(const_cast<InstanceManager*>(this));
    }

  }; // namespace Internal 
}; // namespace Legion

#endif // __LEGION_INSTANCES_H__
