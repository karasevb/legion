/* Copyright 2018 Stanford University, NVIDIA Corporation
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

#include "legion.h"
#include "legion/runtime.h"
#include "legion/legion_ops.h"
#include "legion/legion_tasks.h"
#include "legion/region_tree.h"
#include "legion/legion_spy.h"
#include "legion/legion_trace.h"
#include "legion/legion_profiling.h"
#include "legion/legion_instances.h"
#include "legion/legion_views.h"
#include "legion/legion_analysis.h"
#include "legion/legion_context.h"

namespace Legion {
  namespace Internal {

    LEGION_EXTERN_LOGGER_DECLARATIONS

    /////////////////////////////////////////////////////////////
    // Users and Info 
    /////////////////////////////////////////////////////////////

    //--------------------------------------------------------------------------
    LogicalUser::LogicalUser(void)
      : GenericUser(), op(NULL), idx(0), gen(0), timeout(TIMEOUT)
#ifdef LEGION_SPY
        , uid(0)
#endif
    //--------------------------------------------------------------------------
    {
    }

    //--------------------------------------------------------------------------
    LogicalUser::LogicalUser(Operation *o, unsigned id, const RegionUsage &u,
                             const FieldMask &m)
      : GenericUser(u, m), op(o), idx(id), 
        gen(o->get_generation()), timeout(TIMEOUT)
#ifdef LEGION_SPY
        , uid(o->get_unique_op_id())
#endif
    //--------------------------------------------------------------------------
    {
    }

    //--------------------------------------------------------------------------
    LogicalUser::LogicalUser(Operation *o, GenerationID g, unsigned id, 
                             const RegionUsage &u, const FieldMask &m)
      : GenericUser(u, m), op(o), idx(id), gen(g), timeout(TIMEOUT)
#ifdef LEGION_SPY
        , uid(o->get_unique_op_id())
#endif
    //--------------------------------------------------------------------------
    {
    }

    //--------------------------------------------------------------------------
    PhysicalUser::PhysicalUser(IndexSpaceExpression *e)
      : expr(e), expr_volume(expr->get_volume())
    //--------------------------------------------------------------------------
    {
#ifdef DEBUG_LEGION
      assert(expr != NULL);
#endif
      expr->add_expression_reference();
    }
    
    //--------------------------------------------------------------------------
    PhysicalUser::PhysicalUser(const RegionUsage &u, IndexSpaceExpression *e,
                               UniqueID id, unsigned x)
      : usage(u), expr(e), expr_volume(expr->get_volume()), op_id(id), index(x)
    //--------------------------------------------------------------------------
    {
#ifdef DEBUG_LEGION
      assert(expr != NULL);
#endif
      expr->add_expression_reference();
    }

    //--------------------------------------------------------------------------
    PhysicalUser::PhysicalUser(const PhysicalUser &rhs) 
      : expr(NULL), expr_volume(0)
    //--------------------------------------------------------------------------
    {
      // should never be called
      assert(false);
    }

    //--------------------------------------------------------------------------
    PhysicalUser::~PhysicalUser(void)
    //--------------------------------------------------------------------------
    {
#ifdef DEBUG_LEGION
      assert(expr != NULL);
#endif
      if (expr->remove_expression_reference())
        delete expr;
    }

    //--------------------------------------------------------------------------
    PhysicalUser& PhysicalUser::operator=(const PhysicalUser &rhs)
    //--------------------------------------------------------------------------
    {
      // should never be called
      assert(false);
      return *this;
    }

    //--------------------------------------------------------------------------
    void PhysicalUser::pack_user(Serializer &rez, AddressSpaceID target)
    //--------------------------------------------------------------------------
    {
      expr->pack_expression(rez, target);
      rez.serialize(usage.privilege);
      rez.serialize(usage.prop);
      rez.serialize(usage.redop);
      rez.serialize(op_id);
      rez.serialize(index);
    }

    //--------------------------------------------------------------------------
    /*static*/ PhysicalUser* PhysicalUser::unpack_user(Deserializer &derez,
        bool add_reference, RegionTreeForest *forest, AddressSpaceID source)
    //--------------------------------------------------------------------------
    {
      IndexSpaceExpression *expr = 
          IndexSpaceExpression::unpack_expression(derez, forest, source);
#ifdef DEBUG_LEGION
      assert(expr != NULL);
#endif
      PhysicalUser *result = new PhysicalUser(expr);
      derez.deserialize(result->usage.privilege);
      derez.deserialize(result->usage.prop);
      derez.deserialize(result->usage.redop);
      derez.deserialize(result->op_id);
      derez.deserialize(result->index);
      if (add_reference)
        result->add_reference();
      return result;
    } 

    /////////////////////////////////////////////////////////////
    // VersionInfo 
    /////////////////////////////////////////////////////////////

    //--------------------------------------------------------------------------
    VersionInfo::VersionInfo(void)
    //--------------------------------------------------------------------------
    {
    }

    //--------------------------------------------------------------------------
    VersionInfo::VersionInfo(const VersionInfo &rhs)
    //--------------------------------------------------------------------------
    {
#ifdef DEBUG_LEGION
      assert(equivalence_sets.empty());
      assert(rhs.equivalence_sets.empty());
#endif
    }

    //--------------------------------------------------------------------------
    VersionInfo::~VersionInfo(void)
    //--------------------------------------------------------------------------
    {
#ifdef DEBUG_LEGION
      assert(!mapped_event.exists());
      assert(equivalence_sets.empty());
#endif
    }

    //--------------------------------------------------------------------------
    VersionInfo& VersionInfo::operator=(const VersionInfo &rhs)
    //--------------------------------------------------------------------------
    {
#ifdef DEBUG_LEGION
      assert(equivalence_sets.empty());
      assert(rhs.equivalence_sets.empty());
#endif
      return *this;
    }

    //--------------------------------------------------------------------------
    void VersionInfo::initialize_mapping(RtEvent mapped)
    //--------------------------------------------------------------------------
    {
#ifdef DEBUG_LEGION
      assert(!mapped_event.exists());
#endif
      mapped_event = mapped;
    }

    //--------------------------------------------------------------------------
    void VersionInfo::record_equivalence_set(EquivalenceSet *set)
    //--------------------------------------------------------------------------
    {
#ifdef DEBUG_LEGION 
      assert(mapped_event.exists());
#endif
      std::pair<std::set<EquivalenceSet*>::iterator,bool> result = 
        equivalence_sets.insert(set);
      // If we added this element then need to add a mapping guard
      if (result.second)
        set->add_base_resource_ref(VERSION_INFO_REF);
    }

    //--------------------------------------------------------------------------
    void VersionInfo::make_ready(const RegionRequirement &req, 
                                 const FieldMask &ready_mask,
                                 std::set<RtEvent> &ready_events,
                                 std::set<RtEvent> &applied_events)
    //--------------------------------------------------------------------------
    {
#ifdef DEBUG_LEGION
      assert(mapped_event.exists());
#endif
      // We only need an exclusive mode for this operation if we're 
      // writing otherwise, we know we can do things with a shared copy
      for (std::set<EquivalenceSet*>::const_iterator it = 
            equivalence_sets.begin(); it != equivalence_sets.end(); it++)
        (*it)->request_valid_copy(ready_mask, RegionUsage(req), 
                                  ready_events, applied_events,
                                  (*it)->local_space);
    }

    //--------------------------------------------------------------------------
    void VersionInfo::finalize_mapping(void)
    //--------------------------------------------------------------------------
    {
#ifdef DEBUG_LEGION
      assert(mapped_event.exists());
#endif
      if (!equivalence_sets.empty())
      {
        for (std::set<EquivalenceSet*>::const_iterator it = 
              equivalence_sets.begin(); it != equivalence_sets.end(); it++)
        {
          (*it)->remove_mapping_guard(mapped_event);
          if ((*it)->remove_base_resource_ref(VERSION_INFO_REF))
            delete (*it);
        }
        equivalence_sets.clear();
      }
      mapped_event = RtEvent::NO_RT_EVENT;
    }

    /////////////////////////////////////////////////////////////
    // LogicalTraceInfo 
    /////////////////////////////////////////////////////////////

    //--------------------------------------------------------------------------
    LogicalTraceInfo::LogicalTraceInfo(bool already_tr, LegionTrace *tr, 
                                       unsigned idx, const RegionRequirement &r)
      : already_traced(already_tr), trace(tr), req_idx(idx), req(r)
    //--------------------------------------------------------------------------
    {
      // If we have a trace but it doesn't handle the region tree then
      // we should mark that this is not part of a trace
      if ((trace != NULL) && 
          !trace->handles_region_tree(req.parent.get_tree_id()))
      {
        already_traced = false;
        trace = NULL;
      }
    }

    /////////////////////////////////////////////////////////////
    // PhysicalTraceInfo
    /////////////////////////////////////////////////////////////

    //--------------------------------------------------------------------------
    PhysicalTraceInfo::PhysicalTraceInfo(Operation *o, bool initialize)
    //--------------------------------------------------------------------------
      : op(o), tpl((op == NULL) ? NULL : 
          (op->get_memoizable() == NULL) ? NULL :
            op->get_memoizable()->get_template()),
        recording((tpl == NULL) ? false : tpl->is_recording())
    {
      if (recording && initialize)
        tpl->record_get_term_event(op->get_memoizable());
    }

    //--------------------------------------------------------------------------
    PhysicalTraceInfo::PhysicalTraceInfo(Operation *o, Memoizable *memo)
      : op(o), tpl(memo->get_template()),
        recording((tpl == NULL) ? false : tpl->is_recording())
    //--------------------------------------------------------------------------
    {
    }

    //--------------------------------------------------------------------------
    void PhysicalTraceInfo::record_merge_events(ApEvent &result,
                                                ApEvent e1, ApEvent e2) const
    //--------------------------------------------------------------------------
    {
#ifdef DEBUG_LEGION
      assert(recording);
      assert(tpl != NULL);
      assert(tpl->is_recording());
#endif
      tpl->record_merge_events(result, e1, e2, op);      
    }
    
    //--------------------------------------------------------------------------
    void PhysicalTraceInfo::record_merge_events(ApEvent &result,
                                       ApEvent e1, ApEvent e2, ApEvent e3) const
    //--------------------------------------------------------------------------
    {
#ifdef DEBUG_LEGION
      assert(recording);
      assert(tpl != NULL);
      assert(tpl->is_recording());
#endif
      tpl->record_merge_events(result, e1, e2, e3, op);
    }

    //--------------------------------------------------------------------------
    void PhysicalTraceInfo::record_merge_events(ApEvent &result,
                                          const std::set<ApEvent> &events) const
    //--------------------------------------------------------------------------
    {
#ifdef DEBUG_LEGION
      assert(recording);
      assert(tpl != NULL);
      assert(tpl->is_recording());
#endif
      tpl->record_merge_events(result, events, op);
    }

    //--------------------------------------------------------------------------
    void PhysicalTraceInfo::record_op_sync_event(ApEvent &result) const
    //--------------------------------------------------------------------------
    {
#ifdef DEBUG_LEGION
      assert(recording);
      assert(tpl != NULL);
      assert(tpl->is_recording());
#endif
      tpl->record_set_op_sync_event(result, op);
    }

    //--------------------------------------------------------------------------
    void PhysicalTraceInfo::record_issue_copy(ApEvent &result,
                                 IndexSpaceExpression *expr,
                                 const std::vector<CopySrcDstField>& src_fields,
                                 const std::vector<CopySrcDstField>& dst_fields,
                                 ApEvent precondition,
                                 ReductionOpID redop,
                                 bool reduction_fold) const
    //--------------------------------------------------------------------------
    {
#ifdef DEBUG_LEGION
      assert(recording);
      assert(tpl != NULL);
      assert(tpl->is_recording());
#endif
      tpl->record_issue_copy(op, result, expr, src_fields, dst_fields,
                             precondition, redop, reduction_fold);
    }

    //--------------------------------------------------------------------------
    void PhysicalTraceInfo::record_issue_fill(ApEvent &result,
                                     IndexSpaceExpression *expr,
                                     const std::vector<CopySrcDstField> &fields,
                                     const void *fill_value, size_t fill_size,
#ifdef LEGION_SPY
                                     UniqueID fill_uid,
#endif
                                     ApEvent precondition) const
    //--------------------------------------------------------------------------
    {
#ifdef DEBUG_LEGION
      assert(recording);
      assert(tpl != NULL);
      assert(tpl->is_recording());
#endif
      tpl->record_issue_fill(op, result, expr, fields, fill_value, fill_size,
#ifdef LEGION_SPY
                             fill_uid,
#endif
                             precondition);
    }

    //--------------------------------------------------------------------------
    void PhysicalTraceInfo::record_empty_copy(DeferredView *view,
                                              const FieldMask &copy_mask,
                                              MaterializedView *dst) const
    //--------------------------------------------------------------------------
    {
#ifdef DEBUG_LEGION
      assert(recording);
      assert(tpl != NULL);
      assert(tpl->is_recording());
#endif
      tpl->record_empty_copy(view, copy_mask, dst);
    }

    /////////////////////////////////////////////////////////////
    // ProjectionInfo 
    /////////////////////////////////////////////////////////////

    //--------------------------------------------------------------------------
    ProjectionInfo::ProjectionInfo(Runtime *runtime, 
                      const RegionRequirement &req, IndexSpace launch_space)
      : projection((req.handle_type != SINGULAR) ? 
          runtime->find_projection_function(req.projection) : NULL),
        projection_type(req.handle_type),
        projection_space((req.handle_type != SINGULAR) ?
            runtime->forest->get_node(launch_space) : NULL)
    //--------------------------------------------------------------------------
    {
    }

    /////////////////////////////////////////////////////////////
    // PathTraverser 
    /////////////////////////////////////////////////////////////

    //--------------------------------------------------------------------------
    PathTraverser::PathTraverser(RegionTreePath &p)
      : path(p)
    //--------------------------------------------------------------------------
    {
    }

    //--------------------------------------------------------------------------
    PathTraverser::PathTraverser(const PathTraverser &rhs)
      : path(rhs.path)
    //--------------------------------------------------------------------------
    {
      // should never be called
      assert(false);
    }

    //--------------------------------------------------------------------------
    PathTraverser::~PathTraverser(void)
    //--------------------------------------------------------------------------
    {
    }

    //--------------------------------------------------------------------------
    PathTraverser& PathTraverser::operator=(const PathTraverser &rhs)
    //--------------------------------------------------------------------------
    {
      // should never be called
      assert(false);
      return *this;
    }

    //--------------------------------------------------------------------------
    bool PathTraverser::traverse(RegionTreeNode *node)
    //--------------------------------------------------------------------------
    {
      // Continue visiting nodes and then finding their children
      // until we have traversed the entire path.
      while (true)
      {
#ifdef DEBUG_LEGION
        assert(node != NULL);
#endif
        depth = node->get_depth();
        has_child = path.has_child(depth);
        if (has_child)
          next_child = path.get_child(depth);
        bool continue_traversal = node->visit_node(this);
        if (!continue_traversal)
          return false;
        if (!has_child)
          break;
        node = node->get_tree_child(next_child);
      }
      return true;
    }

    /////////////////////////////////////////////////////////////
    // LogicalPathRegistrar
    /////////////////////////////////////////////////////////////

    //--------------------------------------------------------------------------
    LogicalPathRegistrar::LogicalPathRegistrar(ContextID c, Operation *o,
                                       const FieldMask &m, RegionTreePath &p)
      : PathTraverser(p), ctx(c), field_mask(m), op(o)
    //--------------------------------------------------------------------------
    {
    }

    //--------------------------------------------------------------------------
    LogicalPathRegistrar::LogicalPathRegistrar(const LogicalPathRegistrar&rhs)
      : PathTraverser(rhs.path), ctx(0), field_mask(FieldMask()), op(NULL)
    //--------------------------------------------------------------------------
    {
      // should never be called
      assert(false);
    }

    //--------------------------------------------------------------------------
    LogicalPathRegistrar::~LogicalPathRegistrar(void)
    //--------------------------------------------------------------------------
    {
    }

    //--------------------------------------------------------------------------
    LogicalPathRegistrar& LogicalPathRegistrar::operator=(
                                                const LogicalPathRegistrar &rhs)
    //--------------------------------------------------------------------------
    {
      // should never be called
      assert(false);
      return *this;
    }

    //--------------------------------------------------------------------------
    bool LogicalPathRegistrar::visit_region(RegionNode *node)
    //--------------------------------------------------------------------------
    {
      node->register_logical_dependences(ctx, op, field_mask,false/*dominate*/);
      if (!has_child)
      {
        // If we're at the bottom, fan out and do all the children
        LogicalRegistrar registrar(ctx, op, field_mask, false);
        return node->visit_node(&registrar);
      }
      return true;
    }

    //--------------------------------------------------------------------------
    bool LogicalPathRegistrar::visit_partition(PartitionNode *node)
    //--------------------------------------------------------------------------
    {
      node->register_logical_dependences(ctx, op, field_mask,false/*dominate*/);
      if (!has_child)
      {
        // If we're at the bottom, fan out and do all the children
        LogicalRegistrar registrar(ctx, op, field_mask, false);
        return node->visit_node(&registrar);
      }
      return true;
    }


    /////////////////////////////////////////////////////////////
    // LogicalRegistrar
    /////////////////////////////////////////////////////////////
    
    //--------------------------------------------------------------------------
    LogicalRegistrar::LogicalRegistrar(ContextID c, Operation *o,
                                       const FieldMask &m, bool dom)
      : ctx(c), field_mask(m), op(o), dominate(dom)
    //--------------------------------------------------------------------------
    {
    }

    //--------------------------------------------------------------------------
    LogicalRegistrar::LogicalRegistrar(const LogicalRegistrar &rhs)
      : ctx(0), field_mask(FieldMask()), op(NULL), dominate(rhs.dominate)
    //--------------------------------------------------------------------------
    {
      // should never be called
      assert(false);
    }

    //--------------------------------------------------------------------------
    LogicalRegistrar::~LogicalRegistrar(void)
    //--------------------------------------------------------------------------
    {
    }

    //--------------------------------------------------------------------------
    LogicalRegistrar& LogicalRegistrar::operator=(const LogicalRegistrar &rhs)
    //--------------------------------------------------------------------------
    {
      // should never be called
      assert(false);
      return *this;
    }

    //--------------------------------------------------------------------------
    bool LogicalRegistrar::visit_only_valid(void) const
    //--------------------------------------------------------------------------
    {
      return false;
    }

    //--------------------------------------------------------------------------
    bool LogicalRegistrar::visit_region(RegionNode *node)
    //--------------------------------------------------------------------------
    {
      node->register_logical_dependences(ctx, op, field_mask, dominate);
      return true;
    }

    //--------------------------------------------------------------------------
    bool LogicalRegistrar::visit_partition(PartitionNode *node)
    //--------------------------------------------------------------------------
    {
      node->register_logical_dependences(ctx, op, field_mask, dominate);
      return true;
    }

    /////////////////////////////////////////////////////////////
    // CurrentInitializer 
    /////////////////////////////////////////////////////////////

    //--------------------------------------------------------------------------
    CurrentInitializer::CurrentInitializer(ContextID c)
      : ctx(c)
    //--------------------------------------------------------------------------
    {
    }

    //--------------------------------------------------------------------------
    CurrentInitializer::CurrentInitializer(const CurrentInitializer &rhs)
      : ctx(0)
    //--------------------------------------------------------------------------
    {
      // should never be called
      assert(false);
    }

    //--------------------------------------------------------------------------
    CurrentInitializer::~CurrentInitializer(void)
    //--------------------------------------------------------------------------
    {
    }

    //--------------------------------------------------------------------------
    CurrentInitializer& CurrentInitializer::operator=(
                                                  const CurrentInitializer &rhs)
    //--------------------------------------------------------------------------
    {
      // should never be called
      assert(false);
      return *this;
    }

    //--------------------------------------------------------------------------
    bool CurrentInitializer::visit_only_valid(void) const
    //--------------------------------------------------------------------------
    {
      return false;
    }

    //--------------------------------------------------------------------------
    bool CurrentInitializer::visit_region(RegionNode *node)
    //--------------------------------------------------------------------------
    {
      node->initialize_current_state(ctx); 
      return true;
    }

    //--------------------------------------------------------------------------
    bool CurrentInitializer::visit_partition(PartitionNode *node)
    //--------------------------------------------------------------------------
    {
      node->initialize_current_state(ctx);
      return true;
    }

    /////////////////////////////////////////////////////////////
    // CurrentInvalidator
    /////////////////////////////////////////////////////////////

    //--------------------------------------------------------------------------
    CurrentInvalidator::CurrentInvalidator(ContextID c, bool only)
      : ctx(c), users_only(only)
    //--------------------------------------------------------------------------
    {
    }

    //--------------------------------------------------------------------------
    CurrentInvalidator::CurrentInvalidator(const CurrentInvalidator &rhs)
      : ctx(0), users_only(false)
    //--------------------------------------------------------------------------
    {
      // should never be called
      assert(false);
    }

    //--------------------------------------------------------------------------
    CurrentInvalidator::~CurrentInvalidator(void)
    //--------------------------------------------------------------------------
    {
    }

    //--------------------------------------------------------------------------
    CurrentInvalidator& CurrentInvalidator::operator=(
                                                  const CurrentInvalidator &rhs)
    //--------------------------------------------------------------------------
    {
      // should never be called
      assert(false);
      return *this;
    }

    //--------------------------------------------------------------------------
    bool CurrentInvalidator::visit_only_valid(void) const
    //--------------------------------------------------------------------------
    {
      return false;
    }

    //--------------------------------------------------------------------------
    bool CurrentInvalidator::visit_region(RegionNode *node)
    //--------------------------------------------------------------------------
    {
      node->invalidate_current_state(ctx, users_only); 
      return true;
    }

    //--------------------------------------------------------------------------
    bool CurrentInvalidator::visit_partition(PartitionNode *node)
    //--------------------------------------------------------------------------
    {
      node->invalidate_current_state(ctx, users_only);
      return true;
    }

    /////////////////////////////////////////////////////////////
    // DeletionInvalidator 
    /////////////////////////////////////////////////////////////

    //--------------------------------------------------------------------------
    DeletionInvalidator::DeletionInvalidator(ContextID c, const FieldMask &dm)
      : ctx(c), deletion_mask(dm)
    //--------------------------------------------------------------------------
    {
    }

    //--------------------------------------------------------------------------
    DeletionInvalidator::DeletionInvalidator(const DeletionInvalidator &rhs)
      : ctx(0), deletion_mask(rhs.deletion_mask)
    //--------------------------------------------------------------------------
    {
      // should never be called
      assert(false);
    }

    //--------------------------------------------------------------------------
    DeletionInvalidator::~DeletionInvalidator(void)
    //--------------------------------------------------------------------------
    {
    }

    //--------------------------------------------------------------------------
    DeletionInvalidator& DeletionInvalidator::operator=(
                                                 const DeletionInvalidator &rhs)
    //--------------------------------------------------------------------------
    {
      // should never be called
      assert(false);
      return *this;
    }

    //--------------------------------------------------------------------------
    bool DeletionInvalidator::visit_only_valid(void) const
    //--------------------------------------------------------------------------
    {
      return false;
    }

    //--------------------------------------------------------------------------
    bool DeletionInvalidator::visit_region(RegionNode *node)
    //--------------------------------------------------------------------------
    {
      node->invalidate_deleted_state(ctx, deletion_mask); 
      return true;
    }

    //--------------------------------------------------------------------------
    bool DeletionInvalidator::visit_partition(PartitionNode *node)
    //--------------------------------------------------------------------------
    {
      node->invalidate_deleted_state(ctx, deletion_mask);
      return true;
    }

    /////////////////////////////////////////////////////////////
    // Projection Epoch
    /////////////////////////////////////////////////////////////

    // C++ is really dumb
    const ProjectionEpochID ProjectionEpoch::first_epoch;

    //--------------------------------------------------------------------------
    ProjectionEpoch::ProjectionEpoch(ProjectionEpochID id, const FieldMask &m)
      : epoch_id(id), valid_fields(m)
    //--------------------------------------------------------------------------
    {
    }

    //--------------------------------------------------------------------------
    ProjectionEpoch::ProjectionEpoch(const ProjectionEpoch &rhs)
      : epoch_id(rhs.epoch_id), valid_fields(rhs.valid_fields)
    //--------------------------------------------------------------------------
    {
      // should never be called
      assert(false);
    }

    //--------------------------------------------------------------------------
    ProjectionEpoch::~ProjectionEpoch(void)
    //--------------------------------------------------------------------------
    {
    }

    //--------------------------------------------------------------------------
    ProjectionEpoch& ProjectionEpoch::operator=(const ProjectionEpoch &rhs)
    //--------------------------------------------------------------------------
    {
      // should never be called
      assert(false);
      return *this;
    }

    //--------------------------------------------------------------------------
    void ProjectionEpoch::insert(ProjectionFunction *function, 
                                 IndexSpaceNode* node)
    //--------------------------------------------------------------------------
    {
#ifdef DEBUG_LEGION
      assert(!!valid_fields);
#endif
      write_projections[function].insert(node);
    }

    /////////////////////////////////////////////////////////////
    // LogicalState 
    ///////////////////////////////////////////////////////////// 

    //--------------------------------------------------------------------------
    LogicalState::LogicalState(RegionTreeNode *node, ContextID ctx)
      : owner(node)
    //--------------------------------------------------------------------------
    {
    }

    //--------------------------------------------------------------------------
    LogicalState::LogicalState(const LogicalState &rhs)
      : owner(NULL)
    //--------------------------------------------------------------------------
    {
      // should never be called
      assert(false);
    }

    //--------------------------------------------------------------------------
    LogicalState::~LogicalState(void)
    //--------------------------------------------------------------------------
    {
    }

    //--------------------------------------------------------------------------
    LogicalState& LogicalState::operator=(const LogicalState&rhs)
    //--------------------------------------------------------------------------
    {
      // should never be called
      assert(false);
      return *this;
    }

    //--------------------------------------------------------------------------
    void LogicalState::check_init(void)
    //--------------------------------------------------------------------------
    {
#ifdef DEBUG_LEGION
      assert(field_states.empty());
      assert(curr_epoch_users.empty());
      assert(prev_epoch_users.empty());
      assert(projection_epochs.empty());
      assert(!reduction_fields);
#endif
    }

    //--------------------------------------------------------------------------
    void LogicalState::clear_logical_users(void)
    //--------------------------------------------------------------------------
    {
      if (!curr_epoch_users.empty())
      {
        for (LegionList<LogicalUser,CURR_LOGICAL_ALLOC>::track_aligned::
              const_iterator it = curr_epoch_users.begin(); it != 
              curr_epoch_users.end(); it++)
        {
          it->op->remove_mapping_reference(it->gen); 
        }
        curr_epoch_users.clear();
      }
      if (!prev_epoch_users.empty())
      {
        for (LegionList<LogicalUser,PREV_LOGICAL_ALLOC>::track_aligned::
              const_iterator it = prev_epoch_users.begin(); it != 
              prev_epoch_users.end(); it++)
        {
          it->op->remove_mapping_reference(it->gen); 
        }
        prev_epoch_users.clear();
      }
    }

    //--------------------------------------------------------------------------
    void LogicalState::reset(void)
    //--------------------------------------------------------------------------
    {
      field_states.clear();
      clear_logical_users(); 
      reduction_fields.clear();
      outstanding_reductions.clear();
      for (std::list<ProjectionEpoch*>::const_iterator it = 
            projection_epochs.begin(); it != projection_epochs.end(); it++)
        delete *it;
      projection_epochs.clear();
    } 

    //--------------------------------------------------------------------------
    void LogicalState::clear_deleted_state(const FieldMask &deleted_mask)
    //--------------------------------------------------------------------------
    {
      for (LegionList<FieldState>::aligned::iterator it = field_states.begin();
            it != field_states.end(); /*nothing*/)
      {
        it->valid_fields -= deleted_mask;
        if (!it->valid_fields)
        {
          it = field_states.erase(it);
          continue;
        }
        std::vector<LegionColor> to_delete;
        for (LegionMap<LegionColor,FieldMask>::aligned::iterator child_it = 
              it->open_children.begin(); child_it != 
              it->open_children.end(); child_it++)
        {
          child_it->second -= deleted_mask;
          if (!child_it->second)
            to_delete.push_back(child_it->first);
        }
        if (!to_delete.empty())
        {
          for (std::vector<LegionColor>::const_iterator cit = to_delete.begin();
                cit != to_delete.end(); cit++)
            it->open_children.erase(*cit);
        }
        if (!it->open_children.empty())
          it++;
        else
          it = field_states.erase(it);
      }
      reduction_fields -= deleted_mask;
      if (!outstanding_reductions.empty())
      {
        std::vector<ReductionOpID> to_delete;
        for (LegionMap<ReductionOpID,FieldMask>::aligned::iterator it = 
              outstanding_reductions.begin(); it != 
              outstanding_reductions.end(); it++)
        {
          it->second -= deleted_mask;
          if (!it->second)
            to_delete.push_back(it->first);
        }
        for (std::vector<ReductionOpID>::const_iterator it = 
              to_delete.begin(); it != to_delete.end(); it++)
        {
          outstanding_reductions.erase(*it);
        }
      }
    }

    //--------------------------------------------------------------------------
    void LogicalState::advance_projection_epochs(const FieldMask &advance_mask)
    //--------------------------------------------------------------------------
    {
      // See if we can get some coalescing going on here
      std::map<ProjectionEpochID,ProjectionEpoch*> to_add; 
      for (std::list<ProjectionEpoch*>::iterator it = 
            projection_epochs.begin(); it != 
            projection_epochs.end(); /*nothing*/)
      {
        FieldMask overlap = (*it)->valid_fields & advance_mask;
        if (!overlap)
        {
          it++;
          continue;
        }
        const ProjectionEpochID next_epoch_id = (*it)->epoch_id + 1;
        std::map<ProjectionEpochID,ProjectionEpoch*>::iterator finder = 
          to_add.find(next_epoch_id);
        if (finder == to_add.end())
        {
          ProjectionEpoch *next_epoch = 
            new ProjectionEpoch((*it)->epoch_id+1, overlap);
          to_add[next_epoch_id] = next_epoch;
        }
        else
          finder->second->valid_fields |= overlap;
        // Filter the fields from our old one
        (*it)->valid_fields -= overlap;
        if (!((*it)->valid_fields))
        {
          delete (*it);
          it = projection_epochs.erase(it);
        }
        else
          it++;
      }
      if (!to_add.empty())
      {
        for (std::map<ProjectionEpochID,ProjectionEpoch*>::const_iterator it = 
              to_add.begin(); it != to_add.end(); it++)
          projection_epochs.push_back(it->second);
      }
    } 

    //--------------------------------------------------------------------------
    void LogicalState::update_projection_epochs(FieldMask capture_mask,
                                                const ProjectionInfo &info)
    //--------------------------------------------------------------------------
    {
#ifdef DEBUG_LEGION
      assert(!!capture_mask);
#endif
      for (std::list<ProjectionEpoch*>::const_iterator it = 
            projection_epochs.begin(); it != projection_epochs.end(); it++)
      {
        FieldMask overlap = (*it)->valid_fields & capture_mask;
        if (!overlap)
          continue;
        capture_mask -= overlap;
        if (!capture_mask)
          return;
      }
      // If it didn't already exist, start a new projection epoch
      ProjectionEpoch *new_epoch = 
        new ProjectionEpoch(ProjectionEpoch::first_epoch, capture_mask);
      projection_epochs.push_back(new_epoch);
    }

    /////////////////////////////////////////////////////////////
    // FieldState 
    /////////////////////////////////////////////////////////////

    //--------------------------------------------------------------------------
    FieldState::FieldState(void)
      : open_state(NOT_OPEN), redop(0), projection(NULL), 
        projection_space(NULL), rebuild_timeout(1)
    //--------------------------------------------------------------------------
    {
    }

    //--------------------------------------------------------------------------
    FieldState::FieldState(const GenericUser &user, const FieldMask &m, 
                           const LegionColor c)
      : ChildState(m), redop(0), projection(NULL), 
        projection_space(NULL), rebuild_timeout(1)
    //--------------------------------------------------------------------------
    {
      if (IS_READ_ONLY(user.usage))
        open_state = OPEN_READ_ONLY;
      else if (IS_WRITE(user.usage))
        open_state = OPEN_READ_WRITE;
      else if (IS_REDUCE(user.usage))
      {
        open_state = OPEN_SINGLE_REDUCE;
        redop = user.usage.redop;
      }
      open_children[c] = m;
    }

    //--------------------------------------------------------------------------
    FieldState::FieldState(const RegionUsage &usage, const FieldMask &m,
                           ProjectionFunction *proj, IndexSpaceNode *proj_space,
                           bool disjoint, bool dirty_reduction)
      : ChildState(m), redop(0), projection(proj), 
        projection_space(proj_space), rebuild_timeout(1)
    //--------------------------------------------------------------------------
    {
#ifdef DEBUG_LEGION
      assert(projection != NULL);
#endif
      if (IS_READ_ONLY(usage))
        open_state = OPEN_READ_ONLY_PROJ;
      else if (IS_REDUCE(usage))
      {
        if (dirty_reduction)
          open_state = OPEN_REDUCE_PROJ_DIRTY;
        else
          open_state = OPEN_REDUCE_PROJ;
        redop = usage.redop;
      }
      else if (disjoint && (projection->depth == 0))
        open_state = OPEN_READ_WRITE_PROJ_DISJOINT_SHALLOW;
      else
        open_state = OPEN_READ_WRITE_PROJ;
    }

    //--------------------------------------------------------------------------
    bool FieldState::overlaps(const FieldState &rhs) const
    //--------------------------------------------------------------------------
    {
      if (redop != rhs.redop)
        return false;
      if (projection != rhs.projection)
        return false;
      // Only do this test if they are both projections
      if ((projection != NULL) && (projection_space != rhs.projection_space))
        return false;
      if (redop == 0)
        return (open_state == rhs.open_state);
      else
      {
#ifdef DEBUG_LEGION
        assert((open_state == OPEN_SINGLE_REDUCE) ||
               (open_state == OPEN_MULTI_REDUCE) ||
               (open_state == OPEN_REDUCE_PROJ) ||
               (open_state == OPEN_REDUCE_PROJ_DIRTY));
        assert((rhs.open_state == OPEN_SINGLE_REDUCE) ||
               (rhs.open_state == OPEN_MULTI_REDUCE) ||
               (rhs.open_state == OPEN_REDUCE_PROJ) ||
               (rhs.open_state == OPEN_REDUCE_PROJ_DIRTY));
#endif
        // Only support merging reduction fields with exactly the
        // same mask which should be single fields for reductions
        return (valid_fields == rhs.valid_fields);
      }
    }

    //--------------------------------------------------------------------------
    void FieldState::merge(const FieldState &rhs, RegionTreeNode *node)
    //--------------------------------------------------------------------------
    {
      valid_fields |= rhs.valid_fields;
      for (LegionMap<LegionColor,FieldMask>::aligned::const_iterator it = 
            rhs.open_children.begin(); it != rhs.open_children.end(); it++)
      {
        LegionMap<LegionColor,FieldMask>::aligned::iterator finder = 
                                      open_children.find(it->first);
        if (finder == open_children.end())
          open_children[it->first] = it->second;
        else
          finder->second |= it->second;
      }
#ifdef DEBUG_LEGION
      assert(redop == rhs.redop);
      assert(projection == rhs.projection);
#endif
      if (redop > 0)
      {
#ifdef DEBUG_LEGION
        assert(!open_children.empty());
#endif
        // For the reductions, handle the case where we need to merge
        // reduction modes, if they are all disjoint, we don't need
        // to distinguish between single and multi reduce
        if (node->are_all_children_disjoint())
        {
          open_state = OPEN_READ_WRITE;
          redop = 0;
        }
        else
        {
          if (open_children.size() == 1)
            open_state = OPEN_SINGLE_REDUCE;
          else
            open_state = OPEN_MULTI_REDUCE;
        }
      }
    }

    //--------------------------------------------------------------------------
    bool FieldState::projection_domain_dominates(
                                               IndexSpaceNode *next_space) const
    //--------------------------------------------------------------------------
    {
#ifdef DEBUG_LEGION
      assert(projection_space != NULL);
#endif
      if (projection_space == next_space)
        return true;
      // If the domains do not have the same type, the answer must be no
      if (projection_space->handle.get_type_tag() != 
          next_space->handle.get_type_tag())
        return false;
      return projection_space->dominates(next_space);
    }

    //--------------------------------------------------------------------------
    void FieldState::print_state(TreeStateLogger *logger,
                                 const FieldMask &capture_mask,
                                 RegionNode *node) const
    //--------------------------------------------------------------------------
    {
      switch (open_state)
      {
        case NOT_OPEN:
          {
            logger->log("Field State: NOT OPEN (%ld)", 
                        open_children.size());
            break;
          }
        case OPEN_READ_WRITE:
          {
            logger->log("Field State: OPEN READ WRITE (%ld)", 
                        open_children.size());
            break;
          }
        case OPEN_READ_ONLY:
          {
            logger->log("Field State: OPEN READ-ONLY (%ld)", 
                        open_children.size());
            break;
          }
        case OPEN_SINGLE_REDUCE:
          {
            logger->log("Field State: OPEN SINGLE REDUCE Mode %d (%ld)", 
                        redop, open_children.size());
            break;
          }
        case OPEN_MULTI_REDUCE:
          {
            logger->log("Field State: OPEN MULTI REDUCE Mode %d (%ld)", 
                        redop, open_children.size());
            break;
          }
        case OPEN_READ_ONLY_PROJ:
          {
            logger->log("Field State: OPEN READ-ONLY PROJECTION %d",
                        projection->projection_id);
            break;
          }
        case OPEN_READ_WRITE_PROJ:
          {
            logger->log("Field State: OPEN READ WRITE PROJECTION %d",
                        projection->projection_id);
            break;
          }
        case OPEN_READ_WRITE_PROJ_DISJOINT_SHALLOW:
          {
            logger->log("Field State: OPEN READ WRITE PROJECTION (Disjoint Shallow) %d",
                        projection->projection_id);
            break;
          }
        case OPEN_REDUCE_PROJ:
          {
            logger->log("Field State: OPEN REDUCE PROJECTION %d Mode %d",
                        projection->projection_id, redop);
            break;
          }
        case OPEN_REDUCE_PROJ_DIRTY:
          {
            logger->log("Field State: OPEN REDUCE PROJECTION (Dirty) %d Mode %d",
                        projection->projection_id, redop);
            break;
          }
        default:
          assert(false);
      }
      logger->down();
      for (LegionMap<LegionColor,FieldMask>::aligned::const_iterator it = 
            open_children.begin(); it != open_children.end(); it++)
      {
        FieldMask overlap = it->second & capture_mask;
        if (!overlap)
          continue;
        char *mask_buffer = overlap.to_string();
        logger->log("Color %d   Mask %s", it->first, mask_buffer);
        free(mask_buffer);
      }
      logger->up();
    }

    //--------------------------------------------------------------------------
    void FieldState::print_state(TreeStateLogger *logger,
                                 const FieldMask &capture_mask,
                                 PartitionNode *node) const
    //--------------------------------------------------------------------------
    {
      switch (open_state)
      {
        case NOT_OPEN:
          {
            logger->log("Field State: NOT OPEN (%ld)", 
                        open_children.size());
            break;
          }
        case OPEN_READ_WRITE:
          {
            logger->log("Field State: OPEN READ WRITE (%ld)", 
                        open_children.size());
            break;
          }
        case OPEN_READ_ONLY:
          {
            logger->log("Field State: OPEN READ-ONLY (%ld)", 
                        open_children.size());
            break;
          }
        case OPEN_SINGLE_REDUCE:
          {
            logger->log("Field State: OPEN SINGLE REDUCE Mode %d (%ld)", 
                        redop, open_children.size());
            break;
          }
        case OPEN_MULTI_REDUCE:
          {
            logger->log("Field State: OPEN MULTI REDUCE Mode %d (%ld)", 
                        redop, open_children.size());
            break;
          }
        case OPEN_READ_ONLY_PROJ:
          {
            logger->log("Field State: OPEN READ-ONLY PROJECTION %d",
                        projection->projection_id);
            break;
          }
        case OPEN_READ_WRITE_PROJ:
          {
            logger->log("Field State: OPEN READ WRITE PROJECTION %d",
                        projection->projection_id);
            break;
          }
        case OPEN_READ_WRITE_PROJ_DISJOINT_SHALLOW:
          {
            logger->log("Field State: OPEN READ WRITE PROJECTION (Disjoint Shallow) %d",
                        projection->projection_id);
            break;
          }
        case OPEN_REDUCE_PROJ:
          {
            logger->log("Field State: OPEN REDUCE PROJECTION %d Mode %d",
                        projection->projection_id, redop);
            break;
          }
        case OPEN_REDUCE_PROJ_DIRTY:
          {
            logger->log("Field State: OPEN REDUCE PROJECTION (Dirty) %d Mode %d",
                        projection->projection_id, redop);
            break;
          }
        default:
          assert(false);
      }
      logger->down();
      for (LegionMap<LegionColor,FieldMask>::aligned::const_iterator it = 
            open_children.begin(); it != open_children.end(); it++)
      {
        DomainPoint color =
          node->row_source->color_space->delinearize_color_to_point(it->first);
        FieldMask overlap = it->second & capture_mask;
        if (!overlap)
          continue;
        char *mask_buffer = overlap.to_string();
        switch (color.get_dim())
        {
          case 1:
            {
              logger->log("Color %d   Mask %s", 
                          color[0], mask_buffer);
              break;
            }
          case 2:
            {
              logger->log("Color (%d,%d)   Mask %s", 
                          color[0], color[1], mask_buffer);
              break;
            }
          case 3:
            {
              logger->log("Color (%d,%d,%d)   Mask %s", 
                          color[0], color[1], color[2], mask_buffer);
              break;
            }
          default:
            assert(false); // implemenent more dimensions
        }
        free(mask_buffer);
      }
      logger->up();
    }

    /////////////////////////////////////////////////////////////
    // Logical Closer 
    /////////////////////////////////////////////////////////////

    //--------------------------------------------------------------------------
    LogicalCloser::LogicalCloser(ContextID c, const LogicalUser &u, 
                                 RegionTreeNode *r, bool val)
      : ctx(c), user(u), root_node(r), validates(val), close_op(NULL)
    //--------------------------------------------------------------------------
    {
    }

    //--------------------------------------------------------------------------
    LogicalCloser::LogicalCloser(const LogicalCloser &rhs)
      : user(rhs.user), root_node(rhs.root_node), validates(rhs.validates)
    //--------------------------------------------------------------------------
    {
      // should never be called
      assert(false);
    }

    //--------------------------------------------------------------------------
    LogicalCloser::~LogicalCloser(void)
    //--------------------------------------------------------------------------
    {
    }

    //--------------------------------------------------------------------------
    LogicalCloser& LogicalCloser::operator=(const LogicalCloser &rhs)
    //--------------------------------------------------------------------------
    {
      // should never be called
      assert(false);
      return *this;
    }

    //--------------------------------------------------------------------------
    void LogicalCloser::record_close_operation(const FieldMask &mask) 
    //--------------------------------------------------------------------------
    {
#ifdef DEBUG_LEGION
      assert(!!mask);
#endif
      close_mask |= mask;
    }

    //--------------------------------------------------------------------------
    void LogicalCloser::record_closed_user(const LogicalUser &user,
                                           const FieldMask &mask)
    //--------------------------------------------------------------------------
    {
      closed_users.push_back(user);
      LogicalUser &closed_user = closed_users.back();
      closed_user.field_mask = mask;
    }

