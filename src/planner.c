#include <postgres.h>
#include <nodes/plannodes.h>
#include <parser/parsetree.h>
#include <optimizer/clauses.h>
#include <optimizer/planner.h>
#include <optimizer/pathnode.h>
#include <optimizer/paths.h>
#include <catalog/namespace.h>
#include <utils/guc.h>
#include <miscadmin.h>

#include "compat-msvc-enter.h"
#include <optimizer/cost.h>
#include "compat-msvc-exit.h"

#include "hypertable_cache.h"
#include "extension.h"
#include "utils.h"
#include "guc.h"
#include "dimension.h"
#include "chunk_dispatch_plan.h"
#include "planner_utils.h"
#include "hypertable_insert.h"
#include "constraint_aware_append.h"

void		_planner_init(void);
void		_planner_fini(void);

static planner_hook_type prev_planner_hook;
static set_rel_pathlist_hook_type prev_set_rel_pathlist_hook;

typedef struct ModifyTableWalkerCtx
{
	Query	   *parse;
	Cache	   *hcache;
	List	   *rtable;
} ModifyTableWalkerCtx;

/*
 * Traverse the plan tree to find ModifyTable nodes that indicate an INSERT
 * operation. We'd like to modify these plans to redirect tuples to chunks
 * instead of the parent table.
 *
 * From the ModifyTable description: "Each ModifyTable node contains
 * a list of one or more subplans, much like an Append node.  There
 * is one subplan per result relation."
 *
 * The subplans produce the tuples for INSERT, while the result relation is the
 * table we'd like to insert into.
 *
 * The way we redirect tuples to chunks is to insert an intermediate "chunk
 * dispatch" plan node, inbetween the ModifyTable and its subplan that produces
 * the tuples. When the ModifyTable plan is executed, it tries to read a tuple
 * from the intermediate chunk dispatch plan instead of the original
 * subplan. The chunk plan reads the tuple from the original subplan, looks up
 * the chunk, sets the executor's resultRelation to the chunk table and finally
 * returns the tuple to the ModifyTable node.
 *
 * We also need to wrap the ModifyTable plan node with a HypertableInsert node
 * to give the ChunkDispatchState node access to the ModifyTableState node in
 * the execution phase.
 *
 * Conceptually, the plan modification looks like this:
 *
 * Original plan:
 *
 *		  ^
 *		  |
 *	[ ModifyTable ] -> resultRelation
 *		  ^
 *		  | Tuple
 *		  |
 *	  [ subplan ]
 *
 *
 * Modified plan:
 *
 *	[ HypertableInsert ]
 *		  ^
 *		  |
 *	[ ModifyTable ] -> resultRelation
 *		  ^			   ^
 *		  | Tuple	  / <Set resultRelation to the matching chunk table>
 *		  |			 /
 * [ ChunkDispatch ]
 *		  ^
 *		  | Tuple
 *		  |
 *	  [ subplan ]
 */
static void
modifytable_plan_walker(Plan **planptr, void *pctx)
{
	ModifyTableWalkerCtx *ctx = (ModifyTableWalkerCtx *) pctx;
	Plan	   *plan = *planptr;

	if (IsA(plan, ModifyTable))
	{
		ModifyTable *mt = (ModifyTable *) plan;

		if (mt->operation == CMD_INSERT)
		{
			bool		hypertable_found = false;
			ListCell   *lc_plan,
					   *lc_rel;

			/*
			 * To match up tuple-producing subplans with result relations, we
			 * simultaneously loop over subplans and resultRelations, although
			 * for INSERTs we expect only one of each.
			 */
			forboth(lc_plan, mt->plans, lc_rel, mt->resultRelations)
			{
				Index		rti = lfirst_int(lc_rel);
				RangeTblEntry *rte = rt_fetch(rti, ctx->rtable);
				Hypertable *ht = hypertable_cache_get_entry(ctx->hcache, rte->relid);

				if (ht != NULL)
				{
					void	  **subplan_ptr = &lfirst(lc_plan);
					Plan	   *subplan = *subplan_ptr;

					if (ctx->parse->onConflict != NULL &&
						ctx->parse->onConflict->constraint != InvalidOid)
						ereport(ERROR,
								(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
								 errmsg("Hypertables do not support ON CONFLICT statements that reference constraints"),
								 errhint("Use column names to infer indexes instead.")));

					/*
					 * We replace the plan with our custom chunk dispatch
					 * plan.
					 */
					*subplan_ptr = chunk_dispatch_plan_create(subplan, rti, rte->relid, ctx->parse);
					hypertable_found = true;
				}
			}

			if (hypertable_found)
				*planptr = hypertable_insert_plan_create(mt);
		}
	}
}


static PlannedStmt *
timescaledb_planner(Query *parse, int cursor_opts, ParamListInfo bound_params)
{
	PlannedStmt *plan_stmt = NULL;

	if (prev_planner_hook != NULL)
	{
		/* Call any earlier hooks */
		plan_stmt = (prev_planner_hook) (parse, cursor_opts, bound_params);
	}
	else
	{
		/* Call the standard planner */
		plan_stmt = standard_planner(parse, cursor_opts, bound_params);
	}

	if (extension_is_loaded())
	{
		ModifyTableWalkerCtx ctx = {
			.parse = parse,
			.hcache = hypertable_cache_pin(),
			.rtable = plan_stmt->rtable,
		};

		planned_stmt_walker(plan_stmt, modifytable_plan_walker, &ctx);
		cache_release(ctx.hcache);
	}

	return plan_stmt;
}