#ifndef LEGION_SPY
    //--------------------------------------------------------------------------
    void LogicalCloser::pop_closed_user(void)
    //--------------------------------------------------------------------------
    {
      closed_users.pop_back();
    }
#endif

    //--------------------------------------------------------------------------
    void LogicalCloser::initialize_close_operations(LogicalState &state, 
                                             Operation *creator,
                                             const LogicalTraceInfo &trace_info)
    //--------------------------------------------------------------------------
    {
#ifdef DEBUG_LEGION
      // These sets of fields better be disjoint
      assert(!!close_mask);
      assert(close_op == NULL);
#endif
      // Construct a reigon requirement for this operation
      // All privileges are based on the parent logical region
      RegionRequirement req;
      if (root_node->is_region())
        req = RegionRequirement(root_node->as_region_node()->handle,
                                READ_WRITE, EXCLUSIVE, trace_info.req.parent);
      else
        req = RegionRequirement(root_node->as_partition_node()->handle, 0,
                                READ_WRITE, EXCLUSIVE, trace_info.req.parent);
      close_op = creator->runtime->get_available_merge_close_op();
      merge_close_gen = close_op->get_generation();
      req.privilege_fields.clear();
      root_node->column_source->get_field_set(close_mask,
                                             trace_info.req.privilege_fields,
                                             req.privilege_fields);
      close_op->initialize(creator->get_context(), req, trace_info, 
                           trace_info.req_idx, close_mask, creator);
    }

    //--------------------------------------------------------------------------
    void LogicalCloser::perform_dependence_analysis(const LogicalUser &current,
                                                    const FieldMask &open_below,
              LegionList<LogicalUser,CURR_LOGICAL_ALLOC>::track_aligned &cusers,
              LegionList<LogicalUser,PREV_LOGICAL_ALLOC>::track_aligned &pusers)
    //--------------------------------------------------------------------------
    {
      // We also need to do dependence analysis against all the other operations
      // that this operation recorded dependences on above in the tree so we
      // don't run too early.
      LegionList<LogicalUser,LOGICAL_REC_ALLOC>::track_aligned &above_users = 
                                              current.op->get_logical_records();
      const LogicalUser merge_close_user(close_op, 0/*idx*/, 
          RegionUsage(READ_WRITE, EXCLUSIVE, 0/*redop*/), close_mask);
      register_dependences(close_op, merge_close_user, current, 
          open_below, closed_users, above_users, cusers, pusers);
      // Now we can remove our references on our local users
      for (LegionList<LogicalUser>::aligned::const_iterator it = 
            closed_users.begin(); it != closed_users.end(); it++)
      {
        it->op->remove_mapping_reference(it->gen);
      }
    }

    // If you are looking for LogicalCloser::register_dependences it can 
    // be found in region_tree.cc to make sure that templates are instantiated

    //--------------------------------------------------------------------------
    void LogicalCloser::update_state(LogicalState &state)
    //--------------------------------------------------------------------------
    {
#ifdef DEBUG_LEGION
      assert(state.owner == root_node);
#endif
      root_node->filter_prev_epoch_users(state, close_mask);
      root_node->filter_curr_epoch_users(state, close_mask);
    }

    //--------------------------------------------------------------------------
    void LogicalCloser::register_close_operations(
               LegionList<LogicalUser,CURR_LOGICAL_ALLOC>::track_aligned &users)
    //--------------------------------------------------------------------------
    {
      // No need to add mapping references, we did that in 
      // Note we also use the cached generation IDs since the close
      // operations have already been kicked off and might be done
      // LogicalCloser::register_dependences
      const LogicalUser close_user(close_op, merge_close_gen,0/*idx*/,
        RegionUsage(READ_WRITE, EXCLUSIVE, 0/*redop*/), close_mask);
      users.push_back(close_user);
    }

    /////////////////////////////////////////////////////////////
    // Copy Fill Aggregator
    /////////////////////////////////////////////////////////////

    //--------------------------------------------------------------------------
    CopyFillAggregator::CopyFillAggregator(RegionTreeForest *f, Operation *o,
                                    unsigned idx, std::set<RtEvent> &ev, bool t)
      : WrapperReferenceMutator(ev), forest(f), op(o), index(idx), 
        track_events(t), effects(ev)
    //--------------------------------------------------------------------------
    {
    }

    //--------------------------------------------------------------------------
    CopyFillAggregator::CopyFillAggregator(const CopyFillAggregator &rhs)
      : WrapperReferenceMutator(rhs.effects), forest(rhs.forest), op(rhs.op),
        index(rhs.index), track_events(rhs.track_events), effects(rhs.effects)
    //--------------------------------------------------------------------------
    {
      // Should never be called
      assert(false);
    }

    //--------------------------------------------------------------------------
    CopyFillAggregator::~CopyFillAggregator(void)
    //--------------------------------------------------------------------------
    {
      // Remove references from any views that we have
      for (std::set<LogicalView*>::const_iterator it = 
            all_views.begin(); it != all_views.end(); it++)
        if ((*it)->remove_base_valid_ref(AGGREGATORE_REF))
          delete (*it);
      // Delete all our copy updates
      for (LegionMap<InstanceView*,FieldMaskSet<Update> >::aligned::
            const_iterator mit = sources.begin(); mit != sources.end(); mit++)
      {
        for (FieldMaskSet<Update>::const_iterator it = 
              mit->second.begin(); it != mit->second.end(); it++)
          delete it->first;
      }
      for (LegionMap<InstanceView*,FieldMaskSet<Update> >::aligned::
            const_iterator mit = reductions.begin(); 
            mit != reductions.end(); mit++)
      {
        for (FieldMaskSet<Update>::const_iterator it = 
              mit->second.begin(); it != mit->second.end(); it++)
          delete it->first;
      }
    }

    //--------------------------------------------------------------------------
    CopyFillAggregator& CopyFillAggregator::operator=(
                                                  const CopyFillAggregator &rhs)
    //--------------------------------------------------------------------------
    {
      // should never be called
      assert(false);
      return *this;
    }

    //--------------------------------------------------------------------------
    void CopyFillAggregator::CopyUpdate::record_source_expressions(
                                            InstanceFieldExprs &src_exprs) const
    //--------------------------------------------------------------------------
    {
      FieldMaskSet<IndexSpaceExpression> &exprs = src_exprs[source];  
      FieldMaskSet<IndexSpaceExpression>::iterator finder = 
        exprs.find(expr);
      if (finder == exprs.end())
        exprs.insert(expr, src_mask);
      else
        finder.merge(src_mask);
    }

    //--------------------------------------------------------------------------
    void CopyFillAggregator::CopyUpdate::compute_source_preconditions(
                     RegionTreeForest *forest,
                     const std::map<InstanceView*,EventFieldExprs> &src_pre,
                     LegionMap<ApEvent,FieldMask>::aligned &preconditions) const
    //--------------------------------------------------------------------------
    {
      std::map<InstanceView*,EventFieldExprs>::const_iterator finder = 
        src_pre.find(source);
      if (finder == src_pre.end())
        return;
      for (EventFieldExprs::const_iterator eit = 
            finder->second.begin(); eit != finder->second.end(); eit++)
      {
        FieldMask set_overlap = src_mask & eit->second.get_valid_mask();
        if (!set_overlap)
          continue;
        for (FieldMaskSet<IndexSpaceExpression>::const_iterator it = 
              eit->second.begin(); it != eit->second.end(); it++)
        {
          const FieldMask overlap = set_overlap & it->second;
          if (!overlap)
            continue;
          IndexSpaceExpression *expr_overlap = 
            forest->intersect_index_spaces(expr, it->first);
          if (expr_overlap->is_empty())
            continue;
          // Overlap in both so record it
          LegionMap<ApEvent,FieldMask>::aligned::iterator
            event_finder = preconditions.find(eit->first);
          if (event_finder == preconditions.end())
            preconditions[eit->first] = overlap;
          else
            event_finder->second |= overlap;
          set_overlap -= overlap;
          if (!set_overlap)
            break;
        }
      }
    }

    //--------------------------------------------------------------------------
    void CopyFillAggregator::CopyUpdate::sort_updates(std::map<InstanceView*,
                                           std::vector<CopyUpdate*> > &copies,
                                          std::vector<ReduceUpdate*> &reduces,
                                          std::vector<FillUpdate*> &fills)
    //--------------------------------------------------------------------------
    {
      copies[source].push_back(this);
    }

    //--------------------------------------------------------------------------
    void CopyFillAggregator::FillUpdate::record_source_expressions(
                                            InstanceFieldExprs &src_exprs) const
    //--------------------------------------------------------------------------
    {
      // Do nothing, we have no source expressions
    }

    //--------------------------------------------------------------------------
    void CopyFillAggregator::FillUpdate::compute_source_preconditions(
                     RegionTreeForest *forest,
                     const std::map<InstanceView*,EventFieldExprs> &src_pre,
                     LegionMap<ApEvent,FieldMask>::aligned &preconditions) const
    //--------------------------------------------------------------------------
    {
      // Do nothing, we have no source preconditions to worry about
    }

    //--------------------------------------------------------------------------
    void CopyFillAggregator::FillUpdate::sort_updates(std::map<InstanceView*,
                                           std::vector<CopyUpdate*> > &copies,
                                          std::vector<ReduceUpdate*> &reduces,
                                          std::vector<FillUpdate*> &fills)
    //--------------------------------------------------------------------------
    {
      fills.push_back(this);
    }

    //--------------------------------------------------------------------------
    void CopyFillAggregator::ReduceUpdate::record_source_expressions(
                                            InstanceFieldExprs &src_exprs) const
    //--------------------------------------------------------------------------
    {
      FieldMask src_mask;
      src_mask.set_bit(src_fidx);
      for (std::vector<ReductionView*>::const_iterator sit = 
            sources.begin(); sit != sources.end(); sit++)
      {
        FieldMaskSet<IndexSpaceExpression> &exprs = src_exprs[*sit];
        FieldMaskSet<IndexSpaceExpression>::iterator finder = 
          exprs.find(expr);
        if (finder == exprs.end())
          exprs.insert(expr, src_mask);
        else
          finder.merge(src_mask);
      }
    }

    //--------------------------------------------------------------------------
    void CopyFillAggregator::ReduceUpdate::compute_source_preconditions(
                     RegionTreeForest *forest,
                     const std::map<InstanceView*,EventFieldExprs> &src_pre,
                     LegionMap<ApEvent,FieldMask>::aligned &preconditions) const
    //--------------------------------------------------------------------------
    {
      FieldMask src_mask;
      src_mask.set_bit(src_fidx);
      for (std::vector<ReductionView*>::const_iterator sit = 
            sources.begin(); sit != sources.end(); sit++)
      {
        std::map<InstanceView*,EventFieldExprs>::const_iterator expr_finder =
          src_pre.find(*sit);
        if (expr_finder == src_pre.end())
          continue;
        for (EventFieldExprs::const_iterator eit = expr_finder->second.begin();
              eit != expr_finder->second.end(); eit++)
        {
          if (!eit->second.get_valid_mask().is_set(src_fidx))
            continue;
          for (FieldMaskSet<IndexSpaceExpression>::const_iterator it = 
                eit->second.begin(); it != eit->second.end(); it++)
          {
            if (!it->second.is_set(src_fidx))
              continue;
            IndexSpaceExpression *overlap = 
              forest->intersect_index_spaces(expr, it->first);
            if (overlap->is_empty())
              continue;
            // Overlap on both so record them
            LegionMap<ApEvent,FieldMask>::aligned::iterator
              finder = preconditions.find(eit->first);
            if (finder == preconditions.end())
              preconditions[eit->first] = src_mask;
            else
              finder->second.set_bit(src_fidx);
            // We found an overlap for this event and field so 
            // we can go on to the next event
            break;
          }
        }
      }
    }

    //--------------------------------------------------------------------------
    void CopyFillAggregator::ReduceUpdate::sort_updates(std::map<InstanceView*,
                                           std::vector<CopyUpdate*> > &copies,
                                          std::vector<ReduceUpdate*> &reduces,
                                          std::vector<FillUpdate*> &fills)
    //--------------------------------------------------------------------------
    {
      reduces.push_back(this);
    }

    //--------------------------------------------------------------------------
    void CopyFillAggregator::record_updates(InstanceView *dst_view, 
                                    const FieldMaskSet<LogicalView> &src_views,
                                    const FieldMask &src_mask,
                                    IndexSpaceExpression *expr,
                                    ReductionOpID redop /*=0*/,
                                    CopyAcrossHelper *helper /*=NULL*/)
    //--------------------------------------------------------------------------
    {
#ifdef DEBUG_LEGION
      assert(!!src_mask);
      assert(!src_views.empty());
      assert(!expr->is_empty());
#endif
      FieldMaskSet<Update> &updates = sources[dst_view];
      record_view(dst_view);
      if (src_views.size() == 1)
      {
        const LogicalView *view = src_views.begin()->first;
        const FieldMask record_mask = 
          src_views.get_valid_mask() & src_mask;
#ifdef DEBUG_LEGION
        assert(!!record_mask);
#endif
        if (view->is_instance_view())
        {
          InstanceView *inst = view->as_instance_view();
          record_view(inst);
          CopyUpdate *update = 
            new CopyUpdate(inst, record_mask, expr, redop, helper);
          if (helper == NULL)
            updates.insert(update, record_mask);
          else
            updates.insert(update, helper->convert_src_to_dst(record_mask));
        }
        else
        {
          DeferredView *def = view->as_deferred_view();
          def->flatten(*this, dst_view, record_mask, expr, helper);
        }
      }
      else
      {
        // We have multiple views, so let's sort them
        LegionList<FieldSet<LogicalView*> >::aligned view_sets;
        src_views.compute_field_sets(src_mask, view_sets);
        for (LegionList<FieldSet<LogicalView*> >::aligned::const_iterator
              vit = view_sets.begin(); vit != view_sets.end(); vit++)
        {
#ifdef DEBUG_LEGION
          assert(!vit->elements.empty());
#endif
          if (vit->elements.size() == 1)
          {
            // Easy case, just one view so do it  
            const LogicalView *view = *(vit->elements.begin());
            const FieldMask &record_mask = vit->set_mask;
            if (view->is_instance_view())
            {
              InstanceView *inst = view->as_instance_view();
              record_view(inst);
              CopyUpdate *update = 
                new CopyUpdate(inst, record_mask, expr, redop, helper);
              if (helper == NULL)
                updates.insert(update, record_mask);
              else
                updates.insert(update, helper->convert_src_to_dst(record_mask));
            }
            else
            {
              DeferredView *def = view->as_deferred_view();
              def->flatten(*this, dst_view, record_mask, expr, helper);
            }
          }
          else
          {
            // Sort the views, prefer fills, then instances, then deferred
            FillView *fill = NULL;
            DeferredView *deferred = NULL;
            std::vector<InstanceView*> instances;
            for (std::set<LogicalView*>::const_iterator it = 
                  vit->elements.begin(); it != vit->elements.end(); it++)
            {
              if (!(*it)->is_instance_view())
              {
                DeferredView *def = (*it)->as_deferred_view();
                if (!def->is_fill_view())
                {
                  if (deferred == NULL)
                    deferred = def;
                }
                else
                {
                  fill = def->as_fill_view();
                  // Break out since we found what we're looking for
                  break;
                }
              }
              else
                instances.push_back((*it)->as_instance_view());
            }
            if (fill != NULL)
              record_fill(dst_view, fill, vit->set_mask, expr, helper);
            else if (!instances.empty())
            {
              if (instances.size() == 1)
              {
                // Easy, just one instance to use
                InstanceView *inst = instances.back();
                record_view(inst);
                CopyUpdate *update = 
                  new CopyUpdate(inst, vit->set_mask, expr, redop, helper);
                if (helper == NULL)
                  updates.insert(update, vit->set_mask);
                else
                  updates.insert(update, 
                      helper->convert_src_to_dst(vit->set_mask));
              }
              else
              {
                // Hard, multiple potential sources,
                // ask the mapper which one to use
                // First though check to see if we've already asked it
                bool found = false;
                const std::set<InstanceView*> instances_set(instances.begin(),
                                                            instances.end());
                std::map<InstanceView*,LegionVector<SourceQuery>::aligned>::
                  const_iterator finder = mapper_queries.find(dst_view);
                if (finder != mapper_queries.end())
                {
                  for (LegionVector<SourceQuery>::aligned::const_iterator qit = 
                        finder->second.begin(); qit != 
                        finder->second.end(); qit++)
                  {
                    if ((qit->query_mask == vit->set_mask) &&
                        (qit->sources == instances_set))
                    {
                      found = true;
                      record_view(qit->result);
                      CopyUpdate *update = new CopyUpdate(qit->result, 
                                    qit->query_mask, expr, redop, helper);
                      if (helper == NULL)
                        updates.insert(update, qit->query_mask);
                      else
                        updates.insert(update, 
                            helper->convert_src_to_dst(qit->query_mask));
                      break;
                    }
                  }
                }
                if (!found)
                {
                  // If we didn't find the query result we need to do
                  // it for ourself, start by constructing the inputs
                  InstanceRef dst(dst_view->get_manager(),
                      helper == NULL ? vit->set_mask : 
                        helper->convert_src_to_dst(vit->set_mask));
                  InstanceSet sources(instances.size());
                  unsigned src_idx = 0;
                  for (std::vector<InstanceView*>::const_iterator it = 
                        instances.begin(); it != instances.end(); it++)
                    sources[src_idx++] = InstanceRef((*it)->get_manager(),
                                                     vit->set_mask);
                  std::vector<unsigned> ranking;
                  op->select_sources(dst, sources, ranking);
                  // We know that which ever one was chosen first is
                  // the one that satisfies all our fields since all
                  // these instances are valid for all fields
                  InstanceView *result = ranking.empty() ? 
                    instances.front() : instances[ranking[0]];
                  // Record the update
                  record_view(result);
                  CopyUpdate *update = new CopyUpdate(result, vit->set_mask,
                                                      expr, redop, helper);
                  if (helper == NULL)
                    updates.insert(update, vit->set_mask);
                  else
                    updates.insert(update, 
                        helper->convert_src_to_dst(vit->set_mask));
                  // Save the result for the future
                  mapper_queries[dst_view].push_back(
                      SourceQuery(instances_set, vit->set_mask, result));
                }
              }
            }
            else
            {
#ifdef DEBUG_LEGION
              assert(deferred != NULL);
#endif
              deferred->flatten(*this, dst_view, vit->set_mask, expr, helper);
            }
          }
        }
      }
    }

    //--------------------------------------------------------------------------
    void CopyFillAggregator::record_fill(InstanceView *dst_view,
                                         FillView *src_view,
                                         const FieldMask &fill_mask,
                                         IndexSpaceExpression *expr,
                                         CopyAcrossHelper *helper /*=NULL*/)
    //--------------------------------------------------------------------------
    {
      // No need to record the destination as we already did that the first
      // time through on our way to finding this fill view
#ifdef DEBUG_LEGION
      assert(all_views.find(dst_view) != all_views.end());
      assert(!!fill_mask);
      assert(!expr->is_empty());
#endif
      record_view(src_view);
      FillUpdate *update = new FillUpdate(src_view, fill_mask, expr, helper); 
      if (helper == NULL)
        sources[dst_view].insert(update, fill_mask);
      else
        sources[dst_view].insert(update, helper->convert_src_to_dst(fill_mask));
    }

    //--------------------------------------------------------------------------
    void CopyFillAggregator::record_reductions(InstanceView *dst_view,
                                   const std::vector<ReductionView*> &src_views,
                                   const unsigned src_fidx,
                                   const unsigned dst_fidx,
                                   IndexSpaceExpression *expr,
                                   CopyAcrossHelper *across_helper)
    //--------------------------------------------------------------------------
    {
#ifdef DEBUG_LEGION
      assert(!src_views.empty());
      assert(!expr->is_empty());
#endif 
      record_view(dst_view);
      for (std::vector<ReductionView*>::const_iterator it = 
            src_views.begin(); it != src_views.end(); it++)
        record_view(*it);
      ReduceUpdate *update = 
        new ReduceUpdate(src_views, src_fidx, dst_fidx, expr, across_helper);
      FieldMask dst_mask;
      dst_mask.set_bit(dst_fidx);
      reductions[dst_view].insert(update, dst_mask);
    }

    //--------------------------------------------------------------------------
    void CopyFillAggregator::record_preconditions(InstanceView *view, 
                                   bool reading, EventFieldExprs &preconditions)
    //--------------------------------------------------------------------------
    {
#ifdef DEBUG_LEGION
      assert(!preconditions.empty());
#endif
      AutoLock p_lock(pre_lock);
      EventFieldExprs &pre = reading ? src_pre[view] : dst_pre[view]; 
      for (EventFieldExprs::iterator eit = preconditions.begin();
            eit != preconditions.end(); eit++)
      {
        EventFieldExprs::iterator event_finder = pre.find(eit->first);
        if (event_finder != pre.end())
        {
          // Need to do the merge manually 
          for (FieldMaskSet<IndexSpaceExpression>::const_iterator it = 
                eit->second.begin(); it != eit->second.end(); it++)
          {
            FieldMaskSet<IndexSpaceExpression>::iterator finder = 
              event_finder->second.find(it->first);
            if (finder == event_finder->second.end())
              event_finder->second.insert(it->first, it->second);
            else
              finder.merge(it->second);
          }
        }
        else // We can just swap this over
          pre[eit->first].swap(eit->second);
      }
    }

    //--------------------------------------------------------------------------
    void CopyFillAggregator::issue_updates(const PhysicalTraceInfo &trace_info,
                                           ApEvent precondition)
    //--------------------------------------------------------------------------
    {
      // Perform updates from any sources first
      if (!sources.empty())
        perform_updates(sources, trace_info, precondition);
      // Then apply any reductions that we might have
      if (!reductions.empty())
        perform_updates(reductions, trace_info, precondition);
    }

    //--------------------------------------------------------------------------
    ApEvent CopyFillAggregator::summarize(const PhysicalTraceInfo &info) const
    //--------------------------------------------------------------------------
    {
#ifdef DEBUG_LEGION
      assert(track_events);
#endif
      if (!events.empty())
        return Runtime::merge_events(&info, events);
      else
        return ApEvent::NO_AP_EVENT;
    }

    //--------------------------------------------------------------------------
    void CopyFillAggregator::record_view(LogicalView *new_view)
    //--------------------------------------------------------------------------
    {
      std::pair<std::set<LogicalView*>::iterator,bool> result = 
        all_views.insert(new_view);
      if (result.second)
        new_view->add_base_valid_ref(AGGREGATORE_REF, this);
    }

    //--------------------------------------------------------------------------
    void CopyFillAggregator::perform_updates(
         const LegionMap<InstanceView*,FieldMaskSet<Update> >::aligned &updates,
         const PhysicalTraceInfo &trace_info, ApEvent precondition)
    //--------------------------------------------------------------------------
    {
      // First compute the access expressions for all the copies
      InstanceFieldExprs dst_exprs, src_exprs;
      for (LegionMap<InstanceView*,FieldMaskSet<Update> >::aligned::
            const_iterator uit = updates.begin(); uit != updates.end(); uit++)
      {
        FieldMaskSet<IndexSpaceExpression> &dst_expr = dst_exprs[uit->first];
        for (FieldMaskSet<Update>::const_iterator it = 
              uit->second.begin(); it != uit->second.end(); it++)
        {
          // Update the destinations first
          FieldMaskSet<IndexSpaceExpression>::iterator finder = 
            dst_expr.find(it->first->expr);
          if (finder == dst_expr.end())
            dst_expr.insert(it->first->expr, it->second);
          else
            finder.merge(it->second);
          // Now record the source expressions
          it->first->record_source_expressions(src_exprs);
        }
      }
      // Next compute the event preconditions for these accesses
      std::set<RtEvent> preconditions_ready; 
      const UniqueID op_id = op->get_unique_op_id();
      for (InstanceFieldExprs::const_iterator dit = 
            dst_exprs.begin(); dit != dst_exprs.end(); dit++)
      {
        if (dit->second.size() == 1)
        {
          // No need to do any kind of sorts here
          IndexSpaceExpression *copy_expr = dit->second.begin()->first;
          const FieldMask &copy_mask = dit->second.get_valid_mask();
          RtEvent pre_ready = dit->first->find_copy_preconditions(
                                false/*reading*/, copy_mask, copy_expr,
                                op_id, index, *this, trace_info);
          if (pre_ready.exists())
            preconditions_ready.insert(pre_ready);
        }
        else
        {
          // Sort into field sets and merge expressions
          LegionList<FieldSet<IndexSpaceExpression*> >::aligned sorted_exprs;
          dit->second.compute_field_sets(FieldMask(), sorted_exprs);
          for (LegionList<FieldSet<IndexSpaceExpression*> >::aligned::
                const_iterator it = sorted_exprs.begin(); 
                it != sorted_exprs.end(); it++)
          {
            const FieldMask &copy_mask = it->set_mask; 
            IndexSpaceExpression *copy_expr = (it->elements.size() == 1) ?
              *(it->elements.begin()) : 
              forest->union_index_spaces(it->elements);
            RtEvent pre_ready = dit->first->find_copy_preconditions(
                                  false/*reading*/, copy_mask, copy_expr,
                                  op_id, index, *this, trace_info);
            if (pre_ready.exists())
              preconditions_ready.insert(pre_ready);
          }
        }
      }
      for (InstanceFieldExprs::const_iterator sit = 
            src_exprs.begin(); sit != src_exprs.end(); sit++)
      {
        if (sit->second.size() == 1)
        {
          // No need to do any kind of sorts here
          IndexSpaceExpression *copy_expr = sit->second.begin()->first;
          const FieldMask &copy_mask = sit->second.get_valid_mask();
          RtEvent pre_ready = sit->first->find_copy_preconditions(
                                true/*reading*/, copy_mask, copy_expr,
                                op_id, index, *this, trace_info);
          if (pre_ready.exists())
            preconditions_ready.insert(pre_ready);
        }
        else
        {
          // Sort into field sets and merge expressions
          LegionList<FieldSet<IndexSpaceExpression*> >::aligned sorted_exprs;
          sit->second.compute_field_sets(FieldMask(), sorted_exprs);
          for (LegionList<FieldSet<IndexSpaceExpression*> >::aligned::
                const_iterator it = sorted_exprs.begin(); 
                it != sorted_exprs.end(); it++)
          {
            const FieldMask &copy_mask = it->set_mask; 
            IndexSpaceExpression *copy_expr = (it->elements.size() == 1) ?
              *(it->elements.begin()) : 
              forest->union_index_spaces(it->elements);
            RtEvent pre_ready = sit->first->find_copy_preconditions(
                                  true/*reading*/, copy_mask, copy_expr,
                                  op_id, index, *this, trace_info);
            if (pre_ready.exists())
              preconditions_ready.insert(pre_ready);
          }
        }
      }
      // If necessary wait until all we have all the preconditions
      if (!preconditions_ready.empty())
      {
        RtEvent wait_on = Runtime::merge_events(preconditions_ready);
        if (wait_on.exists())
          wait_on.wait();
      }
      // Iterate over the destinations and compute updates that have the
      // same preconditions on different fields
      std::map<std::set<ApEvent>,ApEvent> merge_cache;
      for (LegionMap<InstanceView*,FieldMaskSet<Update> >::aligned::
            const_iterator uit = updates.begin(); uit != updates.end(); uit++)
      {
        EventFieldUpdates update_groups;
        const EventFieldExprs &dst_preconditions = dst_pre[uit->first];
        for (FieldMaskSet<Update>::const_iterator it = 
              uit->second.begin(); it != uit->second.end(); it++)
        {
          // Compute the preconditions for this update
          // This is a little tricky for across copies because we need
          // to make sure that all the fields are in same field space
          // which will be the source field space, so we need to convert
          // some field masks back to that space if necessary
          LegionMap<ApEvent,FieldMask>::aligned preconditions;
          // Compute the destination preconditions first
          if (!dst_preconditions.empty())
          {
            for (EventFieldExprs::const_iterator pit = 
                  dst_preconditions.begin(); pit != 
                  dst_preconditions.end(); pit++)
            {
              FieldMask set_overlap = it->second & pit->second.get_valid_mask();
              if (!set_overlap)
                continue;
              for (FieldMaskSet<IndexSpaceExpression>::const_iterator eit =
                    pit->second.begin(); eit != pit->second.end(); eit++)
              {
                const FieldMask overlap = set_overlap & eit->second;
                if (!overlap)
                  continue;
                IndexSpaceExpression *expr_overlap = 
                  forest->intersect_index_spaces(eit->first, it->first->expr);
                if (expr_overlap->is_empty())
                  continue;
                // Overlap on both so add it to the set
                LegionMap<ApEvent,FieldMask>::aligned::iterator finder = 
                  preconditions.find(pit->first);
                // Make sure to convert back to the source field space
                // in the case of across copies if necessary
                if (finder == preconditions.end())
                {
                  if (it->first->across_helper == NULL)
                    preconditions[pit->first] = overlap;
                  else
                    preconditions[pit->first] = 
                      it->first->across_helper->convert_dst_to_src(overlap);
                }
                else
                {
                  if (it->first->across_helper == NULL)
                    finder->second |= overlap;
                  else
                    finder->second |= 
                      it->first->across_helper->convert_dst_to_src(overlap);
                }
                set_overlap -= overlap;
                // If we found preconditions on all our fields then we're done
                if (!set_overlap)
                  break;
              }
            }
          }
          // The compute the source preconditions for this update
          it->first->compute_source_preconditions(forest,src_pre,preconditions);
          if (preconditions.empty())
            // NO precondition so enter it with a no event
            update_groups[ApEvent::NO_AP_EVENT].insert(it->first, 
                                                       it->first->src_mask);
          else if (preconditions.size() == 1)
          {
            LegionMap<ApEvent,FieldMask>::aligned::const_iterator
              first = preconditions.begin();
            update_groups[first->first].insert(it->first, first->second);
            const FieldMask remainder = it->first->src_mask - first->second;
            if (!!remainder)
              update_groups[ApEvent::NO_AP_EVENT].insert(it->first, remainder);
          }
          else
          {
            // Group event preconditions by fields
            LegionList<FieldSet<ApEvent> >::aligned grouped_events;
            compute_field_sets<ApEvent>(it->first->src_mask,
                                        preconditions, grouped_events);
            for (LegionList<FieldSet<ApEvent> >::aligned::const_iterator ait =
                  grouped_events.begin(); ait != grouped_events.end(); ait++) 
            {
              ApEvent key;
              if (ait->elements.size() > 1)
              {
                // See if the set is in the cache or we need to compute it 
                std::map<std::set<ApEvent>,ApEvent>::const_iterator finder =
                  merge_cache.find(ait->elements);
                if (finder == merge_cache.end())
                {
                  key = Runtime::merge_events(&trace_info, ait->elements);
                  merge_cache[ait->elements] = key;
                }
                else
                  key = finder->second;
              }
              else if (ait->elements.size() == 1)
                key = *(ait->elements.begin());
              FieldMaskSet<Update> &group = update_groups[key]; 
              FieldMaskSet<Update>::iterator finder = group.find(it->first);
              if (finder != group.end())
                finder.merge(ait->set_mask);
              else
                group.insert(it->first, ait->set_mask);
            }
          }
        }
        // Now iterate over events and group by fields
        for (EventFieldUpdates::const_iterator eit = 
              update_groups.begin(); eit != update_groups.end(); eit++)
        {
          // Merge in the over-arching precondition if necessary
          const ApEvent group_precondition = precondition.exists() ? 
            Runtime::merge_events(&trace_info, precondition, eit->first) :
            eit->first;
          const FieldMaskSet<Update> &group = eit->second;
          if (group.size() == 1)
          {
            // Only one update so no need to try to group or merge 
            std::vector<FillUpdate*> fills;
            std::vector<ReduceUpdate*> reduces;
            std::map<InstanceView* /*src*/,std::vector<CopyUpdate*> > copies;
            Update *update = group.begin()->first;
            update->sort_updates(copies, reduces, fills);
            const FieldMask &update_mask = group.get_valid_mask();
            if (!fills.empty())
              issue_fills(uit->first, fills, group_precondition, 
                          update_mask, trace_info);
            if (!copies.empty())
              issue_copies(uit->first, copies, group_precondition, 
                           update_mask, trace_info);
            if (!reduces.empty())
              issue_reductions(uit->first, reduces, group_precondition,
                               update_mask, trace_info); 
          }
          else
          {
            // Group by fields
            LegionList<FieldSet<Update*> >::aligned field_groups;
            group.compute_field_sets(FieldMask(), field_groups);
            for (LegionList<FieldSet<Update*> >::aligned::const_iterator fit =
                  field_groups.begin(); fit != field_groups.end(); fit++)
            {
              std::vector<FillUpdate*> fills;
              std::vector<ReduceUpdate*> reduces;
              std::map<InstanceView* /*src*/,
                       std::vector<CopyUpdate*> > copies;
              for (std::set<Update*>::const_iterator it = 
                    fit->elements.begin(); it != fit->elements.end(); it++)
                (*it)->sort_updates(copies, reduces, fills);
              if (!fills.empty())
                issue_fills(uit->first, fills, group_precondition,
                            fit->set_mask, trace_info);
              if (!copies.empty())
                issue_copies(uit->first, copies, group_precondition, 
                             fit->set_mask, trace_info);
              if (!reduces.empty())
                issue_reductions(uit->first, reduces, group_precondition,
                                 fit->set_mask, trace_info);
            }
          }
        }
      } // iterate over dst instances
    }

    //--------------------------------------------------------------------------
    void CopyFillAggregator::issue_fills(InstanceView *target,
                                         const std::vector<FillUpdate*> &fills,
                                         ApEvent precondition, 
                                         const FieldMask &fill_mask,
                                         const PhysicalTraceInfo &trace_info)
    //--------------------------------------------------------------------------
    {
#ifdef DEBUG_LEGION
      assert(!fills.empty());
      assert(!!fill_mask); 
#endif
      std::vector<CopySrcDstField> dst_fields;
      target->copy_to(fill_mask, dst_fields, fills[0]->across_helper);
      const UniqueID op_id = op->get_unique_op_id();
      if (fills.size() == 1)
      {
#ifdef DEBUG_LEGION
        // Should cover all the fields
        assert(!(fill_mask - fills[0]->src_mask));
#endif
        IndexSpaceExpression *fill_expr = fills[0]->expr;
        FillView *fill_view = fills[0]->source;
        const ApEvent result = fill_expr->issue_fill(trace_info, dst_fields,
                                                     fill_view->value->value,
                                                   fill_view->value->value_size,
#ifdef LEGION_SPY
                                                     fill_view->fill_op_uid,
#endif
                                                     precondition);
        // Record the fill result in the destination 
        if (result.exists())
        {
          target->add_copy_user(false/*reading*/,
                                result, fill_mask, fill_expr,
                                op_id, index, effects, trace_info); 
          if (track_events)
            events.insert(result);
        }
      }
      else
      {
#ifdef DEBUG_LEGION
#ifndef NDEBUG
        // These should all have had the same across helper
        for (unsigned idx = 1; idx < fills.size(); idx++)
          assert(fills[idx]->across_helper == fills[0]->across_helper);
#endif
#endif
        std::map<FillView*,std::set<IndexSpaceExpression*> > exprs;
        for (std::vector<FillUpdate*>::const_iterator it = 
              fills.begin(); it != fills.end(); it++)
        {
#ifdef DEBUG_LEGION
          // Should cover all the fields
          assert(!(fill_mask - (*it)->src_mask));
#endif
          exprs[(*it)->source].insert((*it)->expr);
        }
        for (std::map<FillView*,std::set<IndexSpaceExpression*> >::
              const_iterator it = exprs.begin(); it != exprs.end(); it++)
        {
          IndexSpaceExpression *fill_expr = (it->second.size() == 1) ?
            *(it->second.begin()) : forest->union_index_spaces(it->second);
          const ApEvent result = fill_expr->issue_fill(trace_info, dst_fields,
                                                       it->first->value->value,
                                                  it->first->value->value_size,
#ifdef LEGION_SPY
                                                       it->first->fill_op_uid,
#endif
                                                       precondition);
          if (result.exists())
          {
            target->add_copy_user(false/*reading*/,
                                  result, fill_mask, fill_expr,
                                  op_id, index, effects, trace_info);
            if (track_events)
              events.insert(result);
          }
        }
      }
    }

    //--------------------------------------------------------------------------
    void CopyFillAggregator::issue_copies(InstanceView *target, 
                              const std::map<InstanceView*,
                                             std::vector<CopyUpdate*> > &copies,
                              ApEvent precondition, const FieldMask &copy_mask,
                              const PhysicalTraceInfo &trace_info)
    //--------------------------------------------------------------------------
    {
#ifdef DEBUG_LEGION
      assert(!copies.empty());
      assert(!!copy_mask);
#endif
      const UniqueID op_id = op->get_unique_op_id();
      for (std::map<InstanceView*,std::vector<CopyUpdate*> >::const_iterator
            cit = copies.begin(); cit != copies.end(); cit++)
      {
#ifdef DEBUG_LEGION
        assert(!cit->second.empty());
#endif
        std::vector<CopySrcDstField> dst_fields, src_fields;
        target->copy_to(copy_mask, dst_fields, cit->second[0]->across_helper);
        if (cit->second.size() == 1)
        {
          // Easy case of a single update copy
          CopyUpdate *update = cit->second[0];
#ifdef DEBUG_LEGION
          // Should cover all the fields
          assert(!(copy_mask - update->src_mask));
#endif
          InstanceView *source = update->source;
          source->copy_from(copy_mask, src_fields);
          IndexSpaceExpression *copy_expr = update->expr;
          const ApEvent result = copy_expr->issue_copy(trace_info, 
            dst_fields, src_fields, precondition, update->redop, false/*fold*/);
          if (result.exists())
          {
            source->add_copy_user(true/*reading*/,
                                  result, copy_mask, copy_expr,
                                  op_id, index, effects, trace_info);
            target->add_copy_user(false/*reading*/,
                                  result, copy_mask, copy_expr,
                                  op_id, index, effects, trace_info);
            if (track_events)
              events.insert(result);
          }
        }
        else
        {
          // Have to group by source instances in order to merge together
          // different index space expressions for the same copy
          std::map<InstanceView*,std::set<IndexSpaceExpression*> > src_exprs;
          const ReductionOpID redop = cit->second[0]->redop;
          for (std::vector<CopyUpdate*>::const_iterator it = 
                cit->second.begin(); it != cit->second.end(); it++)
          {
#ifdef DEBUG_LEGION
            // Should cover all the fields
            assert(!(copy_mask - (*it)->src_mask));
            // Should have the same redop
            assert(redop == (*it)->redop);
            // Should also have the same across helper as the first one
            assert(cit->second[0]->across_helper == (*it)->across_helper);
#endif
            src_exprs[(*it)->source].insert((*it)->expr);
          }
          for (std::map<InstanceView*,std::set<IndexSpaceExpression*> >::
                const_iterator it = src_exprs.begin(); 
                it != src_exprs.end(); it++)
          {
            IndexSpaceExpression *copy_expr = (it->second.size() == 1) ? 
              *(it->second.begin()) : forest->union_index_spaces(it->second);
            src_fields.clear();
            it->first->copy_from(copy_mask, src_fields);
            const ApEvent result = copy_expr->issue_copy(trace_info, 
              dst_fields, src_fields, precondition, redop, false/*fold*/);
            if (result.exists())
            {
              it->first->add_copy_user(true/*reading*/,
                                    result, copy_mask, copy_expr,
                                    op_id, index, effects, trace_info);
              target->add_copy_user(false/*reading*/,
                                    result, copy_mask, copy_expr,
                                    op_id, index, effects, trace_info);
              if (track_events)
                events.insert(result);
            }
          }
        }
      }
    }

    //--------------------------------------------------------------------------
    void CopyFillAggregator::issue_reductions(InstanceView *target,
                             const std::vector<ReduceUpdate*> &reduces,
                             ApEvent precondition, const FieldMask &reduce_mask,
                             const PhysicalTraceInfo &trace_info)
    //--------------------------------------------------------------------------
    {
      // We have to iterate through and find the reduction epochs for each
      // field that have to be issued, we'll keep doing this for all the 
      // reduction updates until we've issued all their reductions 
      std::vector<ReduceUpdateState> reduce_states(reduces.size());
      for (unsigned idx = 0; idx < reduces.size(); idx++)
      {
        ReduceUpdateState &state = reduce_states[idx];
        state.reduce = reduces[idx];
        state.current_index = 0;
        state.current_precondition = precondition;
      }
      const bool reduction_fold = target->is_reduction_view();
      const UniqueID op_id = op->get_unique_op_id();
      bool done = false;
      while (!done)
      {
        // Group by fields and reduction epochs
        std::map<std::pair<unsigned,ReductionOpID>,
                 std::vector<unsigned/*live index*/> > current_epochs;
        for (unsigned idx = 0; idx < reduce_states.size(); idx++)
        {
          const ReduceUpdateState &state = reduce_states[idx];
          // Skip any updates which we're aleady done for
          if (state.current_index == state.reduce->sources.size())
            continue;
          const ReductionOpID redop =
            state.reduce->sources[state.current_index]->get_redop();
          const std::pair<unsigned,ReductionOpID> key(
              state.reduce->src_fidx, redop);
          current_epochs[key].push_back(idx);
        }
        // Set this to true, if we have unfinished updates then we'll
        // set it back to false 
        done = true;
        for (std::map<std::pair<unsigned,ReductionOpID>,
                      std::vector<unsigned> >::const_iterator eit = 
              current_epochs.begin(); eit != current_epochs.end(); eit++)
        {
          FieldMask src_mask;
          src_mask.set_bit(eit->first.first);
          if (eit->second.size() == 1)
          {
            // Simple: just issue the reductions from this update
            ReduceUpdateState &state = reduce_states[eit->second[0]];
            ReduceUpdate *update = state.reduce;
            std::set<ApEvent> result_events;
            for (unsigned idx = state.current_index;
                  idx < update->sources.size(); idx++)
            {
              ReductionView *src_view = update->sources[idx];
              if (src_view->get_redop() != eit->first.second)
              {
                done = false;
                state.current_index = idx;
                break;
              }
              else
              {
                state.current_index = idx+1;
                // Issue the reduction from this view
                std::vector<CopySrcDstField> src_fields, dst_fields;
                src_view->reduce_from(eit->first.second, src_mask, src_fields);
                target->reduce_to(eit->first.second, src_mask, src_fields,
                                  update->across_helper); 
                ApEvent result = update->expr->issue_copy(trace_info,
                    src_fields, dst_fields, state.current_precondition, 
                    eit->first.second, reduction_fold); 
                if (result.exists())
                {
                  src_view->add_copy_user(true/*reading*/,
                                          result, src_mask, update->expr, 
                                          op_id, index, effects, trace_info);
                  result_events.insert(result);
                }
              }
            }
            // Update the precondition based on the done events
            if (!result_events.empty())
              state.current_precondition = 
                Runtime::merge_events(&trace_info, result_events);
          }
          else
          {
            // Harder: group by event preconditions and source instances
            // Then issue merged reductions for index space expressions
            std::map<std::pair<ApEvent,ReductionView*>,
                     std::vector<unsigned> > grouped_sources;
            for (std::vector<unsigned>::const_iterator sit = 
                  eit->second.begin(); sit != eit->second.end(); sit++)
            {
              ReduceUpdateState &state = 
                reduce_states[eit->second[*sit]];
              for (unsigned idx = state.current_index;
                    idx < state.reduce->sources.size(); idx++)
              {
                ReductionView *src_view = state.reduce->sources[idx];
                if (src_view->get_redop() != eit->first.second)
                {
                  done = false;
                  state.current_index = idx;
                  break;
                }
                else 
                {
                  state.current_index = idx+1;
                  const std::pair<ApEvent,ReductionView*> key(
                      state.current_precondition, src_view);
                  grouped_sources[key].push_back(*sit);
                }
              }
            }
            // Issue the merged reductions for each and record
            // the result events for each of the updates
            std::map<unsigned,std::set<ApEvent> > result_events;
            for (std::map<std::pair<ApEvent,ReductionView*>,
                          std::vector<unsigned> >::const_iterator git = 
                  grouped_sources.begin(); git != grouped_sources.end(); git++)
            {
#ifdef DEBUG_LEGION
              assert(!git->second.empty());
#endif
              const ReduceUpdateState &state = 
                reduce_states[git->second[0]];
              std::vector<CopySrcDstField> src_fields, dst_fields;
              git->first.second->reduce_from(eit->first.second, 
                                             src_mask, src_fields);
              target->reduce_to(eit->first.second, src_mask, src_fields,
                                state.reduce->across_helper); 
              IndexSpaceExpression *reduce_expr = NULL;
              if (git->second.size() > 1)
              {
                std::set<IndexSpaceExpression*> reduce_exprs;
                for (unsigned idx = 0; idx < git->second.size(); idx++)
                  reduce_exprs.insert(
                      reduce_states[git->second[idx]].reduce->expr);
                reduce_expr = forest->union_index_spaces(reduce_exprs);
              }
              else
                reduce_expr = state.reduce->expr; 
              ApEvent result = reduce_expr->issue_copy(trace_info,
                  src_fields, dst_fields, git->first.first, 
                  eit->first.second, reduction_fold);
              if (result.exists())
              {
                // Record the source user
                git->first.second->add_copy_user(true/*reading*/, result, 
                    src_mask, reduce_expr, op_id, index, effects, trace_info);
                for (unsigned idx = 0; idx < git->second.size(); idx++)
                  result_events[git->second[idx]].insert(result);
              }
            }
            // Update the event preconditions for each of the updates
            for (std::map<unsigned,std::set<ApEvent> >::const_iterator
                  it = result_events.begin(); it != result_events.end(); it++)
            {
              ReduceUpdateState &state = reduce_states[it->first]; 
              if (it->second.size() > 1)
                state.current_precondition = 
                  Runtime::merge_events(&trace_info, it->second);
              else
                state.current_precondition = *(it->second.begin());
            }
          }
        }
      }
      // Once we're done iterating, save out the copy results and 
      // record the done events if necessary
      std::map<ApEvent,std::vector<unsigned> > grouped_updates;
      for (unsigned idx = 0; idx < reduce_states.size(); idx++)
      {
        const ReduceUpdateState &state = reduce_states[idx];
#ifdef DEBUG_LEGION
        assert(state.current_precondition != precondition);
#endif
        grouped_updates[state.current_precondition].push_back(idx);
      }
      for (std::map<ApEvent,std::vector<unsigned> >::const_iterator git =
            grouped_updates.begin(); git != grouped_updates.end(); git++)
      {
        if (git->second.size() == 1)
        {
          ReduceUpdate *update = reduce_states[0].reduce;
          IndexSpaceExpression *reduce_expr = update->expr;
          FieldMask reduce_mask;
          reduce_mask.set_bit(update->dst_fidx);
          target->add_copy_user(false/*reading*/, git->first, reduce_mask, 
                                reduce_expr, op_id, index, effects, trace_info);
        }
        else
        {
          FieldMaskSet<IndexSpaceExpression> reduce_sets;
          for (std::vector<unsigned>::const_iterator sit = 
                git->second.begin(); sit != git->second.end(); sit++)
          {
            ReduceUpdate *update = reduce_states[*sit].reduce;
            IndexSpaceExpression *expr = update->expr;
            FieldMask dst_mask;
            dst_mask.set_bit(update->dst_fidx);
            FieldMaskSet<IndexSpaceExpression>::iterator finder = 
              reduce_sets.find(expr);
            if (finder != reduce_sets.end())
              finder.merge(dst_mask);
            else
              reduce_sets.insert(expr, dst_mask);
          }
          if (reduce_sets.size() == 1)
          {
            IndexSpaceExpression *reduce_expr = reduce_sets.begin()->first;
            const FieldMask &reduce_mask = reduce_sets.get_valid_mask();
            target->add_copy_user(false/*reading*/, git->first, reduce_mask, 
                            reduce_expr, op_id, index, effects, trace_info);
          }
          else
          {
            LegionList<FieldSet<IndexSpaceExpression*> >::aligned expr_sets;
            reduce_sets.compute_field_sets(FieldMask(), expr_sets);
            for (LegionList<FieldSet<IndexSpaceExpression*> >::aligned::
                  const_iterator it = expr_sets.begin(); 
                  it != expr_sets.end(); it++)
            {
              IndexSpaceExpression *reduce_expr = (it->elements.size() == 1) ?
                *(it->elements.begin()) : 
                forest->union_index_spaces(it->elements);
              target->add_copy_user(false/*reading*/, git->first, it->set_mask,
                               reduce_expr, op_id, index, effects, trace_info);
            }
          }
        }
        if (track_events)
          events.insert(git->first);
      }
    }

    /////////////////////////////////////////////////////////////
    // Equivalence Set
    /////////////////////////////////////////////////////////////

    // C++ is dumb
    const VersionID EquivalenceSet::init_version;

    //--------------------------------------------------------------------------
    EquivalenceSet::EquivalenceSet(Runtime *rt, DistributedID did,
                 AddressSpaceID owner, IndexSpaceExpression *expr, bool reg_now)
      : DistributedCollectable(rt, did, owner, reg_now), set_expr(expr),
        eq_lock(gc_lock), eq_state(is_owner() ? MAPPING_STATE : INVALID_STATE),
        unrefined_remainder(NULL)
    //--------------------------------------------------------------------------
    {
      set_expr->add_expression_reference();
      if (is_owner())
      {
        // If we're the owner, we hold everything in exclusive mode
        exclusive_fields = FieldMask(LEGION_FIELD_MASK_FIELD_ALL_ONES);
        exclusive_copies[owner_space] = exclusive_fields;
      }
    }

    //--------------------------------------------------------------------------
    EquivalenceSet::EquivalenceSet(const EquivalenceSet &rhs)
      : DistributedCollectable(rhs), set_expr(NULL), eq_lock(gc_lock)
    //--------------------------------------------------------------------------
    {
      // should never be called
      assert(false);
    }

    //--------------------------------------------------------------------------
    EquivalenceSet::~EquivalenceSet(void)
    //--------------------------------------------------------------------------
    {
      if (set_expr->remove_expression_reference())
        delete set_expr;
      if (!subsets.empty())
      {
        for (std::vector<EquivalenceSet*>::const_iterator it = 
              subsets.begin(); it != subsets.end(); it++)
          if ((*it)->remove_nested_resource_ref(did))
            delete (*it);
        subsets.clear();
      }
    }

    //--------------------------------------------------------------------------
    EquivalenceSet& EquivalenceSet::operator=(const EquivalenceSet &rhs)
    //--------------------------------------------------------------------------
    {
      // should never be called
      assert(false);
      return *this;
    }

    //--------------------------------------------------------------------------
    EquivalenceSet::RefinementThunk::RefinementThunk(IndexSpaceExpression *exp,
                                     EquivalenceSet *own, AddressSpaceID source)
      : owner(own), expr(exp), refinement(NULL)
    //--------------------------------------------------------------------------
    {
      Runtime *runtime = owner->runtime;
      // If we're not the owner send a request to make the refinement set
      if (source != runtime->address_space)
      {
        refinement_ready = Runtime::create_rt_user_event();
        Serializer rez;
        {
          RezCheck z(rez);
          rez.serialize(this);
          expr->pack_expression(rez, source);
        }
        runtime->send_equivalence_set_create_remote_request(source, rez);
      }
      else
      {
        // We're the owner so we can make the refinement set now
        DistributedID did = runtime->get_available_distributed_id();
        refinement = new EquivalenceSet(runtime, did, source, 
                                        expr, true/*register*/);
      }
    }

    //--------------------------------------------------------------------------
    EquivalenceSet* EquivalenceSet::RefinementThunk::perform_refinement(void)
    //--------------------------------------------------------------------------
    {
      if (refinement_ready.exists() && !refinement_ready.has_triggered())
        refinement_ready.wait();
#ifdef DEBUG_LEGION
      assert(refinement != NULL);
#endif
      refinement->clone_from(owner);
      return refinement;
    }

    //--------------------------------------------------------------------------
    EquivalenceSet* EquivalenceSet::RefinementThunk::get_refinement(void)
    //--------------------------------------------------------------------------
    {
      if (refinement_ready.exists() && !refinement_ready.has_triggered())
        refinement_ready.wait();
#ifdef DEBUG_LEGION
      assert(refinement != NULL);
#endif
      return refinement;
    }

    //--------------------------------------------------------------------------
    void EquivalenceSet::RefinementThunk::record_refinement(
                                          EquivalenceSet *result, RtEvent ready)
    //--------------------------------------------------------------------------
    {
#ifdef DEBUG_LEGION
      assert(refinement == NULL);
      assert(refinement_ready.exists());
      assert(!refinement_ready.has_triggered());
#endif
      refinement = result;
      Runtime::trigger_event(refinement_ready, ready);
    }

    //--------------------------------------------------------------------------
    EquivalenceSet::LocalRefinement::LocalRefinement(IndexSpaceExpression *expr, 
                                                     EquivalenceSet *owner) 
      : RefinementThunk(expr, owner, owner->runtime->address_space)
    //--------------------------------------------------------------------------
    {
    }

    //--------------------------------------------------------------------------
    EquivalenceSet::RemoteComplete::RemoteComplete(IndexSpaceExpression *expr, 
                                   EquivalenceSet *owner, AddressSpaceID source)
      : RefinementThunk(expr, owner, source), target(source)
    //--------------------------------------------------------------------------
    {
    }

    //--------------------------------------------------------------------------
    EquivalenceSet::RemoteComplete::~RemoteComplete(void)
    //--------------------------------------------------------------------------
    {
      // We already hold the lock from the caller
      owner->process_subset_request(target, false/*need lock*/);
    }

    //--------------------------------------------------------------------------
    void EquivalenceSet::notify_active(ReferenceMutator *mutator)
    //--------------------------------------------------------------------------
    {
      // should never be called
      assert(false);
    }

    //--------------------------------------------------------------------------
    void EquivalenceSet::notify_inactive(ReferenceMutator *mutator)
    //--------------------------------------------------------------------------
    {
      // should never be called
      assert(false);
    }

    //--------------------------------------------------------------------------
    void EquivalenceSet::notify_valid(ReferenceMutator *mutator)
    //--------------------------------------------------------------------------
    {
      // should never be called
      assert(false);
    }

    //--------------------------------------------------------------------------
    void EquivalenceSet::notify_invalid(ReferenceMutator *mutator)
    //--------------------------------------------------------------------------
    {
      // should never be called
      assert(false);
    } 

    //--------------------------------------------------------------------------
    void EquivalenceSet::clone_from(EquivalenceSet *parent)
    //--------------------------------------------------------------------------
    {
      // No need to hold a lock on the parent since we know that
      // no one else will be modifying these data structures
      this->valid_instances = parent->valid_instances;
      this->reduction_instances = parent->reduction_instances;
      this->reduction_fields = parent->reduction_fields;
      this->restricted_instances = parent->restricted_instances;
      this->restricted_fields = parent->restricted_fields;
      this->version_numbers = parent->version_numbers;
      // Now add references to all the views
      // No need for a mutator here since all the views already
      // have valid references being held by the parent equivalence set
      if (!valid_instances.empty())
      {
        for (FieldMaskSet<LogicalView>::const_iterator it =
              valid_instances.begin(); it != valid_instances.end(); it++)
          it->first->add_nested_valid_ref(did);
      }
      if (!reduction_instances.empty())
      {
        for (std::map<unsigned,std::vector<ReductionView*> >::const_iterator
              rit = reduction_instances.begin(); 
              rit != reduction_instances.end(); rit++)
        {
          for (std::vector<ReductionView*>::const_iterator it = 
                rit->second.begin(); it != rit->second.end(); it++)
            (*it)->add_nested_valid_ref(did);
        }
      }
      if (!restricted_instances.empty())
      {
        for (FieldMaskSet<InstanceView>::const_iterator it = 
              restricted_instances.begin(); it != 
              restricted_instances.end(); it++)
          it->first->add_nested_valid_ref(did);
      }
    }

    //--------------------------------------------------------------------------
    void EquivalenceSet::add_mapping_guard(RtEvent mapped_event)
    //--------------------------------------------------------------------------
    {
#ifdef DEBUG_LEGION
      if (is_owner())
        assert((eq_state == MAPPING_STATE) || 
                (eq_state == PENDING_REFINED_STATE));
      else
        assert((eq_state == VALID_STATE) ||
                (eq_state == PENDING_INVALID_STATE));
      assert(subsets.empty()); // should not have any subsets at this point
      assert(unrefined_remainder == NULL); // nor a remainder
#endif
      std::map<RtEvent,unsigned>::iterator finder = 
        mapping_guards.find(mapped_event);
      if (finder == mapping_guards.end())
        mapping_guards[mapped_event] = 1;
      else
        finder->second++;
    }

    //--------------------------------------------------------------------------
    void EquivalenceSet::remove_mapping_guard(RtEvent mapped_event)
    //--------------------------------------------------------------------------
    {
      AutoLock eq(eq_lock);
      std::map<RtEvent,unsigned>::iterator finder = 
        mapping_guards.find(mapped_event);
#ifdef DEBUG_LEGION
      if (is_owner())
        assert((eq_state == MAPPING_STATE) || 
                (eq_state == PENDING_REFINED_STATE));
      else
        assert((eq_state == VALID_STATE) ||
                (eq_state == PENDING_INVALID_STATE));
      assert(finder != mapping_guards.end());
      assert(finder->second > 0);
#endif
      finder->second--;
      if (finder->second == 0)
      {
        mapping_guards.erase(finder);
        if (mapping_guards.empty())
        {
          if (is_owner() && (eq_state == PENDING_REFINED_STATE))
          {
            eq_state = REFINED_STATE;
            // Kick off the refinement task now that we're transitioning
            launch_refinement_task();
          }
          else if (!is_owner() && (eq_state == PENDING_INVALID_STATE))
          {
            eq_state = INVALID_STATE;
#ifdef DEBUG_LEGION
            assert(transition_event.exists());
#endif
            invalidate_remote_state(transition_event);
            transition_event = RtUserEvent::NO_RT_USER_EVENT;
          }
        }
      }
    }

    //--------------------------------------------------------------------------
    RtEvent EquivalenceSet::ray_trace_equivalence_sets(VersionManager *target,
                                                     IndexSpaceExpression *expr,
                                                     AddressSpaceID source) 
    //--------------------------------------------------------------------------
    {
      // If this is not the owner node then send the request there
      if (!is_owner())
      {
        RtUserEvent done_event = Runtime::create_rt_user_event();
        Serializer rez;
        {
          RezCheck z(rez);
          rez.serialize(did);
          rez.serialize(target);
          expr->pack_expression(rez, owner_space);
          rez.serialize(source);
          rez.serialize(done_event);
        }
        runtime->send_equivalence_set_ray_trace_request(owner_space, rez);
        return done_event;
      }
#ifdef DEBUG_LEGION
      assert(expr != NULL);
#endif
      // At this point we're on the owner
      std::map<EquivalenceSet*,IndexSpaceExpression*> to_traverse;
      std::map<RefinementThunk*,IndexSpaceExpression*> refinements_to_traverse;
      RtEvent refinement_done;
      bool record_self = false;
      {
        RegionTreeForest *forest = runtime->forest;
        AutoLock eq(eq_lock);
        // Ray tracing can run as long as we don't have an outstanding
        // refining task that we need to wait for
        while (eq_state == REFINING_STATE)
        {
          if (!transition_event.exists())
            transition_event = Runtime::create_rt_user_event();
          RtEvent wait_on = transition_event;
          eq.release();
          wait_on.wait();
          eq.reacquire();
        }
        // Two cases here, one where refinement has already begun and
        // one where it is just starting
        if (!subsets.empty() || !pending_refinements.empty())
        {
          // Iterate through all the subsets and find any overlapping ones
          // that we need to traverse in order to do our subsets
          bool is_empty = false;
          for (std::vector<EquivalenceSet*>::const_iterator it = 
                subsets.begin(); it != subsets.end(); it++)
          {
            IndexSpaceExpression *overlap = 
              forest->intersect_index_spaces((*it)->set_expr, expr);
            if (overlap->is_empty())
              continue;
            to_traverse[*it] = overlap;
            expr = forest->subtract_index_spaces(expr, overlap);
            if ((expr == NULL) || expr->is_empty())
            {
              is_empty = true;
              break;
            }
          }
          for (std::vector<RefinementThunk*>::const_iterator it = 
                pending_refinements.begin(); !is_empty && (it != 
                pending_refinements.end()); it++)
          {
            IndexSpaceExpression *overlap = 
              forest->intersect_index_spaces((*it)->expr, expr);
            if (overlap->is_empty())
              continue;
            (*it)->add_reference();
            refinements_to_traverse[*it] = overlap;
            // If this is a pending refinement then we'll need to
            // wait for it before traversing farther
            if (!refinement_done.exists())
            {
#ifdef DEBUG_LEGION
              assert(eq_state == PENDING_REFINED_STATE);
#endif
              if (!transition_event.exists())
                transition_event = Runtime::create_rt_user_event();
              refinement_done = transition_event;
            }
            expr = forest->subtract_index_spaces(expr, overlap);
            if ((expr == NULL) || expr->is_empty())
              is_empty = true;
          }
          if (!is_empty)
          {
#ifdef DEBUG_LEGION
            assert(unrefined_remainder != NULL);
#endif
            LocalRefinement *refinement = new LocalRefinement(expr, this);
            refinement->add_reference();
            refinements_to_traverse[refinement] = expr;
            add_pending_refinement(refinement);
            // If this is a pending refinement then we'll need to
            // wait for it before traversing farther
            if (!refinement_done.exists())
            {
#ifdef DEBUG_LEGION
              assert((eq_state == PENDING_REFINED_STATE) ||
                     (eq_state == REFINING_STATE));
#endif
              if (!transition_event.exists())
                transition_event = Runtime::create_rt_user_event();
              refinement_done = transition_event;
            }
            unrefined_remainder = 
              forest->subtract_index_spaces(unrefined_remainder, expr);
            if ((unrefined_remainder != NULL) && 
                  unrefined_remainder->is_empty())
              unrefined_remainder = NULL;
          }
        }
        else
        {
#ifdef DEBUG_LEGION
          assert(unrefined_remainder == NULL);
#endif
          // See if the set expressions are the same or whether we 
          // need to make a refinement
          IndexSpaceExpression *diff = 
            forest->subtract_index_spaces(set_expr, expr);
          if ((diff != NULL) && !diff->is_empty())
          {
            // Time to refine this since we only need a subset of it
            LocalRefinement *refinement = new LocalRefinement(expr, this);
            refinement->add_reference();
            refinements_to_traverse[refinement] = expr;
            add_pending_refinement(refinement);
#ifdef DEBUG_LEGION
            assert((eq_state == PENDING_REFINED_STATE) ||
                   (eq_state == REFINING_STATE));
#endif
            if (!transition_event.exists())
              transition_event = Runtime::create_rt_user_event();
            refinement_done = transition_event;
            // Update the unrefined remainder
            unrefined_remainder = diff;
          }
          else
          {
            // Just record this as one of the results
            if (source != runtime->address_space)
            {
              // Not local so we need to send a message
              RtUserEvent recorded_event = Runtime::create_rt_user_event();
              Serializer rez;
              {
                RezCheck z(rez);
                rez.serialize(did);
                rez.serialize(target);
                rez.serialize(recorded_event);
              }
              runtime->send_equivalence_set_ray_trace_response(source, rez);
              return recorded_event;
            }
            else // Local so we can update this directly
              record_self = true;
          }
        }
      }
      if (record_self)
        target->record_equivalence_set(this);
      // If we have a refinement to do then we need to wait for that
      // to be done before we continue our traversal
      if (refinement_done.exists() && !refinement_done.has_triggered())
        refinement_done.wait();
      // Get the actual equivalence sets for any refinements we needed to
      // wait for because they weren't ready earlier
      if (!refinements_to_traverse.empty())
      {
        for (std::map<RefinementThunk*,IndexSpaceExpression*>::const_iterator
              it = refinements_to_traverse.begin(); 
              it != refinements_to_traverse.end(); it++)
        {
          EquivalenceSet *result = it->first->get_refinement();
#ifdef DEBUG_LEGION
          assert(to_traverse.find(result) == to_traverse.end());
#endif
          to_traverse[result] = it->second;
          if (it->first->remove_reference())
            delete it->first;
        }
      }
      // Finally traverse any subsets that we have to do
      if (!to_traverse.empty())
      {
        std::set<RtEvent> done_events;
        for (std::map<EquivalenceSet*,IndexSpaceExpression*>::const_iterator 
              it = to_traverse.begin(); it != to_traverse.end(); it++)
        {
          RtEvent done = it->first->ray_trace_equivalence_sets(target, 
                                                  it->second, source);
          if (done.exists())
            done_events.insert(done);
        }
        if (!done_events.empty())
          return Runtime::merge_events(done_events);
        // Otherwise fall through and return a no-event
      }
      return RtEvent::NO_RT_EVENT;
    }

    //--------------------------------------------------------------------------
    void EquivalenceSet::perform_versioning_analysis(VersionInfo &version_info)
    //--------------------------------------------------------------------------
    {
      std::vector<EquivalenceSet*> to_recurse;
      {
        AutoLock eq(eq_lock);
        // We need to iterate until we get a valid lease while holding the lock
        if (is_owner())
        {
          // If we have a unrefined remainder then we need that now
          if (unrefined_remainder != NULL)
          {
            LocalRefinement *refinement = 
              new LocalRefinement(unrefined_remainder, this);
            // We can clear the unrefined remainder now
            unrefined_remainder = NULL;
            add_pending_refinement(refinement);
          }
          // Wait until all the refinements are done before going on
          // to the next step
          while ((eq_state == REFINING_STATE) ||
                  !pending_refinements.empty())
          {
            if (!transition_event.exists())
              transition_event = Runtime::create_rt_user_event();
            RtEvent wait_on = transition_event;
            eq.release();
            wait_on.wait();
            eq.reacquire();
          }
        }
        else
        {
          // We're not the owner so see if we need to request a mapping state
          while ((eq_state != VALID_STATE) && 
                  (eq_state != PENDING_INVALID_STATE))
          {
            bool send_request = false;
            if (eq_state == INVALID_STATE)
            {
#ifdef DEBUG_LEGION
              assert(!transition_event.exists());
#endif
              transition_event = Runtime::create_rt_user_event(); 
              eq_state = PENDING_VALID_STATE;
              // Send the request for the update
              send_request = true;
            }
#ifdef DEBUG_LEGION
            assert(transition_event.exists());
#endif
            RtEvent wait_on = transition_event;
            eq.release();
            if (send_request)
            {
              Serializer rez;
              {
                RezCheck z(rez);
                rez.serialize(did);
              }
              runtime->send_equivalence_set_subset_request(owner_space, rez);
            }
            wait_on.wait();
            eq.reacquire();
          }
        }
        // If we have subsets then we're going to need to recurse and
        // traverse those as well since we need to get to the leaves
        if (!subsets.empty())
          // Our set of subsets are now what we need
          to_recurse = subsets;
        else // We're going to record ourself so add the mapping guard
          add_mapping_guard(version_info.get_guard_event());
      }
      // If we have subsets then continue the traversal
      if (!to_recurse.empty())
      {
        for (std::vector<EquivalenceSet*>::const_iterator it = 
              to_recurse.begin(); it != to_recurse.end(); it++)
          (*it)->perform_versioning_analysis(version_info);
      }
      else // Otherwise we can record ourselves
        version_info.record_equivalence_set(this);
    }

    //--------------------------------------------------------------------------
    void EquivalenceSet::request_valid_copy(FieldMask request_mask,
                                            const RegionUsage usage,
                                            std::set<RtEvent> &ready_events, 
                                            std::set<RtEvent> &applied_events,
                                            AddressSpaceID request_space,
                                            PendingRequest *pending_request)
    //--------------------------------------------------------------------------
    {
      AutoLock eq(eq_lock);
#ifdef DEBUG_LEGION
      sanity_check();
#endif
      if (!is_owner())
      {
#ifdef DEBUG_LEGION
        assert(pending_request == NULL);
#endif
        // Check to see which fields we need to request
        if (IS_REDUCE(usage))
        {
#ifdef DEBUG_LEGION
          assert(usage.redop != 0);
#endif
          // Check to see if we are single or multiple reduce with
          // the right reduction operator
          FieldMask valid_mask = 
                      request_mask & (single_redop_fields | multi_redop_fields);
          if (!!valid_mask)
          {
            LegionMap<ReductionOpID,FieldMask>::aligned::const_iterator
              finder = redop_modes.find(usage.redop);
            if (finder != redop_modes.end())
            {
              valid_mask &= finder->second;
              if (!!valid_mask)
                request_mask -= valid_mask;
            }
          }
        }
        else if (IS_READ_ONLY(usage))
        {
          // For read-only we'll be happy with exclusive or read-only
          const FieldMask valid_mask = 
                        request_mask & (exclusive_fields | shared_fields);
          if (!!valid_mask)
            request_mask -= valid_mask;
        }
        else
        {
          // for write privileges then we need to be exclusive
          const FieldMask valid_mask = request_mask & exclusive_fields;
          if (!!valid_mask)
            request_mask -= valid_mask;
        }
        // If not all our fields are valid send a request to the owner
        if (!!request_mask)
        {
          // Check to see if there are already any pending requests
          // that we need to check against
          if (!outstanding_requests.empty() &&
              !(request_mask * outstanding_requests.get_valid_mask()))
          {
            for (FieldMaskSet<PendingRequest>::const_iterator it = 
                  outstanding_requests.begin(); it != 
                  outstanding_requests.end(); it++)
            {
              const FieldMask overlap = request_mask & it->second;
              if (!overlap)
                continue;
              ready_events.insert(it->first->ready_event);
              applied_events.insert(it->first->applied_event);
              request_mask -= overlap;
              if (!request_mask)
                break;
            }
          }
          // If we still have fields to request then issue our request
          if (!!request_mask)
          {
            RtUserEvent ready = Runtime::create_rt_user_event();
            RtUserEvent applied = Runtime::create_rt_user_event(); 
            PendingRequest *request = 
              new PendingRequest(ready, applied, usage.redop);
            Serializer rez;
            {
              RezCheck z(rez);
              rez.serialize(did);
              rez.serialize(request_mask);
              rez.serialize(usage);
              rez.serialize(request);
            }
            runtime->send_equivalence_set_valid_request(owner_space, rez);
            outstanding_requests.insert(request, request_mask);
            ready_events.insert(ready);
            applied_events.insert(applied);
          }
        }
      }
      else
      {
        // If the request is local then check to see which fields are already
        // valid, if its remote this check was already done on the remote node
        const FieldMask orig_mask = request_mask;
        if (request_space == owner_space)
        {
          if (!outstanding_requests.empty() && 
              !(request_mask * outstanding_requests.get_valid_mask()))
          {
            for (FieldMaskSet<PendingRequest>::const_iterator it = 
                  outstanding_requests.begin(); it != 
                  outstanding_requests.end(); it++)
            {
              const FieldMask overlap = request_mask & it->second;
              if (!overlap)
                continue;
              ready_events.insert(it->first->ready_event);
              applied_events.insert(it->first->applied_event);
              request_mask -= overlap;
              if (!request_mask)
                break;
            }
          }
          // First see which ones we already have outstanding requests for
          if (!!request_mask)
          {
            if (IS_REDUCE(usage))
            {
              // Check to see if we have it in a reduction mode
              FieldMask redop_mask;
              if (!(request_mask * single_redop_fields))
              {
                LegionMap<AddressSpaceID,FieldMask>::aligned::const_iterator
                  finder = single_reduction_copies.find(owner_space);
                if (finder != single_reduction_copies.end())
                  redop_mask |= finder->second & request_mask;
              }
              if (!(request_mask * multi_redop_fields))
              {
                LegionMap<AddressSpaceID,FieldMask>::aligned::const_iterator
                  finder = multi_reduction_copies.find(owner_space);
                if (finder != multi_reduction_copies.end())
                  redop_mask |= finder->second & request_mask;
              }
              if (!!redop_mask)
              {
                // Has to be in the right reduction mode too
                LegionMap<ReductionOpID,FieldMask>::aligned::const_iterator
                  redop_finder = redop_modes.find(usage.redop);
                if (redop_finder != redop_modes.end())
                  request_mask -= (redop_finder->second & redop_mask);
              }
            }
            else 
            {
              // Check to see if we have it in exclusive mode
              if (!(request_mask * exclusive_fields))
              {
                LegionMap<AddressSpaceID,FieldMask>::aligned::const_iterator
                  finder = exclusive_copies.find(owner_space);
                if (finder != exclusive_copies.end())
                  request_mask -= finder->second;
              }
              // Read-only can also be in shared mode
              if (IS_READ_ONLY(usage) && !!request_mask)
              {
                LegionMap<AddressSpaceID,FieldMask>::aligned::const_iterator
                  finder = shared_copies.find(owner_space);
                if (finder != shared_copies.end())
                  request_mask -= finder->second;
              }
            }
            if (!!request_mask)
            {
#ifdef DEBUG_LEGION
              assert(pending_request == NULL);
#endif
              RtUserEvent ready = Runtime::create_rt_user_event();
              RtUserEvent applied = Runtime::create_rt_user_event(); 
              pending_request = 
                new PendingRequest(ready, applied, usage.redop);
              outstanding_requests.insert(pending_request, request_mask); 
            }
          }
        }
        // If we still have fields that aren't valid then we need to 
        // send messages to remote nodes for updates and invalidations
        if (!!request_mask)
        {
#ifdef DEBUG_LEGION
          assert(pending_request != NULL);
#endif
          unsigned pending_updates = 0;
          if (IS_REDUCE(usage))
          {
            FieldMask same_redop_mask;
            // If we're in single or multi reduce mode with the same reduction 
            // operation then we can go to multi-reduce mode for the requester
            LegionMap<ReductionOpID,FieldMask>::aligned::const_iterator 
              redop_finder = redop_modes.find(usage.redop);
            if (redop_finder != redop_modes.end())
            {
              same_redop_mask = redop_finder->second & request_mask;
              if (!!same_redop_mask)
              {
                // Remove these fields from the request mask
                request_mask -= same_redop_mask;
                update_reduction_copies(same_redop_mask, request_space, 
                        pending_request, pending_updates, usage.redop);
              }
            }
            // See if we still have request fields
            if (!!request_mask)
            {
              FieldMask filter_mask = request_mask;
              // No matter what mode we're in now we're going to single-reduce
              // Issue updates from all exclusive and reduction copies
              // and send invalidations to any shared copies
              // Keep track of which fields we've issue updates from for
              // when we get to any shared nodes in case we need to issue
              // an update from one of them as well
              filter_single_copies(filter_mask, exclusive_fields, 
                                   exclusive_copies, request_space, 
                                   pending_request, pending_updates);
              filter_multi_copies(filter_mask, shared_fields, shared_copies,
                            request_space, pending_request, pending_updates);
              const FieldMask redop_overlap = filter_mask & 
                                  (single_redop_fields | multi_redop_fields);
              if (!!redop_overlap)
              {
                filter_single_copies(filter_mask, single_redop_fields, 
                    single_reduction_copies, request_space, 
                    pending_request, pending_updates);
                filter_multi_copies(filter_mask, multi_redop_fields,
                    multi_reduction_copies, request_space,
                    pending_request, pending_updates);
                filter_redop_modes(redop_overlap);
              }
#ifdef DEBUG_LEGION
              assert(!filter_mask); // should have seen them all
#endif
              // Now put everything in single reduce mode
              record_single_reduce(request_space, request_mask, usage.redop); 
            }
          }
          else
          {
            // Any reduction modes are going to exclusive regardless of
            // whether we are reading or writing 
            const FieldMask redop_overlap = request_mask &
                              (single_redop_fields | multi_redop_fields);
            if (!!redop_overlap)
            {
              FieldMask filter_mask = redop_overlap;
              filter_single_copies(filter_mask, single_redop_fields,
                                   single_reduction_copies, request_space,
                                   pending_request, pending_updates);
              filter_multi_copies(filter_mask, multi_redop_fields,
                                  multi_reduction_copies, request_space,
                                  pending_request, pending_updates);
              record_exclusive_copy(request_space, redop_overlap);
              request_mask -= redop_overlap;
            }
            if (!!request_mask)
            {
              if (IS_READ_ONLY(usage))
              {
                // Filter anything in exclusive mode going to shared mode
                filter_single_copies(request_mask, exclusive_fields,
                                     exclusive_copies, request_space,
                                     pending_request, pending_updates);
                // If we still have fields we can record a new shared user
                if (!!request_mask)
                  record_shared_copy(request_space, request_mask);
              }
              else
              {
                // Must be in shared or exclusive mode going to exclusive mode
                const FieldMask shared_overlap = shared_fields & request_mask;
                if (!!shared_overlap)
                {
                  filter_multi_copies(request_mask, shared_fields,shared_copies,
                              request_space, pending_request, pending_updates);
                  record_exclusive_copy(request_space, shared_overlap);
                }
                if (!!request_mask)
                  update_exclusive_copies(request_mask, request_space,
                                          pending_request, pending_updates);
              }
            }
          }
          // Now either make a new request if this was from the owner
          // or send back an update message to the requester with
          // the expected number of updates
          if (request_space != local_space)
          {
            Serializer rez;
            {
              RezCheck z(rez);
              rez.serialize(did);
              rez.serialize(pending_request);
              rez.serialize(pending_updates);
              // Since we're sening this back to a remote node then
              // we need to determine the owner mask
              FieldMask owner_mask;
              if (usage.redop > 0)
              {
                LegionMap<AddressSpaceID,FieldMask>::aligned::const_iterator
                  finder = single_reduction_copies.find(request_space);
                if (finder != single_reduction_copies.end())
                  owner_mask = finder->second & orig_mask;
              }
              else
              {
                LegionMap<AddressSpaceID,FieldMask>::aligned::const_iterator
                  finder = exclusive_copies.find(request_space);
                if (finder != exclusive_copies.end())
                  owner_mask = finder->second & orig_mask;
              }
              rez.serialize(owner_mask);
            }
            runtime->send_equivalence_set_valid_response(request_space, rez);
          }
          else
            record_pending_updates(pending_request, pending_updates, false);
        }
      }
#ifdef DEBUG_LEGION
      sanity_check();
#endif
    }

    //--------------------------------------------------------------------------
    void EquivalenceSet::update_exclusive_copies(FieldMask &to_update,
                                               AddressSpaceID request_space,
                                               PendingRequest *pending_request,
                                               unsigned &pending_updates)
    //--------------------------------------------------------------------------
    {
#ifdef DEBUG_LEGION
      assert(!(to_update - exclusive_fields)); // should all be exclusive
#endif
      // Scan through the exclusive copies, send any updates
      FieldMask to_add;
      std::vector<AddressSpaceID> to_delete;
      for (LegionMap<AddressSpaceID,FieldMask>::aligned::iterator it =
            exclusive_copies.begin(); it != exclusive_copies.end(); it++)
      {
        const FieldMask overlap = to_update & it->second;
        if (!overlap)
          continue;
        // If it's already valid then there's nothing to do
        if (it->first == request_space)
          continue;
        request_update(it->first, request_space, overlap, 
                       pending_request, pending_updates, true/*invalidate*/);
        to_add |= overlap;
        it->second -= overlap;
        if (!it->second)
          to_delete.push_back(it->first);
        to_update -= overlap;
        if (!to_update)
          break;
      }
      if (!to_delete.empty())
      {
        for (std::vector<AddressSpaceID>::const_iterator it = 
              to_delete.begin(); it != to_delete.end(); it++)
          exclusive_copies.erase(*it);
      }
      if (!!to_add)
      {
        LegionMap<AddressSpaceID,FieldMask>::aligned::iterator finder = 
          exclusive_copies.find(request_space);
        if (finder == exclusive_copies.end())
          exclusive_copies[request_space] = to_add;
        else
          finder->second |= to_add;
      }
    }

    //--------------------------------------------------------------------------
    void EquivalenceSet::update_reduction_copies(FieldMask &to_update,
                                                AddressSpaceID request_space,
                                                PendingRequest *pending_request,
                                                unsigned &pending_updates,
                                                ReductionOpID redop)
    //--------------------------------------------------------------------------
    {
#ifdef DEBUG_LEGION
      assert(redop != 0);
#endif
      // Check to the multi-case first
      FieldMask multi_overlap = to_update & multi_redop_fields;
      if (!!multi_overlap)
      {
        to_update -= multi_overlap;
        // Add it to the set, none of the fields should be valid currently
        LegionMap<AddressSpaceID,FieldMask>::aligned::iterator finder = 
          multi_reduction_copies.find(request_space);
        if (finder != multi_reduction_copies.end())
        {
#ifdef DEBUG_LEGION
          assert(finder->second * multi_overlap);
#endif
          finder->second |= multi_overlap; 
        }
        else
          multi_reduction_copies[request_space] = multi_overlap;
        // Request updates for all the fields
        for (LegionMap<AddressSpaceID,FieldMask>::aligned::const_iterator
              it = multi_reduction_copies.begin(); 
              it != multi_reduction_copies.end(); it++)
        {
          if (it->first == request_space)
            continue;
          const FieldMask overlap = multi_overlap & it->second;
          if (!overlap)
            continue;
          request_update(it->first, request_space, overlap, pending_request,
                         pending_updates, false/*invalidate*/, redop);
          multi_overlap -= overlap;
          if (!multi_overlap)
            break;
        }
      }
      // If we still have fields left then do the single case
      if (!!to_update)
      {
#ifdef DEBUG_LEGION
        assert(!(to_update - single_redop_fields));
#endif
        // All these fields are about to become multi-reduction fields
        single_redop_fields -= to_update;
        multi_redop_fields |= to_update;
        // Add it to the set, none of the fields should be valid currently
        LegionMap<AddressSpaceID,FieldMask>::aligned::iterator finder = 
          multi_reduction_copies.find(request_space);
        if (finder != multi_reduction_copies.end())
        {
#ifdef DEBUG_LEGION
          assert(finder->second * to_update);
#endif
          finder->second |= to_update; 
        }
        else
          multi_reduction_copies[request_space] = to_update;
        // Filter any single reduction nodes to multi-nodes
        std::vector<AddressSpaceID> to_delete;
        for (LegionMap<AddressSpaceID,FieldMask>::aligned::iterator it =
              single_reduction_copies.begin(); it != 
              single_reduction_copies.end(); it++)
        {
          const FieldMask overlap = to_update & it->second;
          if (!overlap)
            continue;
#ifdef DEBUG_LEGION
          assert(it->first != request_space);
#endif
          request_update(it->first, request_space, overlap, pending_request, 
                         pending_updates, false/*invalidate*/, redop);
          it->second -= overlap;
          if (!it->second)
            to_delete.push_back(it->first);
          // Move it to multi reduction
          LegionMap<AddressSpaceID,FieldMask>::aligned::iterator finder = 
            multi_reduction_copies.find(it->first);
          if (finder != multi_reduction_copies.end())
          {
#ifdef DEBUG_LEGION
            assert(finder->second * overlap);
#endif
            finder->second |= overlap;
          }
          else
            multi_reduction_copies[it->first] = overlap;
          // Fields should only appear once for single reductions
          to_update -= overlap;
          if (!to_update)
            break;
        }
      }
    }

    //--------------------------------------------------------------------------
    void EquivalenceSet::record_exclusive_copy(AddressSpaceID request_space,
                                               const FieldMask &request_mask)
    //--------------------------------------------------------------------------
    {
      exclusive_copies[request_space] |= request_mask; 
      exclusive_fields |= request_mask;
    }

    //--------------------------------------------------------------------------
    void EquivalenceSet::record_shared_copy(AddressSpaceID request_space,
                                            const FieldMask &request_mask)
    //--------------------------------------------------------------------------
    {
      shared_copies[request_space] |= request_mask;
      shared_fields |= request_mask;
    }

    //--------------------------------------------------------------------------
    void EquivalenceSet::record_single_reduce(AddressSpaceID request_space,
                                              const FieldMask &request_mask,
                                              ReductionOpID redop)
    //--------------------------------------------------------------------------
    {
      single_reduction_copies[request_space] |= request_mask;
      single_redop_fields |= request_mask;
      redop_modes[redop] |= request_mask;
    }

    //--------------------------------------------------------------------------
    void EquivalenceSet::filter_single_copies(FieldMask &to_filter, 
                                              FieldMask &single_fields,
              LegionMap<AddressSpaceID,FieldMask>::aligned &single_copies,
                                              AddressSpaceID request_space,
                                              PendingRequest *pending_request,
                                              unsigned &pending_updates)
    //--------------------------------------------------------------------------
    {
      if (!to_filter)
        return;
      FieldMask single_overlap = to_filter & single_fields;
      if (!single_overlap)
        return;
      single_fields -= single_overlap;
      to_filter -= single_overlap;
      std::vector<AddressSpaceID> to_delete;
      for (LegionMap<AddressSpaceID,FieldMask>::aligned::iterator it = 
            single_copies.begin(); it != single_copies.end(); it++)
      {
        const FieldMask overlap = single_overlap & it->second;
        if (!overlap)
          continue;
        if (it->first == request_space)
          // Just send an invalidate, when the response goes back
          // it will restore it to being valid
          request_invalidate(request_space, request_space, overlap, 
              pending_request, pending_updates, true/*meta only*/);
        else
          request_update(it->first, request_space, overlap, pending_request, 
                         pending_updates, true/*invalidate*/);
        it->second -= overlap;
        if (!it->second)
          to_delete.push_back(it->first);
        single_overlap -= overlap;
        if (!single_overlap)
          break;
      }
      if (!to_delete.empty())
      {
        for (std::vector<AddressSpaceID>::const_iterator it = 
              to_delete.begin(); it != to_delete.end(); it++)
          single_copies.erase(*it);
      }
    }

    //--------------------------------------------------------------------------
    void EquivalenceSet::filter_multi_copies(FieldMask &to_filter,
                                             FieldMask &multi_fields,
                LegionMap<AddressSpaceID,FieldMask>::aligned &multi_copies,
                                             AddressSpaceID request_space,
                                             PendingRequest *pending_request,
                                             unsigned &pending_updates)
    //--------------------------------------------------------------------------
    {
      if (!to_filter)
        return;
      const FieldMask multi_overlap = to_filter & multi_fields;
      if (!multi_overlap)
        return;
      FieldMask update_mask = multi_overlap;
      // Handle the same node case now to avoid unnecessary communication
      LegionMap<AddressSpaceID,FieldMask>::aligned::const_iterator finder = 
        multi_copies.find(request_space);
      if (finder != multi_copies.end())
      {
        const FieldMask overlap = update_mask & finder->second;
        if (!!overlap)
        {
          request_invalidate(request_space, request_space, overlap,
              pending_request, pending_updates, true/*meta only*/);
          update_mask -= overlap;
        }
      }
      std::vector<AddressSpaceID> to_delete;
      for (LegionMap<AddressSpaceID,FieldMask>::aligned::iterator it = 
            multi_copies.begin(); it != multi_copies.end(); it++)
      {
        const FieldMask overlap = multi_overlap & it->second;
        if (!overlap)
          continue;
        // We already handled the same node case above
        if (it->first != request_space)
        {
          // See if we need an update
          const FieldMask update = update_mask & overlap;
          if (!!update)
          {
            request_update(it->first, request_space, update, pending_request,
                           pending_updates, true/*invalidate*/);
            update_mask -= update;
            const FieldMask invalidate = overlap - update;
            if (!!invalidate)
              request_invalidate(it->first, request_space, invalidate,
                  pending_request, pending_updates, false/*meta only*/);
          }
          else
            request_invalidate(it->first, request_space, overlap,
                pending_request, pending_updates, false/*meta only*/);
        }
        it->second -= overlap;
        if (!it->second) 
          to_delete.push_back(it->first); 
      }
      if (!to_delete.empty())
      {
        for (std::vector<AddressSpaceID>::const_iterator it = 
              to_delete.begin(); it != to_delete.end(); it++)
          multi_copies.erase(*it);
      }
      multi_fields -= multi_overlap;
      to_filter -= multi_overlap;
    }

    //--------------------------------------------------------------------------
    void EquivalenceSet::filter_redop_modes(FieldMask to_filter)
    //--------------------------------------------------------------------------
    {
      std::vector<ReductionOpID> to_delete;
      for (LegionMap<ReductionOpID,FieldMask>::aligned::iterator it = 
            redop_modes.begin(); it != redop_modes.end(); it++)
      {
        const FieldMask overlap = to_filter & it->second;
        if (!overlap)
          continue;
        it->second -= overlap;
        if (!it->second)
          to_delete.push_back(it->first);
        to_filter -= overlap;
        if (!to_filter)
          break;
      }
      if (!to_delete.empty())
      {
        for (std::vector<ReductionOpID>::const_iterator it = 
              to_delete.begin(); it != to_delete.end(); it++)
          redop_modes.erase(*it);
      }
    }

    //--------------------------------------------------------------------------
    void EquivalenceSet::request_update(AddressSpaceID valid_space, 
                                        AddressSpaceID invalid_space,
                                        const FieldMask &update_mask,
                                        PendingRequest *pending_request,
                                        unsigned &pending_updates,
                                        bool invalidate,
                                        ReductionOpID skip_redop,
                                        bool needs_lock)
    //--------------------------------------------------------------------------
    {
      if (needs_lock)
      {
        AutoLock eq(eq_lock);
        request_update(valid_space, invalid_space, update_mask, pending_request,
                       pending_updates, invalidate, skip_redop, false);
        return;
      }
#ifdef DEBUG_LEGION
      assert(valid_space != invalid_space);
#endif
      // Increment the number of updates that the requestor should expect
      pending_updates++;
      if (valid_space != local_space)
      {
        // Send the request to the valid node  
        Serializer rez;
        {
          RezCheck z(rez);
          rez.serialize(did);
          rez.serialize(invalid_space);
          rez.serialize(update_mask);
          rez.serialize(pending_request);
          rez.serialize<bool>(invalidate);
          rez.serialize(skip_redop);
        }
        runtime->send_equivalence_set_update_request(valid_space, rez);
      }
      else
      {
        // We're on the valid node 
#ifdef DEBUG_LEGION
        assert(invalid_space != local_space);
#endif
        // Before we going packing things up and sending them off
        // see if we need to defer this response until we get all
        // of our own responses back, this can happen with some
        // collapses for reduction modes
        if (!outstanding_requests.empty() && 
            !(outstanding_requests.get_valid_mask() * update_mask))
        {
          // We need the precise set of fields that we are
          // going to depend on for this request
          FieldMask needed_mask;
          for (FieldMaskSet<PendingRequest>::const_iterator it = 
                outstanding_requests.begin(); it != 
                outstanding_requests.end(); it++)
          {
            const FieldMask overlap = it->second & update_mask;
            if (!overlap)
              continue;
            needed_mask |= overlap;
          }
          // Defer this update
          DeferredRequest *deferred = 
            new DeferredRequest(invalid_space, pending_request, 
                                update_mask, invalidate, skip_redop);
          deferred_requests.insert(deferred, needed_mask);
          return;
        }
        // Pack up the message
        Serializer rez;
        {
          RezCheck z(rez);
          rez.serialize(did);
          rez.serialize(pending_request);
          // Pack the valid instances
          if (!valid_instances.empty() &&
              !(valid_instances.get_valid_mask() * update_mask))
          {
            if (valid_instances.size() == 1)
            {
              rez.serialize<size_t>(1);
              FieldMaskSet<LogicalView>::iterator it = 
                valid_instances.begin();
              rez.serialize(it->first->did);
              const FieldMask overlap = it->second & update_mask;
              rez.serialize(overlap);
              if (invalidate)
              {
                it.filter(overlap);
                if (!it->second)
                {
                  if (it->first->remove_nested_valid_ref(did))
                    delete it->first;
                  valid_instances.erase(it);
                }
              }
            }
            else
            {
              std::vector<DistributedID> dids;
              LegionVector<FieldMask>::aligned masks;
              std::vector<LogicalView*> to_delete;
              for (FieldMaskSet<LogicalView>::iterator it = 
                    valid_instances.begin(); it != valid_instances.end(); it++)
              {
                const FieldMask overlap = update_mask & it->second;
                if (!overlap)
                  continue;
                dids.push_back(it->first->did);
                masks.push_back(overlap);
                if (invalidate)
                {
                  it.filter(overlap);
                  if (!it->second)
                    to_delete.push_back(it->first);
                }
              }
              rez.serialize<size_t>(dids.size());
              for (unsigned idx = 0; idx < dids.size(); idx++)
              {
                rez.serialize(dids[idx]);
                rez.serialize(masks[idx]);
              }
              if (!to_delete.empty())
              {
                for (std::vector<LogicalView*>::const_iterator it = 
                      to_delete.begin(); it != to_delete.end(); it++)
                {
                  valid_instances.erase(*it);
                  if ((*it)->remove_nested_valid_ref(did))
                    delete (*it);
                }
              }
            }
          }
          else
            rez.serialize<size_t>(0);
          // Pack the reduction instances
          if (!reduction_instances.empty())
          {
            const FieldMask redop_overlap = reduction_fields & update_mask;
            if (!!redop_overlap)
            {
              rez.serialize<size_t>(redop_overlap.pop_count());
              int fidx = redop_overlap.find_first_set();
              while (fidx >= 0)
              {
                rez.serialize(fidx);
                std::map<unsigned,std::vector<ReductionView*> >::iterator
                  finder = reduction_instances.find(fidx);
#ifdef DEBUG_LEGION
                assert(finder != reduction_instances.end());
                assert(!finder->second.empty());
#endif
                if (skip_redop > 0)
                {
                  size_t send_size = finder->second.size(); 
                  // Skip any reduction views with the same redop
                  // at the end of the list (e.g. ones in the same epoch)
                  while (finder->second[send_size-1]->get_redop() == skip_redop)
                  {
                    send_size--;
                    if (send_size == 0)
                      break;
                  }
                  rez.serialize<size_t>(send_size);
                  for (unsigned idx = 0; idx < send_size; idx++)
                    rez.serialize(finder->second[idx]->did);
                }
                else
                {
                  rez.serialize<size_t>(finder->second.size());
                  for (std::vector<ReductionView*>::const_iterator it = 
                       finder->second.begin(); it != finder->second.end(); it++)
                    rez.serialize((*it)->did);
                }
                if (invalidate)
                {
                  for (std::vector<ReductionView*>::const_iterator it = 
                       finder->second.begin(); it != finder->second.end(); it++)
                    if ((*it)->remove_nested_valid_ref(did))
                      delete (*it);
                  reduction_instances.erase(finder);
                }
                fidx = redop_overlap.find_next_set(fidx+1);
              }
              if (invalidate)
                reduction_fields -= redop_overlap;
            }
            else
              rez.serialize<size_t>(0);
          }
          else
            rez.serialize<size_t>(0);
          // Pack the restricted instances
          if (!restricted_instances.empty() && 
              !(restricted_instances.get_valid_mask() * update_mask))
          {
            if (restricted_instances.size() == 1)
            {
              rez.serialize<size_t>(1);
              FieldMaskSet<InstanceView>::iterator it = 
                restricted_instances.begin();
              rez.serialize(it->first->did);
              const FieldMask overlap = it->second & update_mask;
              rez.serialize(overlap);
              if (invalidate)
              {
                it.filter(overlap);
                if (!it->second)
                {
                  if (it->first->remove_nested_valid_ref(did))
                    delete it->first;
                  restricted_instances.erase(it);
                }
              }
            }
            else
            {
              std::vector<DistributedID> dids;
              LegionVector<FieldMask>::aligned masks;
              std::vector<InstanceView*> to_delete;
              for (FieldMaskSet<InstanceView>::iterator it = 
                    restricted_instances.begin(); it != 
                    restricted_instances.end(); it++)
              {
                const FieldMask overlap = update_mask & it->second;
                if (!overlap)
                  continue;
                dids.push_back(it->first->did);
                masks.push_back(overlap);
                if (invalidate)
                {
                  it.filter(overlap);
                  if (!it->second)
                    to_delete.push_back(it->first);
                }
              }
              rez.serialize<size_t>(dids.size());
              for (unsigned idx = 0; idx < dids.size(); idx++)
              {
                rez.serialize(dids[idx]);
                rez.serialize(masks[idx]);
              }
              if (!to_delete.empty())
              {
                for (std::vector<InstanceView*>::const_iterator it = 
                      to_delete.begin(); it != to_delete.end(); it++)
                {
                  restricted_instances.erase(*it);
                  if ((*it)->remove_nested_valid_ref(did))
                    delete (*it);
                }
              }
            }
            const FieldMask restricted_mask = update_mask & restricted_fields;
            rez.serialize(restricted_mask);
            if (invalidate && !!restricted_mask)
              restricted_fields -= restricted_mask;
          }
          else
            rez.serialize<size_t>(0);
          // Pack the version numbers
          std::vector<VersionID> versions;
          LegionVector<FieldMask>::aligned masks;
          std::vector<VersionID> to_delete;
          for (LegionMap<VersionID,FieldMask>::aligned::iterator it =
                version_numbers.begin(); it != version_numbers.end(); it++)
          {
            const FieldMask overlap = update_mask & it->second;
            if (!overlap)
              continue;
            versions.push_back(it->first);
            masks.push_back(overlap);
            if (invalidate)
            {
              it->second -= overlap;
              if (!it->second)
                to_delete.push_back(it->first);
            }
          }
          rez.serialize<size_t>(versions.size());
          for (unsigned idx = 0; idx < versions.size(); idx++)
          {
            rez.serialize(versions[idx]);
            rez.serialize(masks[idx]);
          }
          if (!to_delete.empty())
          {
            for (std::vector<VersionID>::const_iterator it = 
                  to_delete.begin(); it != to_delete.end(); it++)
              version_numbers.erase(*it);
          }
        }
        // Do any invalidation meta-updates before we send the message
        if (!is_owner())
        {
          if (invalidate)
          {
            exclusive_fields -= update_mask;
            shared_fields -= update_mask;
            single_redop_fields -= update_mask;
            multi_redop_fields -= update_mask;
          }
          else
          {
            // Any of our exclusive or single redop fields are now multi
            const FieldMask exclusive_overlap = exclusive_fields & update_mask;
            if (!!exclusive_overlap)
            {
              exclusive_fields -= exclusive_overlap;
              shared_fields |= exclusive_overlap;
            }
            const FieldMask redop_overlap = single_redop_fields & update_mask;
            if (!!redop_overlap)
            {
              single_redop_fields -= redop_overlap;
              multi_redop_fields |= redop_overlap;
            }
          }
        }
        runtime->send_equivalence_set_update_response(invalid_space, rez);
      }
    }

    //--------------------------------------------------------------------------
    void EquivalenceSet::process_update_response(
                           PendingRequest *pending_request, Deserializer &derez)
    //--------------------------------------------------------------------------
    {
      AutoLock eq(eq_lock);
      std::set<RtEvent> wait_for;
      std::vector<LogicalView*> need_references;
      WrapperReferenceMutator mutator(pending_request->applied_events);

      size_t num_valid;
      derez.deserialize(num_valid);
      for (unsigned idx = 0; idx < num_valid; idx++)
      {
        DistributedID view_did;
        derez.deserialize(view_did);
        RtEvent ready;
        LogicalView *view = 
          runtime->find_or_request_logical_view(view_did, ready);
        FieldMask view_mask;
        derez.deserialize(view_mask);
        if (valid_instances.insert(view, view_mask))
        {
          // New view
          if (ready.exists() && !ready.has_triggered())
          {
            wait_for.insert(ready);
            need_references.push_back(view);
          }
          else
            view->add_nested_valid_ref(did, &mutator);
        }
        else if (ready.exists() && !ready.has_triggered())
          wait_for.insert(ready);
      }
      size_t num_reduc_fields;
      derez.deserialize(num_reduc_fields);
      for (unsigned idx1 = 0; idx1 < num_reduc_fields; idx1++)
      {
        int fidx;
        derez.deserialize(fidx);
        reduction_fields.set_bit(fidx);
        std::vector<ReductionView*> &reduc_views = reduction_instances[fidx];
        size_t num_views;
        derez.deserialize(num_views);
        for (unsigned idx2 = 0; idx2 < num_views; idx2++)
        {
          DistributedID view_did;
          derez.deserialize(view_did);
          RtEvent ready;
          LogicalView *view = 
            runtime->find_or_request_logical_view(view_did, ready);
          ReductionView *reduc_view = static_cast<ReductionView*>(view);
          // Unpack these in order, as long as they zip then we can
          // ignore them, once they stop zipping that is when we begin
          // doing the merge and appending to the end of the list
          if ((idx2 < reduc_views.size()) && (reduc_views[idx2] == reduc_view))
            continue;
          reduc_views.push_back(reduc_view);
          if (ready.exists() && !ready.has_triggered())
          {
            wait_for.insert(ready);
            need_references.push_back(view);
          }
          else
            view->add_nested_valid_ref(did, &mutator);
        }
      }
      size_t num_restricted;
      derez.deserialize(num_restricted);
      if (num_restricted > 0)
      {
        for (unsigned idx = 0; idx < num_restricted; idx++)
        {
          DistributedID view_did;
          derez.deserialize(view_did);
          RtEvent ready;
          LogicalView *view = 
            runtime->find_or_request_logical_view(view_did, ready);
          FieldMask view_mask;
          derez.deserialize(view_mask); 
          if (restricted_instances.insert(
                static_cast<InstanceView*>(view), view_mask))
          {
            // New view
            if (ready.exists() && !ready.has_triggered())
            {
              wait_for.insert(ready);
              need_references.push_back(view);
            }
            else
              view->add_nested_valid_ref(did, &mutator);
          }
          else if (ready.exists() && !ready.has_triggered())
            wait_for.insert(ready);
        }
        if (!!restricted_fields)
        {
          FieldMask restrict_mask;
          derez.deserialize(restrict_mask);
          restricted_fields |= restrict_mask;
        }
        else
          derez.deserialize(restricted_fields);
      }
      if (!wait_for.empty())
      {
        RtEvent wait_on = Runtime::merge_events(wait_for);
        if (wait_on.exists() && !wait_on.has_triggered())
          wait_on.wait();
      }
      if (!need_references.empty())
      {
        for (std::vector<LogicalView*>::const_iterator it = 
              need_references.begin(); it != need_references.end(); it++)
          (*it)->add_nested_valid_ref(did, &mutator);
      }
      pending_request->remaining_count -= 1;
      if (pending_request->remaining_count == 0)
        finalize_pending_request(pending_request);
    }

    //--------------------------------------------------------------------------
    void EquivalenceSet::request_invalidate(AddressSpaceID target,
                                            AddressSpaceID source,
                                            const FieldMask &invalidate_mask,
                                            PendingRequest *pending_request,
                                            unsigned &pending_updates,
                                            bool meta_only, bool needs_lock)
    //--------------------------------------------------------------------------
    {
      if (needs_lock)
      {
        AutoLock eq(eq_lock);
        request_invalidate(target, source, invalidate_mask, pending_request,
                           pending_updates, meta_only, false/*needs lock*/);
        return;
      }
      // Increment the number of updates that the requestor should expect
      pending_updates++;
      if (target != local_space)
      {
        // Send the request to the target node 
        Serializer rez;
        {
          RezCheck z(rez);
          rez.serialize(did);
          rez.serialize(source);
          rez.serialize(invalidate_mask);
          rez.serialize(pending_request);
          rez.serialize<bool>(meta_only);
        }
        runtime->send_equivalence_set_invalidate_request(target, rez);
      }
      else
      {
        // We're the local node so do the invalidation here
        if (!meta_only)
        {
          if (!valid_instances.empty() && 
              !(valid_instances.get_valid_mask() * invalidate_mask))
          {
            std::vector<LogicalView*> to_delete;
            for (FieldMaskSet<LogicalView>::iterator it = 
                  valid_instances.begin(); it != valid_instances.end(); it++)
            {
              const FieldMask overlap = it->second & invalidate_mask;
              if (!overlap)
                continue;
              it.filter(overlap);
              if (!it->second)
                to_delete.push_back(it->first);
            }
            if (!to_delete.empty())
            {
              for (std::vector<LogicalView*>::const_iterator it = 
                    to_delete.begin(); it != to_delete.end(); it++)
              {
                valid_instances.erase(*it);
                if ((*it)->remove_nested_valid_ref(did))
                  delete (*it);
              }
            }
          }
          if (!reduction_instances.empty())
          {
            const FieldMask redop_overlap = reduction_fields & invalidate_mask;
            if (!!redop_overlap)
            {
              int fidx = redop_overlap.find_first_set();
              while (fidx >= 0)
              {
                std::map<unsigned,std::vector<ReductionView*> >::iterator
                  finder = reduction_instances.find(fidx);
#ifdef DEBUG_LEGION
                assert(finder != reduction_instances.end());
#endif
                for (std::vector<ReductionView*>::const_iterator it = 
                      finder->second.begin(); it != finder->second.end(); it++)
                  if ((*it)->remove_nested_valid_ref(did))
                    delete (*it);
                reduction_instances.erase(finder);
                fidx = redop_overlap.find_next_set(fidx+1);
              }
              reduction_fields -= redop_overlap;
            }
          }
          if (!restricted_instances.empty() &&
              !(restricted_instances.get_valid_mask() * invalidate_mask))
          {
            std::vector<InstanceView*> to_delete;
            for (FieldMaskSet<InstanceView>::iterator it = 
                  restricted_instances.begin(); it != 
                  restricted_instances.end(); it++)
            {
              const FieldMask overlap = it->second & invalidate_mask;
              if (!overlap)
                continue;
              it.filter(invalidate_mask);
              if (!it->second)
                to_delete.push_back(it->first);
            }
            for (std::vector<InstanceView*>::const_iterator it = 
                  to_delete.begin(); it != to_delete.end(); it++)
            {
              restricted_instances.erase(*it);
              if ((*it)->remove_nested_valid_ref(did))
                delete (*it);
            }
            restricted_fields -= invalidate_mask;
          }
          std::vector<VersionID> to_delete;
          for (LegionMap<VersionID,FieldMask>::aligned::iterator it =
                version_numbers.begin(); it != version_numbers.end(); it++)
          {
            it->second -= invalidate_mask;
            if (!it->second)
              to_delete.push_back(it->first);
          }
          if (!to_delete.empty())
          {
            for (std::vector<VersionID>::const_iterator it =
                  to_delete.begin(); it != to_delete.end(); it++)
              version_numbers.erase(*it);
          }
        }
        // We only need to invalidate meta-state on remote nodes
        // since the update logic will handle it on the owner node
        if (!is_owner())
        {
          exclusive_fields -= invalidate_mask;
          shared_fields -= invalidate_mask;
          single_redop_fields -= invalidate_mask;
          multi_redop_fields -= invalidate_mask;
        }
        // Then send the response to the source node telling it that
        // the invalidation was done successfully
        if (source != local_space)
        {
          Serializer rez;
          {
            RezCheck z(rez);
            rez.serialize(did);
            rez.serialize(pending_request);
          }
          runtime->send_equivalence_set_invalidate_response(source, rez);
        }
        else
          record_invalidation(pending_request);
      }
    }

    //--------------------------------------------------------------------------
    void EquivalenceSet::record_pending_updates(PendingRequest *pending_request,
                                                unsigned pending_updates,
                                                bool needs_lock)
    //--------------------------------------------------------------------------
    {
      if (needs_lock)
      {
        AutoLock eq(eq_lock);
        record_pending_updates(pending_request, pending_updates, false);
        return;
      }
      pending_request->remaining_count += pending_updates;
      if (pending_request->remaining_count == 0)
        finalize_pending_request(pending_request);         
    }

    //--------------------------------------------------------------------------
    void EquivalenceSet::record_invalidation(PendingRequest *pending_request,
                                             bool needs_lock)
    //--------------------------------------------------------------------------
    {
      if (needs_lock)
      {
        AutoLock eq(eq_lock);
        record_invalidation(pending_request, false/*needs lock*/);
        return;
      }
      pending_request->remaining_count -= 1;
      if (pending_request->remaining_count == 0)
        finalize_pending_request(pending_request);
    }

    //--------------------------------------------------------------------------
    void EquivalenceSet::finalize_pending_request(
                                                PendingRequest *pending_request)
    //--------------------------------------------------------------------------
    {
#ifdef DEBUG_LEGION
      sanity_check();
      assert(pending_request->remaining_count == 0);
#endif
      FieldMaskSet<PendingRequest>::iterator finder = 
        outstanding_requests.find(pending_request);
#ifdef DEBUG_LEGION
      assert(finder != outstanding_requests.end());
#endif
      if (!is_owner())
      {
        const FieldMask &update_mask = finder->second;
        // If we're not the owner then we have to update the state 
        if (pending_request->redop > 0)
        {
          if (!!pending_request->owner_mask)
          {
#ifdef DEBUG_LEGION
            assert(single_redop_fields * pending_request->owner_mask);
#endif
            single_redop_fields |= pending_request->owner_mask;
            const FieldMask non_owner = 
              update_mask - pending_request->owner_mask;
            if (!!non_owner)
            {
#ifdef DEBUG_LEGION
              assert(multi_redop_fields * non_owner);
#endif
              multi_redop_fields |= non_owner;
            }
          }
          else
          {
#ifdef DEBUG_LEGION
            assert(multi_redop_fields * update_mask);
#endif
            multi_redop_fields |= update_mask;
          }
          redop_modes[pending_request->redop] |= update_mask;
        }
        else
        {
          if (!!pending_request->owner_mask)
          {
#ifdef DEBUG_LEGION
            assert(exclusive_fields * pending_request->owner_mask);
#endif
            exclusive_fields |= pending_request->owner_mask;
            const FieldMask non_owner = 
              update_mask - pending_request->owner_mask;
            if (!!non_owner)
            {
#ifdef DEBUG_LEGION
              assert(shared_fields * non_owner);
#endif
              shared_fields |= non_owner;
            }
          }
          else
          {
#ifdef DEBUG_LEGION
            assert(shared_fields * update_mask);
#endif
            shared_fields |= update_mask;
          }
        }
      }
#ifdef DEBUG_LEGION
      sanity_check();
#endif
      if (!pending_request->applied_events.empty())
        Runtime::trigger_event(pending_request->applied_event,
            Runtime::merge_events(pending_request->applied_events));
      else
        Runtime::trigger_event(pending_request->applied_event);
      Runtime::trigger_event(pending_request->ready_event);
      // See if there are any deferred requests that we need to handle
      if (!deferred_requests.empty() &&
          !(finder->second * deferred_requests.get_valid_mask()))
      {
        std::vector<DeferredRequest*> to_perform;
        for (FieldMaskSet<DeferredRequest>::iterator it = 
              deferred_requests.begin(); it != deferred_requests.end(); it++)
        {
          it.filter(finder->second);
          if (!it->second)
            to_perform.push_back(it->first);
        }
        if (!to_perform.empty())
        {
          for (std::vector<DeferredRequest*>::const_iterator it = 
                to_perform.begin(); it != to_perform.end(); it++)
          {
            deferred_requests.erase(*it);
            unsigned dummy_updates = 0;
            request_update(local_space, (*it)->invalid_space,
                           (*it)->update_mask, (*it)->pending_request,
                           dummy_updates, (*it)->invalidate,
                           (*it)->skip_redop, false/*needs lock*/);
            delete (*it);
          }
        }
      }
      outstanding_requests.erase(finder);
      delete pending_request;
    }

#ifdef DEBUG_LEGION
    //--------------------------------------------------------------------------
    void EquivalenceSet::sanity_check(void) const
    //--------------------------------------------------------------------------
    {
      // All the summary masks should be disjoint
      assert(exclusive_fields * shared_fields);
      assert(exclusive_fields * single_redop_fields);
      assert(exclusive_fields * multi_redop_fields);
      assert(shared_fields * single_redop_fields);
      assert(shared_fields * multi_redop_fields);
      assert(single_redop_fields * multi_redop_fields);
      // Reduction modes should all be present
      if (!!single_redop_fields || !! multi_redop_fields)
      {
        FieldMask summary;
        for (LegionMap<ReductionOpID,FieldMask>::aligned::const_iterator
              it = redop_modes.begin(); it != redop_modes.end(); it++)
        {
          assert(!!it->second);
          // fields should appear exactly once
          assert(summary * it->second);
          summary |= it->second;
        }
        const FieldMask combined = single_redop_fields | multi_redop_fields;
        assert(summary == combined);
      }
      else
        assert(redop_modes.empty());
      if (is_owner())
      {
        // Summary masks should match their sets
        if (!!exclusive_fields)
        {
          FieldMask summary;
          for (LegionMap<AddressSpaceID,FieldMask>::aligned::const_iterator it =
                exclusive_copies.begin(); it != exclusive_copies.end(); it++)
          {
            assert(!!it->second);
            // fields should appear exactly once
            assert(summary * it->second);
            summary |= it->second;
          }
          assert(summary == exclusive_fields);
        }
        else
          assert(exclusive_copies.empty());
        if (!!shared_fields)
        {
          FieldMask summary;
          for (LegionMap<AddressSpaceID,FieldMask>::aligned::const_iterator it =
                shared_copies.begin(); it != shared_copies.end(); it++)
          {
            assert(!!it->second);
            summary |= it->second;
          }
          assert(summary == shared_fields);
        }
        else
          assert(shared_copies.empty());
        if (!!single_redop_fields)
        {
          FieldMask summary;
          for (LegionMap<AddressSpaceID,FieldMask>::aligned::const_iterator it =
                single_reduction_copies.begin(); it != 
                single_reduction_copies.end(); it++)
          {
            assert(!!it->second);
            // fields should appear exactly once
            assert(summary * it->second);
            summary |= it->second;
          }
          assert(summary == single_redop_fields);
        }
        else
          assert(single_reduction_copies.empty());
        if (!!multi_redop_fields)
        {
          FieldMask summary;
          for (LegionMap<AddressSpaceID,FieldMask>::aligned::const_iterator it =
                multi_reduction_copies.begin(); it != 
                multi_reduction_copies.end(); it++)
          {
            assert(!!it->second);
            summary |= it->second;
          }
          assert(summary == multi_redop_fields);
        }
        else
          assert(multi_reduction_copies.empty()); 
      }
    }