static inline bool
should_optimize_query(Hypertable *ht)
{
	return !guc_disable_optimizations &&
		(guc_optimize_non_hypertables || ht != NULL);
}


extern void sort_transform_optimization(PlannerInfo *root, RelOptInfo *rel);

static inline bool
should_optimize_append(const Path *path)
{
	RelOptInfo *rel = path->parent;
	ListCell   *lc;

	if (!guc_constraint_aware_append ||
		constraint_exclusion == CONSTRAINT_EXCLUSION_OFF)
		return false;

	/*
	 * If there are clauses that have mutable functions, this path is ripe for
	 * execution-time optimization
	 */
	foreach(lc, rel->baserestrictinfo)
	{
		RestrictInfo *rinfo = (RestrictInfo *) lfirst(lc);

		if (contain_mutable_functions((Node *) rinfo->clause))
			return true;
	}
	return false;
}


static inline bool
is_append_child(RelOptInfo *rel, RangeTblEntry *rte)
{
	return rel->reloptkind == RELOPT_OTHER_MEMBER_REL &&
		rte->inh == false &&
		rel->rtekind == RTE_RELATION &&
		rte->relkind == RELKIND_RELATION;
}

static inline bool
is_append_parent(RelOptInfo *rel, RangeTblEntry *rte)
{
	return rel->reloptkind == RELOPT_BASEREL &&
		rte->inh == true &&
		rel->rtekind == RTE_RELATION &&
		rte->relkind == RELKIND_RELATION;
}


static void
timescaledb_set_rel_pathlist(PlannerInfo *root,
							 RelOptInfo *rel,
							 Index rti,
							 RangeTblEntry *rte)
{
	Hypertable *ht;
	Cache	   *hcache;

	if (prev_set_rel_pathlist_hook != NULL)
		(*prev_set_rel_pathlist_hook) (root, rel, rti, rte);

	if (!extension_is_loaded() || IS_DUMMY_REL(rel) || !OidIsValid(rte->relid))
		return;

	/* quick abort if only optimizing hypertables */
	if (!guc_optimize_non_hypertables && !(is_append_parent(rel, rte) || is_append_child(rel, rte)))
		return;

	hcache = hypertable_cache_pin();
	ht = hypertable_cache_get_entry(hcache, rte->relid);

	if (!should_optimize_query(ht))
		goto out_release;

	if (guc_optimize_non_hypertables)
	{
		/* if optimizing all tables, apply optimization to any table */
		sort_transform_optimization(root, rel);
	}
	else if (ht != NULL && is_append_child(rel, rte))
	{
		/* Otherwise, apply only to hypertables */

		/*
		 * When applying to hypertables, apply when you get the first append
		 * relation child (indicated by RELOPT_OTHER_MEMBER_REL) which is the
		 * main table. Then apply to all other children of that hypertable. We
		 * can't wait to get the parent of the append relation b/c by that
		 * time it's too late.
		 */
		ListCell   *l;

		foreach(l, root->append_rel_list)
		{
			AppendRelInfo *appinfo = (AppendRelInfo *) lfirst(l);
			RelOptInfo *siblingrel;

			/*
			 * Note: check against the reloid not the index in the
			 * simple_rel_array since the current rel is not the parent but
			 * just the child of the append_rel representing the main table.
			 */
			if (appinfo->parent_reloid != rte->relid)
				continue;
			siblingrel = root->simple_rel_array[appinfo->child_relid];
			sort_transform_optimization(root, siblingrel);
		}
	}

	if (

	/*
	 * Right now this optimization applies only to hypertables (ht used
	 * below). Can be relaxed later to apply to reg tables but needs testing
	 */
		ht != NULL &&
		is_append_parent(rel, rte) &&
	/* Do not optimize result relations (INSERT, UPDATE, DELETE) */
		0 == root->parse->resultRelation)
	{
		ListCell   *lc;

		foreach(lc, rel->pathlist)
		{
			Path	  **pathptr = (Path **) &lfirst(lc);
			Path	   *path = *pathptr;

			switch (nodeTag(path))
			{
				case T_AppendPath:
				case T_MergeAppendPath:
					if (should_optimize_append(path))
						*pathptr = constraint_aware_append_path_create(root, ht, path);
				default:
					break;
			}
		}
	}

out_release:
	cache_release(hcache);
}

void
_planner_init(void)
{
	prev_planner_hook = planner_hook;
	planner_hook = timescaledb_planner;
	prev_set_rel_pathlist_hook = set_rel_pathlist_hook;
	set_rel_pathlist_hook = timescaledb_set_rel_pathlist;
}

void
_planner_fini(void)
{
	planner_hook = prev_planner_hook;
	set_rel_pathlist_hook = prev_set_rel_pathlist_hook;
}