#endif

    //--------------------------------------------------------------------------
    void EquivalenceSet::initialize_set(ApEvent term_event,
                                        const RegionUsage &usage,
                                        const FieldMask &user_mask,
                                        const bool restricted,
                                        const InstanceSet &sources,
                                const std::vector<InstanceView*> &corresponding,
                                        UniqueID context_uid, unsigned index,
                                        std::set<RtEvent> &applied_events)
    //--------------------------------------------------------------------------
    {
#ifdef DEBUG_LEGION
      assert(is_owner());
      assert(sources.size() == corresponding.size());
#endif
      WrapperReferenceMutator mutator(applied_events);
      AutoLock eq(eq_lock);
      if (IS_REDUCE(usage))
      {
#ifdef DEBUG_LEGION
        // Reduction-only should always be restricted for now
        // Could change if we started issuing reduction close
        // operations at the end of a context
        assert(restricted);
#endif
        // Since these are restricted, we'll make these the actual
        // target logical instances and record them as restricted
        // instead of recording them as reduction instances
        for (unsigned idx = 0; idx < sources.size(); idx++)
        {
          const FieldMask &view_mask = sources[idx].get_valid_fields();
          InstanceView *view = corresponding[idx];
          FieldMaskSet<LogicalView>::iterator finder = 
            valid_instances.find(view);
          if (finder == valid_instances.end())
          {
            valid_instances.insert(view, view_mask);
            view->add_nested_valid_ref(did, &mutator);
          }
          else
            finder.merge(view_mask);
          // Always restrict reduction-only users since we know the data
          // is going to need to be flushed anyway
          FieldMaskSet<InstanceView>::iterator restricted_finder = 
            restricted_instances.find(view);
          if (restricted_finder == restricted_instances.end())
          {
            restricted_instances.insert(view, view_mask);
            view->add_nested_valid_ref(did, &mutator);
          }
          else
            restricted_finder.merge(view_mask);
          // Record the user for the instance
          view->add_initial_user(term_event, usage, view_mask, 
                                 set_expr, context_uid, index);
        }
      }
      else
      {
        for (unsigned idx = 0; idx < sources.size(); idx++)
        {
          const FieldMask &view_mask = sources[idx].get_valid_fields();
          InstanceView *view = corresponding[idx];
#ifdef DEBUG_LEGION
          assert(!view->is_reduction_view());
#endif
          FieldMaskSet<LogicalView>::iterator finder = 
            valid_instances.find(view);
          if (finder == valid_instances.end())
          {
            valid_instances.insert(view, view_mask);
            view->add_nested_valid_ref(did, &mutator);
          }
          else
            finder.merge(view_mask);
          // If this is restricted then record it
          if (restricted)
          {
            FieldMaskSet<InstanceView>::iterator restricted_finder = 
              restricted_instances.find(view);
            if (restricted_finder == restricted_instances.end())
            {
              restricted_instances.insert(view, view_mask);
              view->add_nested_valid_ref(did, &mutator);
            }
            else
              restricted_finder.merge(view_mask);
          }
          // Record the user for the instance
          view->add_initial_user(term_event, usage, view_mask, 
                                 set_expr, context_uid, index);
        }
      }
      // Update any restricted fields 
      if (restricted)
        restricted_fields |= user_mask;
      // Set the version numbers too
      version_numbers[init_version] |= user_mask;
    }

    //--------------------------------------------------------------------------
    bool EquivalenceSet::find_valid_instances(FieldMaskSet<LogicalView> &insts,
                                              const FieldMask &user_mask) const
    //--------------------------------------------------------------------------
    {
      AutoLock eq(eq_lock,1,false/*exclusive*/);
      for (FieldMaskSet<LogicalView>::const_iterator it = 
            valid_instances.begin(); it != valid_instances.end(); it++)
      {
        const FieldMask overlap = it->second & user_mask;
        if (!overlap)
          continue;
        FieldMaskSet<LogicalView>::iterator finder = insts.find(it->first);
        if (finder == insts.end())
          insts.insert(it->first, it->second);
        else
          finder.merge(it->second);
      }
      return has_restrictions(user_mask);
    }

    //--------------------------------------------------------------------------
    bool EquivalenceSet::filter_valid_instances(
             FieldMaskSet<LogicalView> &insts, const FieldMask &user_mask) const
    //--------------------------------------------------------------------------
    {
      AutoLock eq(eq_lock,1,false/*exclusive*/);
      std::vector<LogicalView*> to_erase;
      for (FieldMaskSet<LogicalView>::iterator it = 
            insts.begin(); it != insts.end(); it++)
      {
        FieldMaskSet<LogicalView>::const_iterator finder = 
          valid_instances.find(it->first);
        if (finder != valid_instances.end())
        {
          const FieldMask diff = it->second - finder->second;
          if (!!diff)
          {
            it.filter(diff);
            if (!it->second)
              to_erase.push_back(it->first);
          }
        }
        else
          to_erase.push_back(it->first);
      }
      if (!to_erase.empty())
      {
        if (to_erase.size() != insts.size())
        {
          for (std::vector<LogicalView*>::const_iterator it = 
                to_erase.begin(); it != to_erase.end(); it++)
            insts.erase(*it);
        }
        else
          insts.clear();
      }
      return has_restrictions(user_mask);
    }

    //--------------------------------------------------------------------------
    bool EquivalenceSet::find_reduction_instances(
                        FieldMaskSet<ReductionView> &insts, ReductionOpID redop,
                        const FieldMask &user_mask) const
    //--------------------------------------------------------------------------
    {
      AutoLock eq(eq_lock,1,false/*exclusive*/);
      // Iterate over all the fields
      int fidx = user_mask.find_first_set();
      while (fidx >= 0)
      {
        std::map<unsigned,std::vector<ReductionView*> >::const_iterator 
          current = reduction_instances.find(fidx);
        if (current != reduction_instances.end())
        {
          FieldMask local_mask;
          local_mask.set_bit(fidx);
          for (std::vector<ReductionView*>::const_reverse_iterator it = 
                current->second.rbegin(); it != current->second.rend(); it++)
          {
            ReductionManager *manager = 
              (*it)->get_manager()->as_reduction_manager();
            if (manager->redop != redop)
              break;
            FieldMaskSet<ReductionView>::iterator finder = 
              insts.find(*it);
            if (finder == insts.end())
              insts.insert(*it, local_mask);
            else
              finder.merge(local_mask);
          }
        }
        fidx = user_mask.find_next_set(fidx+1);
      }
      return has_restrictions(user_mask);
    }

    //--------------------------------------------------------------------------
    bool EquivalenceSet::filter_reduction_instances(
                       FieldMaskSet<ReductionView> &insts, ReductionOpID redop,
                       const FieldMask &user_mask) const
    //--------------------------------------------------------------------------
    {
      AutoLock eq(eq_lock,1,false/*exclusive*/);
      // Iterate over all the fields
      int fidx = user_mask.find_first_set();
      FieldMask filter_mask;
      std::vector<ReductionView*> to_erase;
      while (fidx >= 0)
      {
        std::map<unsigned,std::vector<ReductionView*> >::const_iterator 
          current = reduction_instances.find(fidx);
        if (current != reduction_instances.end())
        {
          FieldMask local_mask;
          local_mask.set_bit(fidx);
          for (FieldMaskSet<ReductionView>::iterator cit = 
                insts.begin(); cit != insts.end(); cit++)
          {
            bool found = false;
            for (std::vector<ReductionView*>::const_reverse_iterator it = 
                  current->second.rbegin(); it != current->second.rend(); it++)
            {
              ReductionManager *manager = 
                  (*it)->get_manager()->as_reduction_manager();
              if (manager->redop != redop)
                break;
              if (cit->first != (*it))
                continue;
              found = true;
              break;
            }
            if (!found)
            {
              cit.filter(local_mask);
              if (!cit->second)
                to_erase.push_back(cit->first);
            }
          }
        }
        else
          filter_mask.set_bit(fidx);
        fidx = user_mask.find_next_set(fidx+1);
      }
      if (!!filter_mask)
        insts.filter(filter_mask);
      if (!to_erase.empty())
      {
        if (to_erase.size() != insts.size())
        {
          for (std::vector<ReductionView*>::const_iterator it = 
                to_erase.begin(); it != to_erase.end(); it++)
            insts.erase(*it);
        }
        else
          insts.clear();
      }
      return has_restrictions(user_mask);
    }

    //--------------------------------------------------------------------------
    void EquivalenceSet::update_set(const RegionUsage &usage, 
                                const FieldMask &user_mask,
                                const InstanceSet &target_instances,
                                const std::vector<InstanceView*> &target_views,
                                CopyFillAggregator &input_aggregator,
                                CopyFillAggregator &output_aggregator,
                                std::set<RtEvent> &applied_events,
                                FieldMask *initialized/*=NULL*/)
    //--------------------------------------------------------------------------
    {
#ifdef DEBUG_LEGION
      assert(target_instances.size() == target_views.size());
#endif
      WrapperReferenceMutator mutator(applied_events);
      AutoLock eq(eq_lock);
      // Check for any uninitialized data
      if (initialized != NULL)
      {
        FieldMask &initialized_fields = *initialized;
        if (!!initialized_fields)
          initialized_fields &= valid_instances.get_valid_mask(); 
      }
      if (IS_REDUCE(usage))
      {
        // Reduction-only
        // Record the reduction instances
        for (unsigned idx = 0; idx < target_views.size(); idx++)
        {
          ReductionView *red_view = target_views[idx]->as_reduction_view();
#ifdef DEBUG_LEGION
          assert(red_view->get_redop() == usage.redop);
#endif
          const FieldMask &update_fields = 
            target_instances[idx].get_valid_fields(); 
          int fidx = update_fields.find_first_set();
          while (fidx >= 0)
          {
            std::vector<ReductionView*> &field_views = 
              reduction_instances[fidx];
            red_view->add_nested_valid_ref(did, &mutator); 
            field_views.push_back(red_view);
            fidx = update_fields.find_next_set(fidx+1);
          }
        }
        // Flush any restricted fields
        if (!!restricted_fields)
        {
          const FieldMask reduce_mask = user_mask & restricted_fields;
          if (!!reduce_mask)
            apply_reductions(reduce_mask, output_aggregator); 
          reduction_fields |= (user_mask - restricted_fields);
        }
        else
          reduction_fields |= user_mask;
      }
      else if (IS_WRITE(usage) && IS_DISCARD(usage))
      {
        // Write-only
        // Filter any reductions that we no longer need
        const FieldMask reduce_filter = reduction_fields & user_mask;
        if (!!reduce_filter)
          filter_reduction_instances(reduce_filter);
        // Filter any normal instances that will be overwritten
        const FieldMask non_restricted = user_mask - restricted_fields; 
        if (!!non_restricted)
        {
          filter_valid_instances(non_restricted);
          // Record any non-restricted instances
          record_instances(non_restricted, target_instances, 
                           target_views, mutator);
        }
        // Issue copy-out copies for any restricted fields
        if (!!restricted_fields)
        {
          const FieldMask restricted_mask = user_mask & restricted_fields;
          if (!!restricted_mask)
            copy_out(restricted_mask, target_instances,
                     target_views, output_aggregator);
        }
        // Advance our version numbers
        advance_version_numbers(user_mask);
      }
      else
      {
        // Read-write or read-only
        // Check for any copies from normal instances first
        issue_update_copies_and_fills(input_aggregator, user_mask, 
                                      target_instances, target_views, set_expr);
        // Get the set of fields to filter, any for which we're about
        // to apply pending reductions or overwite, except those that
        // are restricted
        const FieldMask reduce_mask = reduction_fields & user_mask;
        const FieldMask restricted_mask = restricted_fields & user_mask;
        const bool is_write = IS_WRITE(usage);
        FieldMask filter_mask = is_write ? user_mask : reduce_mask;
        if (!!restricted_mask)
          filter_mask -= restricted_mask;
        if (!!filter_mask)
          filter_valid_instances(filter_mask);
        // Save the instances if they are not restricted
        // Otherwise if they are restricted then the restricted instances
        // are already listed as the valid views so there's nothing more
        // for us to have to do
        if (!!restricted_mask)
        {
          const FieldMask non_restricted = user_mask - restricted_fields;
          if (!!non_restricted)
            record_instances(non_restricted, target_instances, 
                             target_views, mutator); 
        }
        else
          record_instances(user_mask, target_instances,
                           target_views, mutator);
        // Next check for any reductions that need to be applied
        if (!!reduce_mask)
          apply_reductions(reduce_mask, input_aggregator); 
        // Issue copy-out copies for any restricted fields if we wrote stuff
        if (is_write) 
        {
          advance_version_numbers(user_mask);
          if (!!restricted_mask)
            copy_out(restricted_mask, target_instances,
                     target_views, output_aggregator);
        }
      }
    }

    //--------------------------------------------------------------------------
    void EquivalenceSet::acquire_restrictions(FieldMask acquire_mask,
                                          FieldMaskSet<InstanceView> &instances,
           std::map<InstanceView*,std::set<IndexSpaceExpression*> > &inst_exprs)
    //--------------------------------------------------------------------------
    {
      AutoLock eq(eq_lock);
      acquire_mask &= restricted_fields;
      if (!acquire_mask)
        return;
      for (FieldMaskSet<InstanceView>::const_iterator it = 
            restricted_instances.begin(); it != restricted_instances.end();it++)
      {
        const FieldMask overlap = acquire_mask & it->second;
        if (!overlap)
          continue;
        InstanceView *view = it->first->as_instance_view();
        FieldMaskSet<InstanceView>::iterator finder = 
          instances.find(view);
        if (finder != instances.end())
          finder.merge(overlap);
        else
          instances.insert(view, overlap);
        inst_exprs[view].insert(set_expr);
      }
      restricted_fields -= acquire_mask;
    }

    //--------------------------------------------------------------------------
    void EquivalenceSet::release_restrictions(const FieldMask &release_mask,
                                        CopyFillAggregator &release_aggregator,
                                        FieldMaskSet<InstanceView> &instances,
           std::map<InstanceView*,std::set<IndexSpaceExpression*> > &inst_exprs,
                                        std::set<RtEvent> &ready_events)
    //--------------------------------------------------------------------------
    {
      AutoLock eq(eq_lock);
      // Find our local restricted instances and views and record them
      InstanceSet local_instances;
      std::vector<InstanceView*> local_views;
      for (FieldMaskSet<InstanceView>::const_iterator it = 
            restricted_instances.begin(); it != restricted_instances.end();it++)
      {
        const FieldMask overlap = it->second & release_mask;
        if (!overlap)
          continue;
        InstanceView *view = it->first->as_instance_view();
        local_instances.add_instance(InstanceRef(view->get_manager(), overlap));
        local_views.push_back(view);
        FieldMaskSet<InstanceView>::iterator finder = instances.find(view);
        if (finder != instances.end())
          finder.merge(overlap);
        else
          instances.insert(view, overlap);
        inst_exprs[view].insert(set_expr);
      }
      // Issue the updates
      issue_update_copies_and_fills(release_aggregator, release_mask,
                                    local_instances, local_views, set_expr);
      // Filter the valid views
      filter_valid_instances(release_mask);
      // Update with just the restricted instances
      WrapperReferenceMutator mutator(ready_events);
      record_instances(release_mask, local_instances, local_views, mutator);
      // See if we have any reductions to apply as well
      const FieldMask reduce_mask = release_mask & reduction_fields;
      if (!!reduce_mask)
        apply_reductions(reduce_mask, release_aggregator);
      // Add the fields back to the restricted ones
      restricted_fields |= release_mask;
    }

    //--------------------------------------------------------------------------
    void EquivalenceSet::issue_across_copies(const RegionUsage &usage,
                const FieldMask &src_mask, const InstanceSet &target_instances,
                const std::vector<InstanceView*> &target_views,
                IndexSpaceExpression *overlap, CopyFillAggregator &aggregator, 
                PredEvent pred_guard, ReductionOpID redop,
                FieldMask &initialized_fields,
                const std::vector<unsigned> *src_indexes,
                const std::vector<unsigned> *dst_indexes,
                const std::vector<CopyAcrossHelper*> *across_helpers) const
    //--------------------------------------------------------------------------
    {
#ifdef DEBUG_LEGION
      assert(target_instances.size() == target_views.size());
#endif
      AutoLock eq(eq_lock,1,false/*exclusive*/);
      // Check for any uninitialized fields
      initialized_fields &= valid_instances.get_valid_mask();
      if (pred_guard.exists())
        assert(false);
      if (across_helpers != NULL)
      {
        // The general case where fields don't align regardless of
        // whether we are doing a reduction across or not
#ifdef DEBUG_LEGION
        assert(src_indexes != NULL);
        assert(dst_indexes != NULL);
        assert(src_indexes->size() == dst_indexes->size());
        assert(across_helpers->size() == target_instances.size());
#endif
        // We need to figure out how to issue these copies ourself since
        // we need to map from one field to another
        // First construct a map from dst indexes to src indexes 
        std::map<unsigned,unsigned> dst_to_src;
        for (unsigned idx = 0; idx < src_indexes->size(); idx++)
          dst_to_src[(*dst_indexes)[idx]] = (*src_indexes)[idx];
        // Iterate over the target instances
        for (unsigned idx = 0; idx < target_views.size(); idx++)
        {
          const FieldMask dst_mask = target_instances[idx].get_valid_fields();
          // Compute a src_mask based on the dst mask
          FieldMask src_mask;
          int fidx = dst_mask.find_first_set();
          while (fidx >= 0)
          {
            std::map<unsigned,unsigned>::const_iterator finder = 
              dst_to_src.find(fidx);
#ifdef DEBUG_LEGION
            assert(finder != dst_to_src.end());
#endif
            src_mask.set_bit(finder->second);
            fidx = dst_mask.find_next_set(fidx+1);
          }
          // Now find all the source instances for this destination
          FieldMaskSet<LogicalView> src_views;
          for (FieldMaskSet<LogicalView>::const_iterator it =
                valid_instances.begin(); it != valid_instances.end(); it++)
          {
            const FieldMask overlap = it->second & src_mask;
            if (!overlap)
              continue;
            src_views.insert(it->first, overlap);
          }
          aggregator.record_updates(target_views[idx], src_views,
              src_mask, overlap, redop, (*across_helpers)[idx]);
        }
        // Now check for any reductions that need to be applied
        FieldMask reduce_mask = reduction_fields & src_mask;
        if (!!reduce_mask)
        {
#ifdef DEBUG_LEGION
          assert(redop == 0); // can't have reductions of reductions
#endif
          std::map<unsigned,unsigned> src_to_dst;
          for (unsigned idx = 0; idx < src_indexes->size(); idx++)
            src_to_dst[(*src_indexes)[idx]] = (*dst_indexes)[idx];
          int src_fidx = reduce_mask.find_first_set();
          while (src_fidx >= 0)
          {
            std::map<unsigned,std::vector<ReductionView*> >::const_iterator
              finder = reduction_instances.find(src_fidx);
#ifdef DEBUG_LEGION
            assert(finder != reduction_instances.end());
            assert(src_to_dst.find(src_fidx) != src_to_dst.end());
#endif
            const unsigned dst_fidx = src_to_dst[src_fidx];
            // Find the target targets and record them
            for (unsigned idx = 0; idx < target_views.size(); idx++)
            {
              const FieldMask target_mask = 
                target_instances[idx].get_valid_fields();
              if (!target_mask.is_set(dst_fidx))
                continue;
              aggregator.record_reductions(target_views[idx], finder->second,
                        src_fidx, dst_fidx, overlap, (*across_helpers)[idx]);
            }
            src_fidx = reduce_mask.find_next_set(src_fidx+1);
          }
        }
      }
      else if (redop == 0)
      {
        // Fields align and we're not doing a reduction so we can just 
        // do a normal update copy analysis to figure out what to do
        issue_update_copies_and_fills(aggregator, src_mask, target_instances,
                                      target_views, overlap,true/*skip check*/);
        // We also need to check for any reductions that need to be applied
        const FieldMask reduce_mask = reduction_fields & src_mask;
        if (!!reduce_mask)
        {
          int fidx = reduce_mask.find_first_set();
          while (fidx >= 0)
          {
            std::map<unsigned,std::vector<ReductionView*> >::const_iterator
              finder = reduction_instances.find(fidx);
#ifdef DEBUG_LEGION
            assert(finder != reduction_instances.end());
#endif
            // Find the target targets and record them
            for (unsigned idx = 0; idx < target_views.size(); idx++)
            {
              const FieldMask target_mask = 
                target_instances[idx].get_valid_fields();
              if (!target_mask.is_set(fidx))
                continue;
              aggregator.record_reductions(target_views[idx], 
                          finder->second, fidx, fidx, overlap);
            }
            fidx = reduce_mask.find_next_set(fidx+1);
          }
        }
      }
      else
      {
        // Fields align but we're doing a reduction across
        // Find the valid views that we need for issuing the updates  
        FieldMaskSet<LogicalView> src_views;
        for (FieldMaskSet<LogicalView>::const_iterator it = 
              valid_instances.begin(); it != valid_instances.end(); it++)
        {
          const FieldMask overlap = it->second & src_mask;
          if (!overlap)
            continue;
          src_views.insert(it->first, overlap);
        }
        for (unsigned idx = 0; idx < target_views.size(); idx++)
        {
          const FieldMask &mask = target_instances[idx].get_valid_fields(); 
          aggregator.record_updates(target_views[idx], src_views, mask,
                                    overlap, redop, NULL/*across*/);
        }
        // There shouldn't be any reduction instances to worry about here
#ifdef DEBUG_LEGION
        assert(reduction_fields * src_mask);
#endif
      }
    }

    //--------------------------------------------------------------------------
    void EquivalenceSet::overwrite_set(LogicalView *view, const FieldMask &mask,
                                       CopyFillAggregator &output_aggregator,
                                       std::set<RtEvent> &ready_events,
                                       PredEvent pred_guard,
                                       bool add_restriction)
    //--------------------------------------------------------------------------
    {
      AutoLock eq(eq_lock);
      // Two different cases here depending on whether we have a precidate 
      if (pred_guard.exists())
      {
#ifdef DEBUG_LEGION
        assert(!add_restriction); // shouldn't be doing this in this case
#endif
        // We have a predicate so collapse everything to all the valid
        // instances and then do predicate fills to all those instances
        assert(false);
      }
      else
      {
        // Easy case, just filter everything and add the new view
        const FieldMask reduce_filter = mask & reduction_fields;
        if (!!reduce_filter)
          filter_reduction_instances(reduce_filter);
        filter_valid_instances(mask);
        FieldMaskSet<LogicalView>::iterator finder = 
          valid_instances.find(view);
        if (finder == valid_instances.end())
        {
          WrapperReferenceMutator mutator(ready_events);
          view->add_nested_valid_ref(did, &mutator);
          valid_instances.insert(view, mask);
        }
        else
          finder.merge(mask);
        // Advance the version numbers
        advance_version_numbers(mask);
        if (add_restriction)
        {
#ifdef DEBUG_LEGION
          assert(view->is_instance_view());
#endif
          InstanceView *inst_view = view->as_instance_view();
          FieldMaskSet<InstanceView>::iterator restricted_finder = 
            restricted_instances.find(inst_view);
          if (restricted_finder == restricted_instances.end())
          {
            WrapperReferenceMutator mutator(ready_events);
            view->add_nested_valid_ref(did, &mutator);
            restricted_instances.insert(inst_view, mask);
          }
          else
            restricted_finder.merge(mask);
          restricted_fields |= mask; 
        }
      }
    }

    //--------------------------------------------------------------------------
    void EquivalenceSet::filter_set(LogicalView *view, const FieldMask &mask,
                                    bool remove_restriction/*= false*/)
    //--------------------------------------------------------------------------
    {
      AutoLock eq(eq_lock);
      FieldMaskSet<LogicalView>::iterator finder = valid_instances.find(view);
      if (finder != valid_instances.end())
      {
        finder.filter(mask);
        if (!finder->second)
        {
          if (view->remove_nested_valid_ref(did))
            delete view;
          valid_instances.erase(view);
        }
      }
      if (remove_restriction)
      {
        restricted_fields -= mask;
#ifdef DEBUG_LEGION
        assert(view->is_instance_view());
#endif
        InstanceView *inst_view = view->as_instance_view();
        FieldMaskSet<InstanceView>::iterator restricted_finder = 
          restricted_instances.find(inst_view);
        if (restricted_finder != restricted_instances.end())
        {
          restricted_finder.filter(mask);
          if (!restricted_finder->second)
          {
            if (view->remove_nested_valid_ref(did))
              delete view;
            restricted_instances.erase(inst_view);
          }
        }
      }
    }
    
    //--------------------------------------------------------------------------
    void EquivalenceSet::record_instances(const FieldMask &record_mask,
                                 const InstanceSet &target_instances, 
                                 const std::vector<InstanceView*> &target_views,
                                          ReferenceMutator &mutator)
    //--------------------------------------------------------------------------
    {
      for (unsigned idx = 0; idx < target_views.size(); idx++)
      {
        const FieldMask valid_mask = 
          target_instances[idx].get_valid_fields() & record_mask;
        if (!valid_mask)
          continue;
        InstanceView *target = target_views[idx];
        // Add it to the set
        FieldMaskSet<LogicalView>::iterator finder = 
          valid_instances.find(target);
        if (finder == valid_instances.end())
        {
          target->add_nested_valid_ref(did, &mutator);
          valid_instances.insert(target, valid_mask);
        }
        else
          finder.merge(valid_mask);
      }
    }

    //--------------------------------------------------------------------------
    void EquivalenceSet::issue_update_copies_and_fills(
                                 CopyFillAggregator &aggregator,
                                 FieldMask update_mask,
                                 const InstanceSet &target_instances,
                                 const std::vector<InstanceView*> &target_views,
                                 IndexSpaceExpression *update_expr,
                                 bool skip_check) const
    //--------------------------------------------------------------------------
    {
      if (!skip_check)
      {
        // Scan through and figure out which fields are already valid
        for (unsigned idx = 0; idx < target_views.size(); idx++)
        {
          FieldMaskSet<LogicalView>::const_iterator finder = 
            valid_instances.find(target_views[idx]);
          if (finder == valid_instances.end())
            continue;
          const FieldMask &needed_mask = 
            target_instances[idx].get_valid_fields();
          const FieldMask already_valid = needed_mask & finder->second;
          if (!already_valid)
            continue;
          update_mask -= already_valid;
          // If we're already valid for all the fields then we're done
          if (!update_mask)
            return;
        }
      }
#ifdef DEBUG_LEGION
      assert(!!update_mask);
#endif
      // Find the valid views that we need for issuing the updates  
      FieldMaskSet<LogicalView> valid_views;
      for (FieldMaskSet<LogicalView>::const_iterator it = 
            valid_instances.begin(); it != valid_instances.end(); it++)
      {
        const FieldMask overlap = it->second & update_mask;
        if (!overlap)
          continue;
        valid_views.insert(it->first, overlap);
      }
      // Can happen with uninitialized data, we handle this case
      // before calling this method
      if (valid_views.empty())
        return;
      if (target_instances.size() == 1)
        aggregator.record_updates(target_views[0], valid_views, 
                                  update_mask, update_expr);
      else if (valid_views.size() == 1)
      {
        for (unsigned idx = 0; idx < target_views.size(); idx++)
        {
          const FieldMask &mask = target_instances[idx].get_valid_fields();
          aggregator.record_updates(target_views[idx], valid_views,
                                    mask, update_expr);
        }
      }
      else
      {
        for (unsigned idx = 0; idx < target_views.size(); idx++)
        {
          const FieldMask &dst_mask = target_instances[idx].get_valid_fields();
          FieldMaskSet<LogicalView> src_views;
          for (FieldMaskSet<LogicalView>::const_iterator it = 
                valid_views.begin(); it != valid_views.end(); it++)
          {
            const FieldMask overlap = dst_mask & it->second;
            if (!overlap)
              continue;
            src_views.insert(it->first, overlap);
          }
          aggregator.record_updates(target_views[idx], src_views,
                                    dst_mask, update_expr);
        }
      }
    }

    //--------------------------------------------------------------------------
    void EquivalenceSet::filter_valid_instances(const FieldMask &filter_mask)
    //--------------------------------------------------------------------------
    {
#ifdef DEBUG_LEGION
      assert(!!filter_mask);
#endif
      std::vector<LogicalView*> to_erase;
      for (FieldMaskSet<LogicalView>::iterator it = 
            valid_instances.begin(); it != valid_instances.end(); it++)
      {
        const FieldMask overlap = it->second & filter_mask;
        if (!overlap)
          continue;
        it.filter(overlap);
        if (!it->second)
          to_erase.push_back(it->first);
      }
      if (!to_erase.empty())
      {
        for (std::vector<LogicalView*>::const_iterator it = 
              to_erase.begin(); it != to_erase.end(); it++)
        {
          valid_instances.erase(*it);
          if ((*it)->remove_nested_valid_ref(did))
            delete (*it);
        }
      }
    }

    //--------------------------------------------------------------------------
    void EquivalenceSet::filter_reduction_instances(const FieldMask &to_filter)
    //--------------------------------------------------------------------------
    {
#ifdef DEBUG_LEGION
      assert(!!to_filter);
#endif
      int fidx = to_filter.find_first_set();
      while (fidx >= 0)
      {
        std::map<unsigned,std::vector<ReductionView*> >::iterator
          finder = reduction_instances.find(fidx);
#ifdef DEBUG_LEGION
        assert(finder != reduction_instances.end());
#endif
        for (std::vector<ReductionView*>::const_iterator it = 
              finder->second.begin(); it != finder->second.end(); it++)
          if ((*it)->remove_nested_valid_ref(did))
            delete (*it);
        reduction_instances.erase(finder);
        fidx = to_filter.find_next_set(fidx+1);
      }
      reduction_fields -= to_filter;
    }

    //--------------------------------------------------------------------------
    void EquivalenceSet::apply_reductions(const FieldMask &reduce_mask,
                                          CopyFillAggregator &aggregator)
    //--------------------------------------------------------------------------
    {
#ifdef DEBUG_LEGION
      assert(!!reduce_mask);
#endif
      int fidx = reduce_mask.find_first_set();
      while (fidx >= 0)
      {
        std::map<unsigned,std::vector<ReductionView*> >::iterator finder = 
          reduction_instances.find(fidx);
#ifdef DEBUG_LEGION
        assert(finder != reduction_instances.end());
#endif
        // Find the target targets and record them
        for (FieldMaskSet<LogicalView>::const_iterator it = 
              valid_instances.begin(); it != valid_instances.end(); it++)
        {
          if (!it->second.is_set(fidx))
            continue;
          // Shouldn't have any deferred views here
          InstanceView *dst_view = it->first->as_instance_view();
          aggregator.record_reductions(dst_view, finder->second, fidx, 
                                       fidx, set_expr);
        }
        // Remove the reduction views from those available
        for (std::vector<ReductionView*>::const_iterator it = 
              finder->second.begin(); it != finder->second.end(); it++)
          if ((*it)->remove_nested_valid_ref(did))
            delete (*it);
        reduction_instances.erase(finder);
        fidx = reduce_mask.find_next_set(fidx+1);
      }
      // Record that we advanced the version number in this case
      advance_version_numbers(reduce_mask);
      // These reductions have been applied so we are done
      reduction_fields -= reduce_mask;
    }

    //--------------------------------------------------------------------------
    void EquivalenceSet::copy_out(const FieldMask &restricted_mask,
                                  const InstanceSet &src_instances,
                                  const std::vector<InstanceView*> &src_views,
                                  CopyFillAggregator &aggregator) const
    //--------------------------------------------------------------------------
    {
#ifdef DEBUG_LEGION
      assert(!!restricted_mask);
#endif
      if (valid_instances.size() == 1)
      {
        // Only 1 destination
        FieldMaskSet<LogicalView>::const_iterator first = 
          valid_instances.begin();
#ifdef DEBUG_LEGION
        assert(!(restricted_mask - first->second));
#endif
        InstanceView *dst_view = first->first->as_instance_view();
        FieldMaskSet<LogicalView> srcs;
        for (unsigned idx = 0; idx < src_views.size(); idx++)
        {
          const FieldMask overlap = 
            src_instances[idx].get_valid_fields() & restricted_mask;
          if (!overlap)
            continue;
          srcs.insert(src_views[idx], overlap);
        }
        aggregator.record_updates(dst_view, srcs, restricted_mask, set_expr);
      }
      else if (src_instances.size() == 1)
      {
        // Only 1 source
#ifdef DEBUG_LEGION
        assert(!(restricted_mask - src_instances[0].get_valid_fields()));
#endif
        FieldMaskSet<LogicalView> srcs;
        srcs.insert(src_views[0], restricted_mask);
        for (FieldMaskSet<LogicalView>::const_iterator it = 
              valid_instances.begin(); it != valid_instances.end(); it++)
        {
          const FieldMask overlap = it->second & restricted_mask;
          if (!overlap)
            continue;
          InstanceView *dst_view = it->first->as_instance_view();
          aggregator.record_updates(dst_view, srcs, overlap, set_expr);
        }
      }
      else
      {
        // General case for cross-products
        for (FieldMaskSet<LogicalView>::const_iterator it = 
              valid_instances.begin(); it != valid_instances.end(); it++)
        {
          const FieldMask dst_overlap = it->second & restricted_mask;
          if (!dst_overlap)
            continue;
          InstanceView *dst_view = it->first->as_instance_view();
          FieldMaskSet<LogicalView> srcs;
          for (unsigned idx = 0; idx < src_views.size(); idx++)
          {
            const FieldMask src_overlap = 
              src_instances[idx].get_valid_fields() & dst_overlap;
            if (!src_overlap)
              continue;
            srcs.insert(src_views[idx], src_overlap);
          }
#ifdef DEBUG_LEGION
          assert(!srcs.empty());
#endif
          aggregator.record_updates(dst_view, srcs, dst_overlap, set_expr);
        }
      }
    }

    //--------------------------------------------------------------------------
    void EquivalenceSet::advance_version_numbers(FieldMask advance_mask)
    //--------------------------------------------------------------------------
    {
      std::vector<VersionID> to_remove; 
      for (LegionMap<VersionID,FieldMask>::aligned::iterator it = 
            version_numbers.begin(); it != version_numbers.end(); it++)
      {
        const FieldMask overlap = it->second & advance_mask;
        if (!overlap)
          continue;
        LegionMap<VersionID,FieldMask>::aligned::iterator finder = 
          version_numbers.find(it->first + 1);
        if (finder == version_numbers.end())
          version_numbers[it->first + 1] = overlap;
        else
          finder->second |= overlap;
        it->second -= overlap;
        if (!it->second)
          to_remove.push_back(it->first);
        advance_mask -= overlap;
        if (!advance_mask)
          break;
      }
      if (!to_remove.empty())
      {
        for (std::vector<VersionID>::const_iterator it = 
              to_remove.begin(); it != to_remove.end(); it++)
          version_numbers.erase(*it);
      }
      if (!!advance_mask)
        version_numbers[init_version] = advance_mask;
    }

    //--------------------------------------------------------------------------
    void EquivalenceSet::perform_refinements(void)
    //--------------------------------------------------------------------------
    {
      RtUserEvent to_trigger;
      std::vector<RefinementThunk*> to_perform;
      do 
      {
        std::vector<EquivalenceSet*> to_add;
        for (std::vector<RefinementThunk*>::const_iterator it = 
              to_perform.begin(); it != to_perform.end(); it++)
        {
#ifdef DEBUG_LEGION
          assert((*it)->owner == this);
#endif
          EquivalenceSet *result = (*it)->perform_refinement();
          // Add our resource reference too
          result->add_nested_resource_ref(did);
          to_add.push_back(result);
        }
        AutoLock eq(eq_lock);
#ifdef DEBUG_LEGION
        assert(is_owner());
        assert(eq_state == REFINING_STATE);
#endif
        if (!to_add.empty())
          subsets.insert(subsets.end(), to_add.begin(), to_add.end());
        // Add these refinements to our subsets and delete the thunks
        if (!to_perform.empty())
        {
          for (std::vector<RefinementThunk*>::const_iterator it = 
                to_perform.begin(); it != to_perform.end(); it++)
            if ((*it)->remove_reference())
              delete (*it);
          to_perform.clear();
        }
        if (pending_refinements.empty())
        {
          // TODO: If we have too many subsets we need to make intermediates

          // This is the end of the refinement task so we need to update
          // the state and send out any notifications to anyone that the
          // refinements are done
          if (!remote_subsets.empty())
          {
            Serializer rez;
            {
              RezCheck z(rez);
              rez.serialize(did);
              rez.serialize<size_t>(subsets.size());
              for (std::vector<EquivalenceSet*>::const_iterator it = 
                    subsets.begin(); it != subsets.end(); it++)
                rez.serialize((*it)->did);
            }
            for (std::set<AddressSpaceID>::const_iterator it = 
                  remote_subsets.begin(); it != remote_subsets.end(); it++)
              runtime->send_equivalence_set_subset_response(*it, rez);
          }
          // Check to see if we no longer have any more refeinements
          // to perform. If not, then we need to clear our data structures
          // and remove any valid references that we might be holding
          if (unrefined_remainder == NULL)
          {
            if (!valid_instances.empty())
            {
              for (FieldMaskSet<LogicalView>::const_iterator it = 
                    valid_instances.begin(); it != valid_instances.end(); it++)
                if (it->first->remove_nested_valid_ref(did))
                  delete it->first;
            }
            if (!reduction_instances.empty())
            {
              for (std::map<unsigned,std::vector<ReductionView*> >::
                    const_iterator rit = reduction_instances.begin();
                    rit != reduction_instances.end(); rit++)
              {
                for (std::vector<ReductionView*>::const_iterator it = 
                      rit->second.begin(); it != rit->second.end(); it++)
                  if ((*it)->remove_nested_valid_ref(did))
                    delete (*it);
              }
              reduction_fields.clear();
            }
            if (!restricted_instances.empty())
            {
              for (FieldMaskSet<InstanceView>::const_iterator it = 
                    restricted_instances.begin(); it != 
                    restricted_instances.end(); it++)
                if (it->first->remove_nested_valid_ref(did))
                  delete it->first;
              restricted_fields.clear();
            }
            version_numbers.clear();
          }
          // Go back to the refined state and trigger our done event
          eq_state = REFINED_STATE;
          if (transition_event.exists())
          {
            to_trigger = transition_event;
            transition_event = RtUserEvent::NO_RT_USER_EVENT;
          }
        }
        else // there are more refinements to do so we go around again
          to_perform.swap(pending_refinements);
      } while (!to_perform.empty());
      if (to_trigger.exists())
        Runtime::trigger_event(to_trigger);
    }

    //--------------------------------------------------------------------------
    void EquivalenceSet::send_equivalence_set(AddressSpaceID target)
    //--------------------------------------------------------------------------
    {
#ifdef DEBUG_LEGION
      assert(is_owner());
      // We should have had a request for this already
      assert(!has_remote_instance(target));
#endif
      update_remote_instances(target);
      Serializer rez;
      {
        RezCheck z(rez);
        rez.serialize(did);
        set_expr->pack_expression(rez, target);
      }
      runtime->send_equivalence_set_response(target, rez);
    }

    //--------------------------------------------------------------------------
    void EquivalenceSet::add_pending_refinement(RefinementThunk *thunk)
    //--------------------------------------------------------------------------
    {
#ifdef DEBUG_LEGION
      assert(is_owner());
#endif
      thunk->add_reference();
      pending_refinements.push_back(thunk);
      if (eq_state == MAPPING_STATE)
      {
        // Check to see if we can transition right now
        if (mapping_guards.empty())
        {
          // Transition straight to refinement
          eq_state = REFINED_STATE;
          launch_refinement_task();
        }
        else // go to the pending state
          eq_state = PENDING_REFINED_STATE;
      }
      // Otherwise if we don't have an outstanding refinement task
      // then we need to launch one now to handle this
      else if (eq_state == REFINED_STATE)
        launch_refinement_task();
    }

    //--------------------------------------------------------------------------
    void EquivalenceSet::launch_refinement_task(void)
    //--------------------------------------------------------------------------
    {
#ifdef DEBUG_LEGION
      assert(is_owner());
      assert(eq_state == REFINED_STATE);
      assert(!pending_refinements.empty());
#endif
      // Mark that we're about to start refining
      eq_state = REFINING_STATE;
      // Send invalidations to all the remote nodes. This will invalidate
      // their mapping privileges and also send back any remote meta data
      // to the local node 
      std::set<RtEvent> refinement_preconditions;
      for (std::set<AddressSpaceID>::const_iterator it = 
            remote_subsets.begin(); it != remote_subsets.end(); it++)
      {
        Serializer rez;
        {
          RezCheck z(rez);
          rez.serialize(did);
          RtUserEvent remote_done = Runtime::create_rt_user_event();
          rez.serialize(remote_done);
          refinement_preconditions.insert(remote_done);
        }
        runtime->send_equivalence_set_subset_invalidation(*it, rez);
      }
      // There are no more remote subsets
      remote_subsets.clear();
      // We now hold all the fields in exclusive mode
      exclusive_fields |= shared_fields;
      shared_fields.clear();
      exclusive_copies.clear();
      shared_copies.clear();
      RefinementTaskArgs args(this);      
      if (!refinement_preconditions.empty())
      {
        RtEvent wait_for = Runtime::merge_events(refinement_preconditions);
        runtime->issue_runtime_meta_task(args, 
            LG_THROUGHPUT_DEFERRED_PRIORITY, wait_for);
      }
      else
        runtime->issue_runtime_meta_task(args, LG_THROUGHPUT_DEFERRED_PRIORITY);
    }

    //--------------------------------------------------------------------------
    void EquivalenceSet::process_subset_request(AddressSpaceID source,
                                                bool needs_lock/*=true*/)
    //--------------------------------------------------------------------------
    {
      if (needs_lock)
      {
        AutoLock eq(eq_lock);
        process_subset_request(source, false/*needs lock*/);
        return;
      }
#ifdef DEBUG_LEGION
      assert(is_owner());
      assert(remote_subsets.find(source) == remote_subsets.end());
#endif
      // First check to see if we need to complete this refinement
      if (unrefined_remainder != NULL)
      {
        RemoteComplete *refinement = 
          new RemoteComplete(unrefined_remainder, this, source);
        // We can clear the unrefined remainder now
        unrefined_remainder = NULL;
        add_pending_refinement(refinement);
        // We can just return since the refinement will send the
        // response after it's been done
        return;
      }
      // Add ourselves as a remote location for the subsets
      remote_subsets.insert(source);
      // We can only send the response if we're not doing any refinements 
      if ((eq_state != REFINING_STATE) && pending_refinements.empty())
      {
        Serializer rez;
        {
          RezCheck z(rez);
          rez.serialize(did);
          rez.serialize<size_t>(subsets.size());
          for (std::vector<EquivalenceSet*>::const_iterator it = 
                subsets.begin(); it != subsets.end(); it++)
            rez.serialize((*it)->did);
        }
        runtime->send_equivalence_set_subset_response(source, rez);
      }
    }

    //--------------------------------------------------------------------------
    void EquivalenceSet::process_subset_response(Deserializer &derez)
    //--------------------------------------------------------------------------
    {
      AutoLock eq(eq_lock);
#ifdef DEBUG_LEGION
      assert(!is_owner());
      assert(subsets.empty());
      assert(eq_state == PENDING_VALID_STATE);
      assert(transition_event.exists());
      assert(!transition_event.has_triggered());
#endif
      size_t num_subsets;
      derez.deserialize(num_subsets);
      std::set<RtEvent> wait_for;
      if (num_subsets > 0)
      {
        subsets.resize(num_subsets);
        for (unsigned idx = 0; idx < num_subsets; idx++)
        {
          DistributedID subdid;
          derez.deserialize(subdid);
          RtEvent ready;
          subsets[idx] = 
            runtime->find_or_request_equivalence_set(subdid, ready);
          if (ready.exists())
            wait_for.insert(ready);
        }
      }
      if (!wait_for.empty())
      {
        // This has to block in case there is an invalidation that comes
        // after it in the same virtual channel and we need to maintain
        // the ordering of those two operations.
        RtEvent wait_on = Runtime::merge_events(wait_for);
        if (wait_on.exists() && !wait_on.has_triggered())
        {
          eq.release();
          wait_on.wait();
          eq.reacquire();
        }
      }
      // Add our references
      for (std::vector<EquivalenceSet*>::const_iterator it = 
            subsets.begin(); it != subsets.end(); it++)
        (*it)->add_nested_resource_ref(did);
      // Update the state
      eq_state = VALID_STATE;
      // Trigger the transition state to wake up any waiters
      Runtime::trigger_event(transition_event);
      transition_event = RtUserEvent::NO_RT_USER_EVENT;
    }

    //--------------------------------------------------------------------------
    void EquivalenceSet::process_subset_invalidation(RtUserEvent to_trigger)
    //--------------------------------------------------------------------------
    {
      AutoLock eq(eq_lock);
#ifdef DEBUG_LEGION
      assert(!is_owner());
      assert(eq_state == VALID_STATE);
      assert(!transition_event.exists());
#endif
      // Check to see if we have any mapping guards in place
      if (mapping_guards.empty())
      {
        // Update the state to reflect that we are now invalid
        eq_state = INVALID_STATE;
        invalidate_remote_state(to_trigger);
      }
      else
      {
        // Update the state and save the event to trigger
        eq_state = PENDING_INVALID_STATE;
        transition_event = to_trigger;
      }
    }

    //--------------------------------------------------------------------------
    void EquivalenceSet::invalidate_remote_state(RtUserEvent to_trigger)
    //--------------------------------------------------------------------------
    {
#ifdef DEBUG_LEGION
      assert(!is_owner());
      assert(to_trigger.exists());
      assert(eq_state == INVALID_STATE);
#endif
      if (!subsets.empty())
      {
        // Easy case, just invalidate our local subsets and then
        // we can trigger the event
        for (std::vector<EquivalenceSet*>::const_iterator it = 
              subsets.begin(); it != subsets.end(); it++)
          if ((*it)->remove_nested_resource_ref(did))
            delete (*it);
        subsets.clear();
        Runtime::trigger_event(to_trigger);
      }
      else
      {
        // We had a mapping lease so we need to send back any state
        // to the owner node in order to be considered invalidated
        // TODO
        assert(false);
      }
    }

    //--------------------------------------------------------------------------
    /*static*/ void EquivalenceSet::handle_refinement(const void *args)
    //--------------------------------------------------------------------------
    {
      const RefinementTaskArgs *rargs = (const RefinementTaskArgs*)args;
      rargs->target->perform_refinements();
    }

    //--------------------------------------------------------------------------
    /*static*/ void EquivalenceSet::handle_equivalence_set_request(
                   Deserializer &derez, Runtime *runtime, AddressSpaceID source)
    //--------------------------------------------------------------------------
    {
      DerezCheck z(derez);
      DistributedID did;
      derez.deserialize(did);
      DistributedCollectable *dc = runtime->find_distributed_collectable(did);
#ifdef DEBUG_LEGION
      EquivalenceSet *set = dynamic_cast<EquivalenceSet*>(dc);
      assert(set != NULL);
#else
      EquivalenceSet *set = static_cast<EquivalenceSet*>(dc);
#endif
      set->send_equivalence_set(source);
    }

    //--------------------------------------------------------------------------
    /*static*/ void EquivalenceSet::handle_equivalence_set_response(
                   Deserializer &derez, Runtime *runtime, AddressSpaceID source)
    //--------------------------------------------------------------------------
    {
      DerezCheck z(derez);
      DistributedID did;
      derez.deserialize(did);
      IndexSpaceExpression *expr = 
        IndexSpaceExpression::unpack_expression(derez, runtime->forest, source);
      void *location;
      EquivalenceSet *set = NULL;
      if (runtime->find_pending_collectable_location(did, location))
        set = new(location) EquivalenceSet(runtime, did, source, 
                                           expr, false/*register now*/);
      else
        set = new EquivalenceSet(runtime, did, source, 
                                 expr, false/*register now*/);
      // Once construction is complete then we do the registration
      set->register_with_runtime(NULL/*no remote registration needed*/);
    }

    //--------------------------------------------------------------------------
    /*static*/ void EquivalenceSet::handle_subset_request(
                   Deserializer &derez, Runtime *runtime, AddressSpaceID source)
    //--------------------------------------------------------------------------
    {
      DerezCheck z(derez);
      DistributedID did;
      derez.deserialize(did);
      DistributedCollectable *dc = runtime->find_distributed_collectable(did);
#ifdef DEBUG_LEGION
      EquivalenceSet *set = dynamic_cast<EquivalenceSet*>(dc);
      assert(set != NULL);
#else
      EquivalenceSet *set = static_cast<EquivalenceSet*>(dc);
#endif
      set->process_subset_request(source);
    }

    //--------------------------------------------------------------------------
    /*static*/ void EquivalenceSet::handle_subset_response(
                                          Deserializer &derez, Runtime *runtime)
    //--------------------------------------------------------------------------
    {
      DerezCheck z(derez);
      DistributedID did;
      derez.deserialize(did);
      DistributedCollectable *dc = runtime->find_distributed_collectable(did);
#ifdef DEBUG_LEGION
      EquivalenceSet *set = dynamic_cast<EquivalenceSet*>(dc);
      assert(set != NULL);
#else
      EquivalenceSet *set = static_cast<EquivalenceSet*>(dc);
#endif
      set->process_subset_response(derez);
    }

    //--------------------------------------------------------------------------
    /*static*/ void EquivalenceSet::handle_subset_invalidation(
                                          Deserializer &derez, Runtime *runtime)
    //--------------------------------------------------------------------------
    {
      DerezCheck z(derez);
      DistributedID did;
      derez.deserialize(did);
      RtUserEvent to_trigger;
      derez.deserialize(to_trigger);
      DistributedCollectable *dc = runtime->find_distributed_collectable(did);
#ifdef DEBUG_LEGION
      EquivalenceSet *set = dynamic_cast<EquivalenceSet*>(dc);
      assert(set != NULL);
#else
      EquivalenceSet *set = static_cast<EquivalenceSet*>(dc);
#endif
      set->process_subset_invalidation(to_trigger);
    }

    //--------------------------------------------------------------------------
    /*static*/ void EquivalenceSet::handle_ray_trace_request(
                   Deserializer &derez, Runtime *runtime, AddressSpaceID source)
    //--------------------------------------------------------------------------
    {
      DerezCheck z(derez);
      DistributedID did;
      derez.deserialize(did);
      VersionManager *target;
      derez.deserialize(target);
      IndexSpaceExpression *expr = 
        IndexSpaceExpression::unpack_expression(derez, runtime->forest, source);
      AddressSpaceID origin;
      derez.deserialize(origin);
      RtUserEvent done_event;
      derez.deserialize(done_event);

      DistributedCollectable *dc = runtime->find_distributed_collectable(did);
#ifdef DEBUG_LEGION
      EquivalenceSet *set = dynamic_cast<EquivalenceSet*>(dc);
      assert(set != NULL);
#else
      EquivalenceSet *set = static_cast<EquivalenceSet*>(dc);
#endif
      RtEvent done = set->ray_trace_equivalence_sets(target, expr, origin);
      Runtime::trigger_event(done_event, done);
    }

    //--------------------------------------------------------------------------
    /*static*/ void EquivalenceSet::handle_ray_trace_response(
                                          Deserializer &derez, Runtime *runtime)
    //--------------------------------------------------------------------------
    {
      DerezCheck z(derez);
      DistributedID did;
      derez.deserialize(did);
      VersionManager *target;
      derez.deserialize(target);
      RtUserEvent done_event;
      derez.deserialize(done_event);

      RtEvent ready;
      EquivalenceSet *set = runtime->find_or_request_equivalence_set(did,ready);
      target->record_equivalence_set(set);
      Runtime::trigger_event(done_event, ready);
    }

    //--------------------------------------------------------------------------
    /*static*/ void EquivalenceSet::handle_create_remote_request(
                   Deserializer &derez, Runtime *runtime, AddressSpaceID source)
    //--------------------------------------------------------------------------
    {
      DerezCheck z(derez);
      RefinementThunk *thunk;
      derez.deserialize(thunk);
      IndexSpaceExpression *expr = 
        IndexSpaceExpression::unpack_expression(derez, runtime->forest, source);

      DistributedID did = runtime->get_available_distributed_id();
      EquivalenceSet *result = new EquivalenceSet(runtime, did, 
                                runtime->address_space, expr, true/*register*/);
      Serializer rez;
      {
        RezCheck z2(rez);
        rez.serialize(thunk);
        rez.serialize(result->did);
      }
      runtime->send_equivalence_set_create_remote_response(source, rez);
    }

    //--------------------------------------------------------------------------
    /*static*/ void EquivalenceSet::handle_create_remote_response(
                                          Deserializer &derez, Runtime *runtime)
    //--------------------------------------------------------------------------
    {
      DerezCheck z(derez);
      RefinementThunk *thunk;
      derez.deserialize(thunk);
      DistributedID did;
      derez.deserialize(did);

      RtEvent ready;
      EquivalenceSet *result = 
        runtime->find_or_request_equivalence_set(did, ready);
      thunk->record_refinement(result, ready);
    }

    //--------------------------------------------------------------------------
    /*static*/ void EquivalenceSet::handle_valid_request(Deserializer &derez,
                                        Runtime *runtime, AddressSpaceID source)  
    //--------------------------------------------------------------------------
    {
      DerezCheck z(derez);
      DistributedID did;
      derez.deserialize(did);
      RtEvent ready_event;
      EquivalenceSet *set = 
        runtime->find_or_request_equivalence_set(did, ready_event);
      FieldMask request_mask;
      derez.deserialize(request_mask);
      RegionUsage usage;
      derez.deserialize(usage);
      PendingRequest *pending_request;
      derez.deserialize(pending_request);

      std::set<RtEvent> fake_ready, fake_applied;
      if (ready_event.exists() && !ready_event.has_triggered())
        ready_event.wait();
      set->request_valid_copy(request_mask, usage, fake_ready,
                              fake_applied, source, pending_request);
#ifdef DEBUG_LEGION
      assert(fake_ready.empty());
      assert(fake_applied.empty());
#endif
    }

    //--------------------------------------------------------------------------
    /*static*/ void EquivalenceSet::handle_valid_response(Deserializer &derez,
                                                          Runtime *runtime)
    //--------------------------------------------------------------------------
    {
      DerezCheck z(derez);
      DistributedID did;
      derez.deserialize(did);
      RtEvent ready_event;
      EquivalenceSet *set = 
        runtime->find_or_request_equivalence_set(did, ready_event);
      PendingRequest *pending_request;
      derez.deserialize(pending_request);
      unsigned pending_updates;
      derez.deserialize(pending_updates);
      derez.deserialize(pending_request->owner_mask);

      if (ready_event.exists() && !ready_event.has_triggered())
        ready_event.wait();
      set->record_pending_updates(pending_request, pending_updates, true);
    }

    //--------------------------------------------------------------------------
    /*static*/ void EquivalenceSet::handle_update_request(Deserializer &derez,
                                                          Runtime *runtime)
    //--------------------------------------------------------------------------
    {
      DerezCheck z(derez);
      DistributedID did;
      derez.deserialize(did);
      RtEvent ready_event;
      EquivalenceSet *set = 
        runtime->find_or_request_equivalence_set(did, ready_event);
      AddressSpaceID invalid_space;
      derez.deserialize(invalid_space);
      FieldMask update_mask;
      derez.deserialize(update_mask);
      PendingRequest *pending_request;
      derez.deserialize(pending_request);
      bool invalidate;
      derez.deserialize(invalidate);
      ReductionOpID skip_redop;
      derez.deserialize(skip_redop);

      if (ready_event.exists() && !ready_event.has_triggered())
        ready_event.wait();
      unsigned dummy_updates = 0;
      set->request_update(runtime->address_space, invalid_space,
                          update_mask, pending_request, dummy_updates,
                          invalidate, skip_redop, true/*needs lock*/);
#ifdef DEBUG_LEGION
      assert(dummy_updates == 1);
#endif
    }

    //--------------------------------------------------------------------------
    /*static*/ void EquivalenceSet::handle_update_response(Deserializer &derez,
                                                           Runtime *runtime)
    //--------------------------------------------------------------------------
    {
      DerezCheck z(derez);
      DistributedID did;
      derez.deserialize(did);
      RtEvent ready_event;
      EquivalenceSet *set = 
        runtime->find_or_request_equivalence_set(did, ready_event);
      PendingRequest *pending_request;
      derez.deserialize(pending_request);

      if (ready_event.exists() && !ready_event.has_triggered())
        ready_event.wait();
      set->process_update_response(pending_request, derez);
    }

    //--------------------------------------------------------------------------
    /*static*/ void EquivalenceSet::handle_invalidate_request(
                                          Deserializer &derez, Runtime *runtime)
    //--------------------------------------------------------------------------
    {
      DerezCheck z(derez);
      DistributedID did;
      derez.deserialize(did);
      RtEvent ready_event;
      EquivalenceSet *set = 
        runtime->find_or_request_equivalence_set(did, ready_event);
      AddressSpaceID source;
      derez.deserialize(source);
      FieldMask invalidate_mask;
      derez.deserialize(invalidate_mask);
      PendingRequest *pending_request;
      derez.deserialize(pending_request);
      bool meta_only;
      derez.deserialize(meta_only);

      if (ready_event.exists() && !ready_event.has_triggered())
        ready_event.wait();
      unsigned dummy_updates = 0;
      set->request_invalidate(runtime->address_space, source, invalidate_mask,
                pending_request, dummy_updates, meta_only, true/*needs lock*/);
#ifdef DEBUG_LEGION
      assert(dummy_updates == 1);
#endif
    }

    //--------------------------------------------------------------------------
    /*static*/ void EquivalenceSet::handle_invalidate_response(
                                          Deserializer &derez, Runtime *runtime)
    //--------------------------------------------------------------------------
    {
      DerezCheck z(derez);
      DistributedID did;
      derez.deserialize(did);
      RtEvent ready_event;
      EquivalenceSet *set = 
        runtime->find_or_request_equivalence_set(did, ready_event);
      PendingRequest *pending_request;
      derez.deserialize(pending_request);

      if (ready_event.exists() && !ready_event.has_triggered())
        ready_event.wait();
      set->record_invalidation(pending_request, true/*needs lock*/);
    }

    /////////////////////////////////////////////////////////////
    // Version Manager 
    ///////////////////////////////////////////////////////////// 

    //--------------------------------------------------------------------------
    VersionManager::VersionManager(RegionTreeNode *n, ContextID c)
      : ctx(c), node(n), runtime(n->context->runtime),
        has_equivalence_sets(false)
    //--------------------------------------------------------------------------
    {
    }

    //--------------------------------------------------------------------------
    VersionManager::VersionManager(const VersionManager &rhs)
      : ctx(rhs.ctx), node(rhs.node), runtime(rhs.runtime)
    //--------------------------------------------------------------------------
    {
      // should never be called
      assert(false);
    }

    //--------------------------------------------------------------------------
    VersionManager::~VersionManager(void)
    //--------------------------------------------------------------------------
    {
    }

    //--------------------------------------------------------------------------
    VersionManager& VersionManager::operator=(const VersionManager &rhs)
    //--------------------------------------------------------------------------
    {
      // should never be called
      assert(false);
      return *this;
    }

    //--------------------------------------------------------------------------
    void VersionManager::reset(void)
    //--------------------------------------------------------------------------
    {
      AutoLock m_lock(manager_lock);
      if (!equivalence_sets.empty())
      {
        for (std::set<EquivalenceSet*>::const_iterator it = 
              equivalence_sets.begin(); it != equivalence_sets.end(); it++)
        {
          if ((*it)->remove_base_resource_ref(VERSION_MANAGER_REF))
            delete (*it);
        }
        equivalence_sets.clear();
      }
#ifdef DEBUG_LEGION
      assert(!equivalence_sets_ready.exists() || 
              equivalence_sets_ready.has_triggered());
#endif
      equivalence_sets_ready = RtUserEvent::NO_RT_USER_EVENT;
      has_equivalence_sets = false;
    }

    //--------------------------------------------------------------------------
    void VersionManager::perform_versioning_analysis(InnerContext *context,
                                                     VersionInfo &version_info)
    //--------------------------------------------------------------------------
    {
      // If we don't have equivalence classes for this region yet we 
      // either need to compute them or request them from the owner
      RtEvent wait_on;
      bool compute_sets = false;
      if (!has_equivalence_sets)
      {
        // Retake the lock and see if we lost the race
        AutoLock m_lock(manager_lock);
        if (!has_equivalence_sets)
        {
          if (!equivalence_sets_ready.exists()) 
          {
            equivalence_sets_ready = Runtime::create_rt_user_event();
            compute_sets = true;
          }
          wait_on = equivalence_sets_ready;
        }
      }
      if (compute_sets)
      {
        IndexSpaceExpression *expr = node->get_index_space_expression();
        RtEvent ready = context->compute_equivalence_sets(this, 
            node->get_tree_id(), expr, runtime->address_space);
        Runtime::trigger_event(equivalence_sets_ready, ready);
      }
      // Wait if necessary for the results
      if (wait_on.exists() && !wait_on.has_triggered())
        wait_on.wait();
      // Possibly duplicate writes, but that is alright
      if (!has_equivalence_sets)
        has_equivalence_sets = true;
      // Now that we have the equivalence classes we can have them add
      // themselves in case they have been refined and we need to traverse
      for (std::set<EquivalenceSet*>::const_iterator it = 
            equivalence_sets.begin(); it != equivalence_sets.end(); it++)
        (*it)->perform_versioning_analysis(version_info); 
    }

    //--------------------------------------------------------------------------
    void VersionManager::record_equivalence_set(EquivalenceSet *set)
    //--------------------------------------------------------------------------
    {
      set->add_base_resource_ref(VERSION_MANAGER_REF);
      AutoLock m_lock(manager_lock);
#ifdef DEBUG_LEGION
      assert(equivalence_sets.find(set) == equivalence_sets.end());
#endif
      equivalence_sets.insert(set);
    }

    //--------------------------------------------------------------------------
    void VersionManager::print_physical_state(RegionTreeNode *node,
                                              const FieldMask &capture_mask,
                                              TreeStateLogger *logger)
    //--------------------------------------------------------------------------
    {
      logger->log("Equivalence Sets:");
      logger->down();
      // TODO: log equivalence sets
      assert(false);
      logger->up();
    }

    /////////////////////////////////////////////////////////////
    // RegionTreePath 
    /////////////////////////////////////////////////////////////

    //--------------------------------------------------------------------------
    RegionTreePath::RegionTreePath(void) 
      : min_depth(0), max_depth(0)
    //--------------------------------------------------------------------------
    {
    }

    //--------------------------------------------------------------------------
    void RegionTreePath::initialize(unsigned min, unsigned max)
    //--------------------------------------------------------------------------
    {
#ifdef DEBUG_LEGION
      assert(min <= max);
#endif
      min_depth = min;
      max_depth = max;
      path.resize(max_depth+1, INVALID_COLOR);
    }

    //--------------------------------------------------------------------------
    void RegionTreePath::register_child(unsigned depth, 
                                        const LegionColor color)
    //--------------------------------------------------------------------------
    {
#ifdef DEBUG_LEGION
      assert(min_depth <= depth);
      assert(depth <= max_depth);
#endif
      path[depth] = color;
    }

    //--------------------------------------------------------------------------
    void RegionTreePath::record_aliased_children(unsigned depth,
                                                 const FieldMask &mask)
    //--------------------------------------------------------------------------
    {
#ifdef DEBUG_LEGION
      assert(min_depth <= depth);
      assert(depth <= max_depth);
#endif
      LegionMap<unsigned,FieldMask>::aligned::iterator finder = 
        interfering_children.find(depth);
      if (finder == interfering_children.end())
        interfering_children[depth] = mask;
      else
        finder->second |= mask;
    }

    //--------------------------------------------------------------------------
    void RegionTreePath::clear(void)
    //--------------------------------------------------------------------------
    {
      path.clear();
      min_depth = 0;
      max_depth = 0;
    }

#ifdef DEBUG_LEGION
    //--------------------------------------------------------------------------
    bool RegionTreePath::has_child(unsigned depth) const
    //--------------------------------------------------------------------------
    {
      assert(min_depth <= depth);
      assert(depth <= max_depth);
      return (path[depth] != INVALID_COLOR);
    }

    //--------------------------------------------------------------------------
    LegionColor RegionTreePath::get_child(unsigned depth) const
    //--------------------------------------------------------------------------
    {
      assert(min_depth <= depth);
      assert(depth <= max_depth);
      assert(has_child(depth));
      return path[depth];
    }
#endif

    //--------------------------------------------------------------------------
    const FieldMask* RegionTreePath::get_aliased_children(unsigned depth) const
    //--------------------------------------------------------------------------
    {
      if (interfering_children.empty())
        return NULL;
      LegionMap<unsigned,FieldMask>::aligned::const_iterator finder = 
        interfering_children.find(depth);
      if (finder == interfering_children.end())
        return NULL;
      return &(finder->second);
    }

    /////////////////////////////////////////////////////////////
    // InstanceRef 
    /////////////////////////////////////////////////////////////

    //--------------------------------------------------------------------------
    InstanceRef::InstanceRef(bool comp)
      : ready_event(ApEvent::NO_AP_EVENT), manager(NULL), local(true)
    //--------------------------------------------------------------------------
    {
    }

    //--------------------------------------------------------------------------
    InstanceRef::InstanceRef(const InstanceRef &rhs)
      : valid_fields(rhs.valid_fields), ready_event(rhs.ready_event),
        manager(rhs.manager), local(rhs.local)
    //--------------------------------------------------------------------------
    {
    }

    //--------------------------------------------------------------------------
    InstanceRef::InstanceRef(PhysicalManager *man, const FieldMask &m,ApEvent r)
      : valid_fields(m), ready_event(r), manager(man), local(true)
    //--------------------------------------------------------------------------
    {
    }

    //--------------------------------------------------------------------------
    InstanceRef::~InstanceRef(void)
    //--------------------------------------------------------------------------
    {
    }

    //--------------------------------------------------------------------------
    InstanceRef& InstanceRef::operator=(const InstanceRef &rhs)
    //--------------------------------------------------------------------------
    {
      valid_fields = rhs.valid_fields;
      ready_event = rhs.ready_event;
      local = rhs.local;
      manager = rhs.manager;
      return *this;
    }

    //--------------------------------------------------------------------------
    bool InstanceRef::operator==(const InstanceRef &rhs) const
    //--------------------------------------------------------------------------
    {
      if (valid_fields != rhs.valid_fields)
        return false;
      if (ready_event != rhs.ready_event)
        return false;
      if (manager != rhs.manager)
        return false;
      return true;
    }

    //--------------------------------------------------------------------------
    bool InstanceRef::operator!=(const InstanceRef &rhs) const
    //--------------------------------------------------------------------------
    {
      return !(*this == rhs);
    }

    //--------------------------------------------------------------------------
    MappingInstance InstanceRef::get_mapping_instance(void) const
    //--------------------------------------------------------------------------
    {
      return MappingInstance(manager);
    }

    //--------------------------------------------------------------------------
    bool InstanceRef::is_virtual_ref(void) const
    //--------------------------------------------------------------------------
    {
      if (manager == NULL)
        return true;
      return manager->is_virtual_manager(); 
    }

    //--------------------------------------------------------------------------
    void InstanceRef::add_valid_reference(ReferenceSource source) const
    //--------------------------------------------------------------------------
    {
#ifdef DEBUG_LEGION
      assert(manager != NULL);
#endif
      manager->add_base_valid_ref(source);
    }

    //--------------------------------------------------------------------------
    void InstanceRef::remove_valid_reference(ReferenceSource source) const
    //--------------------------------------------------------------------------
    {
#ifdef DEBUG_LEGION
      assert(manager != NULL);
#endif
      if (manager->remove_base_valid_ref(source))
        delete manager;
    }

    //--------------------------------------------------------------------------
    Memory InstanceRef::get_memory(void) const
    //--------------------------------------------------------------------------
    {
#ifdef DEBUG_LEGION
      assert(manager != NULL);
#endif
      return manager->get_memory();
    }

    //--------------------------------------------------------------------------
    Reservation InstanceRef::get_read_only_reservation(void) const
    //--------------------------------------------------------------------------
    {
#ifdef DEBUG_LEGION
      assert(manager != NULL);
      assert(manager->is_instance_manager());
#endif
      return 
        manager->as_instance_manager()->get_read_only_mapping_reservation();
    }

    //--------------------------------------------------------------------------
    bool InstanceRef::is_field_set(FieldID fid) const
    //--------------------------------------------------------------------------
    {
#ifdef DEBUG_LEGION
      assert(manager != NULL);
#endif
      unsigned index = manager->field_space_node->get_field_index(fid);
      return valid_fields.is_set(index);
    }

    //--------------------------------------------------------------------------
    LegionRuntime::Accessor::RegionAccessor<
      LegionRuntime::Accessor::AccessorType::Generic> 
        InstanceRef::get_accessor(void) const
    //--------------------------------------------------------------------------
    {
#ifdef DEBUG_LEGION
      assert(manager != NULL);
#endif
      return manager->get_accessor();
    }

    //--------------------------------------------------------------------------
    LegionRuntime::Accessor::RegionAccessor<
      LegionRuntime::Accessor::AccessorType::Generic>
        InstanceRef::get_field_accessor(FieldID fid) const
    //--------------------------------------------------------------------------
    {
#ifdef DEBUG_LEGION
      assert(manager != NULL);
#endif
      return manager->get_field_accessor(fid);
    }

    //--------------------------------------------------------------------------
    void InstanceRef::pack_reference(Serializer &rez) const
    //--------------------------------------------------------------------------
    {
      rez.serialize(valid_fields);
      rez.serialize(ready_event);
      if (manager != NULL)
        rez.serialize(manager->did);
      else
        rez.serialize<DistributedID>(0);
    }

    //--------------------------------------------------------------------------
    void InstanceRef::unpack_reference(Runtime *runtime,
                                       Deserializer &derez, RtEvent &ready)
    //--------------------------------------------------------------------------
    {
      derez.deserialize(valid_fields);
      derez.deserialize(ready_event);
      DistributedID did;
      derez.deserialize(did);
      if (did == 0)
        return;
      manager = runtime->find_or_request_physical_manager(did, ready);
      local = false;
    } 

    /////////////////////////////////////////////////////////////
    // InstanceSet 
    /////////////////////////////////////////////////////////////
    
    //--------------------------------------------------------------------------
    InstanceSet::CollectableRef& InstanceSet::CollectableRef::operator=(
                                         const InstanceSet::CollectableRef &rhs)
    //--------------------------------------------------------------------------
    {
      valid_fields = rhs.valid_fields;
      ready_event = rhs.ready_event;
      local = rhs.local;
      manager = rhs.manager;
      return *this;
    }

    //--------------------------------------------------------------------------
    InstanceSet::InstanceSet(size_t init_size /*=0*/)
      : single((init_size <= 1)), shared(false)
    //--------------------------------------------------------------------------
    {
      if (init_size == 0)
        refs.single = NULL;
      else if (init_size == 1)
      {
        refs.single = new CollectableRef();
        refs.single->add_reference();
      }
      else
      {
        refs.multi = new InternalSet(init_size);
        refs.multi->add_reference();
      }
    }

    //--------------------------------------------------------------------------
    InstanceSet::InstanceSet(const InstanceSet &rhs)
      : single(rhs.single)
    //--------------------------------------------------------------------------
    {
      // Mark that the other one is sharing too
      if (single)
      {
        refs.single = rhs.refs.single;
        if (refs.single == NULL)
        {
          shared = false;
          return;
        }
        shared = true;
        rhs.shared = true;
        refs.single->add_reference();
      }
      else
      {
        refs.multi = rhs.refs.multi;
        shared = true;
        rhs.shared = true;
        refs.multi->add_reference();
      }
    }

    //--------------------------------------------------------------------------
    InstanceSet::~InstanceSet(void)
    //--------------------------------------------------------------------------
    {
      if (single)
      {
        if ((refs.single != NULL) && refs.single->remove_reference())
          delete (refs.single);
      }
      else
      {
        if (refs.multi->remove_reference())
          delete refs.multi;
      }
    }

    //--------------------------------------------------------------------------
    InstanceSet& InstanceSet::operator=(const InstanceSet &rhs)
    //--------------------------------------------------------------------------
    {
      // See if we need to delete our current one
      if (single)
      {
        if ((refs.single != NULL) && refs.single->remove_reference())
          delete (refs.single);
      }
      else
      {
        if (refs.multi->remove_reference())
          delete refs.multi;
      }
      // Now copy over the other one
      single = rhs.single; 
      if (single)
      {
        refs.single = rhs.refs.single;
        if (refs.single != NULL)
        {
          shared = true;
          rhs.shared = true;
          refs.single->add_reference();
        }
        else
          shared = false;
      }
      else
      {
        refs.multi = rhs.refs.multi;
        shared = true;
        rhs.shared = true;
        refs.multi->add_reference();
      }
      return *this;
    }

    //--------------------------------------------------------------------------
    void InstanceSet::make_copy(void)
    //--------------------------------------------------------------------------
    {
#ifdef DEBUG_LEGION
      assert(shared);
#endif
      if (single)
      {
        if (refs.single != NULL)
        {
          CollectableRef *next = 
            new CollectableRef(*refs.single);
          next->add_reference();
          if (refs.single->remove_reference())
            delete (refs.single);
          refs.single = next;
        }
      }
      else
      {
        InternalSet *next = new InternalSet(*refs.multi);
        next->add_reference();
        if (refs.multi->remove_reference())
          delete refs.multi;
        refs.multi = next;
      }
      shared = false;
    }

    //--------------------------------------------------------------------------
    bool InstanceSet::operator==(const InstanceSet &rhs) const
    //--------------------------------------------------------------------------
    {
      if (single != rhs.single)
        return false;
      if (single)
      {
        if (refs.single == rhs.refs.single)
          return true;
        if (((refs.single == NULL) && (rhs.refs.single != NULL)) ||
            ((refs.single != NULL) && (rhs.refs.single == NULL)))
          return false;
        return ((*refs.single) == (*rhs.refs.single));
      }
      else
      {
        if (refs.multi->vector.size() != rhs.refs.multi->vector.size())
          return false;
        for (unsigned idx = 0; idx < refs.multi->vector.size(); idx++)
        {
          if (refs.multi->vector[idx] != rhs.refs.multi->vector[idx])
            return false;
        }
        return true;
      }
    }

    //--------------------------------------------------------------------------
    bool InstanceSet::operator!=(const InstanceSet &rhs) const
    //--------------------------------------------------------------------------
    {
      return !((*this) == rhs);
    }

    //--------------------------------------------------------------------------
    InstanceRef& InstanceSet::operator[](unsigned idx)
    //--------------------------------------------------------------------------
    {
      if (shared)
        make_copy();
      if (single)
      {
#ifdef DEBUG_LEGION
        assert(idx == 0);
        assert(refs.single != NULL);
#endif
        return *(refs.single);
      }
#ifdef DEBUG_LEGION
      assert(idx < refs.multi->vector.size());
#endif
      return refs.multi->vector[idx];
    }

    //--------------------------------------------------------------------------
    const InstanceRef& InstanceSet::operator[](unsigned idx) const
    //--------------------------------------------------------------------------
    {
      // No need to make a copy if shared here since this is read-only
      if (single)
      {
#ifdef DEBUG_LEGION
        assert(idx == 0);
        assert(refs.single != NULL);
#endif
        return *(refs.single);
      }
#ifdef DEBUG_LEGION
      assert(idx < refs.multi->vector.size());
#endif
      return refs.multi->vector[idx];
    }

    //--------------------------------------------------------------------------
    bool InstanceSet::empty(void) const
    //--------------------------------------------------------------------------
    {
      if (single && (refs.single == NULL))
        return true;
      else if (!single && refs.multi->empty())
        return true;
      return false;
    }

    //--------------------------------------------------------------------------
    size_t InstanceSet::size(void) const
    //--------------------------------------------------------------------------
    {
      if (single)
      {
        if (refs.single == NULL)
          return 0;
        return 1;
      }
      if (refs.multi == NULL)
        return 0;
      return refs.multi->vector.size();
    }

    //--------------------------------------------------------------------------
    void InstanceSet::resize(size_t new_size)
    //--------------------------------------------------------------------------
    {
      if (single)
      {
        if (new_size == 0)
        {
          if ((refs.single != NULL) && refs.single->remove_reference())
            delete (refs.single);
          refs.single = NULL;
          shared = false;
        }
        else if (new_size > 1)
        {
          // Switch to multi
          InternalSet *next = new InternalSet(new_size);
          if (refs.single != NULL)
          {
            next->vector[0] = *(refs.single);
            if (refs.single->remove_reference())
              delete (refs.single);
          }
          next->add_reference();
          refs.multi = next;
          single = false;
          shared = false;
        }
        else if (refs.single == NULL)
        {
          // New size is 1 but we were empty before
          CollectableRef *next = new CollectableRef();
          next->add_reference();
          refs.single = next;
          single = true;
          shared = false;
        }
      }
      else
      {
        if (new_size == 0)
        {
          if (refs.multi->remove_reference())
            delete refs.multi;
          refs.single = NULL;
          single = true;
          shared = false;
        }
        else if (new_size == 1)
        {
          CollectableRef *next = 
            new CollectableRef(refs.multi->vector[0]);
          if (refs.multi->remove_reference())
            delete (refs.multi);
          next->add_reference();
          refs.single = next;
          single = true;
          shared = false;
        }
        else
        {
          size_t current_size = refs.multi->vector.size();
          if (current_size != new_size)
          {
            if (shared)
            {
              // Make a copy
              InternalSet *next = new InternalSet(new_size);
              // Copy over the elements
              for (unsigned idx = 0; idx < 
                   ((current_size < new_size) ? current_size : new_size); idx++)
                next->vector[idx] = refs.multi->vector[idx];
              if (refs.multi->remove_reference())
                delete refs.multi;
              next->add_reference();
              refs.multi = next;
              shared = false;
            }
            else
            {
              // Resize our existing vector
              refs.multi->vector.resize(new_size);
            }
          }
          // Size is the same so there is no need to do anything
        }
      }
    }

    //--------------------------------------------------------------------------
    void InstanceSet::clear(void)
    //--------------------------------------------------------------------------
    {
      // No need to copy since we are removing our references and not mutating
      if (single)
      {
        if ((refs.single != NULL) && refs.single->remove_reference())
          delete (refs.single);
        refs.single = NULL;
      }
      else
      {
        if (shared)
        {
          // Small optimization here, if we're told to delete it, we know
          // that means we were the last user so we can re-use it
          if (refs.multi->remove_reference())
          {
            // Put a reference back on it since we're reusing it
            refs.multi->add_reference();
            refs.multi->vector.clear();
          }
          else
          {
            // Go back to single
            refs.multi = NULL;
            single = true;
          }
        }
        else
          refs.multi->vector.clear();
      }
      shared = false;
    }

    //--------------------------------------------------------------------------
    void InstanceSet::add_instance(const InstanceRef &ref)
    //--------------------------------------------------------------------------
    {
      if (single)
      {
        // No need to check for shared, we're going to make new things anyway
        if (refs.single != NULL)
        {
          // Make the new multi version
          InternalSet *next = new InternalSet(2);
          next->vector[0] = *(refs.single);
          next->vector[1] = ref;
          if (refs.single->remove_reference())
            delete (refs.single);
          next->add_reference();
          refs.multi = next;
          single = false;
          shared = false;
        }
        else
        {
          refs.single = new CollectableRef(ref);
          refs.single->add_reference();
        }
      }
      else
      {
        if (shared)
          make_copy();
        refs.multi->vector.push_back(ref);
      }
    }

    //--------------------------------------------------------------------------
    bool InstanceSet::is_virtual_mapping(void) const
    //--------------------------------------------------------------------------
    {
      if (empty())
        return true;
      if (size() > 1)
        return false;
      return refs.single->is_virtual_ref();
    }

    //--------------------------------------------------------------------------
    void InstanceSet::pack_references(Serializer &rez) const
    //--------------------------------------------------------------------------
    {
      if (single)
      {
        if (refs.single == NULL)
        {
          rez.serialize<size_t>(0);
          return;
        }
        rez.serialize<size_t>(1);
        refs.single->pack_reference(rez);
      }
      else
      {
        rez.serialize<size_t>(refs.multi->vector.size());
        for (unsigned idx = 0; idx < refs.multi->vector.size(); idx++)
          refs.multi->vector[idx].pack_reference(rez);
      }
    }

    //--------------------------------------------------------------------------
    void InstanceSet::unpack_references(Runtime *runtime, Deserializer &derez, 
                                        std::set<RtEvent> &ready_events)
    //--------------------------------------------------------------------------
    {
      size_t num_refs;
      derez.deserialize(num_refs);
      if (num_refs == 0)
      {
        // No matter what, we can just clear out any references we have
        if (single)
        {
          if ((refs.single != NULL) && refs.single->remove_reference())
            delete (refs.single);
          refs.single = NULL;
        }
        else
        {
          if (refs.multi->remove_reference())
            delete refs.multi;
          single = true;
        }
      }
      else if (num_refs == 1)
      {
        // If we're in multi, go back to single
        if (!single)
        {
          if (refs.multi->remove_reference())
            delete refs.multi;
          refs.multi = NULL;
          single = true;
        }
        // Now we can unpack our reference, see if we need to make one
        if (refs.single == NULL)
        {
          refs.single = new CollectableRef();
          refs.single->add_reference();
        }
        RtEvent ready;
        refs.single->unpack_reference(runtime, derez, ready);
        if (ready.exists())
          ready_events.insert(ready);
      }
      else
      {
        // If we're in single, go to multi
        // otherwise resize our multi for the appropriate number of references
        if (single)
        {
          if ((refs.single != NULL) && refs.single->remove_reference())
            delete (refs.single);
          refs.multi = new InternalSet(num_refs);
          refs.multi->add_reference();
          single = false;
        }
        else
          refs.multi->vector.resize(num_refs);
        // Now do the unpacking
        for (unsigned idx = 0; idx < num_refs; idx++)
        {
          RtEvent ready;
          refs.multi->vector[idx].unpack_reference(runtime, derez, ready);
          if (ready.exists())
            ready_events.insert(ready);
        }
      }
      // We are always not shared when we are done
      shared = false;
    }

    //--------------------------------------------------------------------------
    void InstanceSet::add_valid_references(ReferenceSource source) const
    //--------------------------------------------------------------------------
    {
      if (single)
      {
        if (refs.single != NULL)
          refs.single->add_valid_reference(source);
      }
      else
      {
        for (unsigned idx = 0; idx < refs.multi->vector.size(); idx++)
          refs.multi->vector[idx].add_valid_reference(source);
      }
    }

    //--------------------------------------------------------------------------
    void InstanceSet::remove_valid_references(ReferenceSource source) const
    //--------------------------------------------------------------------------
    {
      if (single)
      {
        if (refs.single != NULL)
          refs.single->remove_valid_reference(source);
      }
      else
      {
        for (unsigned idx = 0; idx < refs.multi->vector.size(); idx++)
          refs.multi->vector[idx].remove_valid_reference(source);
      }
    }

    //--------------------------------------------------------------------------
    void InstanceSet::update_wait_on_events(std::set<ApEvent> &wait_on) const 
    //--------------------------------------------------------------------------
    {
      if (single)
      {
        if (refs.single != NULL)
        {
          ApEvent ready = refs.single->get_ready_event();
          if (ready.exists())
            wait_on.insert(ready);
        }
      }
      else
      {
        for (unsigned idx = 0; idx < refs.multi->vector.size(); idx++)
        {
          ApEvent ready = refs.multi->vector[idx].get_ready_event();
          if (ready.exists())
            wait_on.insert(ready);
        }
      }
    }

    //--------------------------------------------------------------------------
    void InstanceSet::find_read_only_reservations(
                                             std::set<Reservation> &locks) const
    //--------------------------------------------------------------------------
    {
      if (single)
      {
        if (refs.single != NULL)
          locks.insert(refs.single->get_read_only_reservation());
      }
      else
      {
        for (unsigned idx = 0; idx < refs.multi->vector.size(); idx++)
          locks.insert(refs.multi->vector[idx].get_read_only_reservation());
      }
    }
    
    //--------------------------------------------------------------------------
    LegionRuntime::Accessor::RegionAccessor<
      LegionRuntime::Accessor::AccessorType::Generic> InstanceSet::
                                           get_field_accessor(FieldID fid) const
    //--------------------------------------------------------------------------
    {
      if (single)
      {
#ifdef DEBUG_LEGION
        assert(refs.single != NULL);
#endif
        return refs.single->get_field_accessor(fid);
      }
      else
      {
        for (unsigned idx = 0; idx < refs.multi->vector.size(); idx++)
        {
          const InstanceRef &ref = refs.multi->vector[idx];
          if (ref.is_field_set(fid))
            return ref.get_field_accessor(fid);
        }
        assert(false);
        return refs.multi->vector[0].get_field_accessor(fid);
      }
    }

    /////////////////////////////////////////////////////////////
    // VersioningInvalidator 
    /////////////////////////////////////////////////////////////

    //--------------------------------------------------------------------------
    VersioningInvalidator::VersioningInvalidator(void)
      : ctx(0), invalidate_all(true)
    //--------------------------------------------------------------------------
    {
    }

    //--------------------------------------------------------------------------
    VersioningInvalidator::VersioningInvalidator(RegionTreeContext c)
      : ctx(c.get_id()), invalidate_all(!c.exists())
    //--------------------------------------------------------------------------
    {
    }

    //--------------------------------------------------------------------------
    bool VersioningInvalidator::visit_region(RegionNode *node)
    //--------------------------------------------------------------------------
    {
      if (invalidate_all)
        node->invalidate_version_managers();
      else
        node->invalidate_version_state(ctx);
      return true;
    }

    //--------------------------------------------------------------------------
    bool VersioningInvalidator::visit_partition(PartitionNode *node)
    //--------------------------------------------------------------------------
    {
      if (invalidate_all)
        node->invalidate_version_managers();
      else
        node->invalidate_version_state(ctx);
      return true;
    }

  }; // namespace Internal 
}; // namespace Legion

