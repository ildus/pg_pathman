/* ------------------------------------------------------------------------
 *
 * nodes_common.c
 *		Common code for custom nodes
 *
 * Copyright (c) 2016, Postgres Professional
 *
 * ------------------------------------------------------------------------
 */
#include "postgres.h"
#include "optimizer/paths.h"
#include "nodes_common.h"
#include "runtimeappend.h"



/* Compare plans by 'original_order' */
static int
cmp_child_scan_common_by_orig_order(const void *ap,
									const void *bp)
{
	ChildScanCommon a = *(ChildScanCommon *) ap;
	ChildScanCommon b = *(ChildScanCommon *) bp;

	if (a->original_order > b->original_order)
		return 1;
	else if (a->original_order < b->original_order)
		return -1;
	else
		return 0;
}

static void
transform_plans_into_states(RuntimeAppendState *scan_state,
							ChildScanCommon *selected_plans, int n,
							EState *estate)
{
	int i;

	for (i = 0; i < n; i++)
	{
		ChildScanCommon		child = selected_plans[i];
		PlanState		   *ps;

		/* Create new node since this plan hasn't been used yet */
		if (child->content_type != CHILD_PLAN_STATE)
		{
			Assert(child->content_type == CHILD_PLAN); /* no paths allowed */

			ps = ExecInitNode(child->content.plan, estate, 0);
			child->content.plan_state = ps;
			child->content_type = CHILD_PLAN_STATE; /* update content type */

			/* Explain and clear_plan_states rely on this list */
			scan_state->css.custom_ps = lappend(scan_state->css.custom_ps, ps);
		}
		else
			ps = child->content.plan_state;

		/* Node with params will be ReScanned */
		if (scan_state->css.ss.ps.chgParam)
			UpdateChangedParamSet(ps, scan_state->css.ss.ps.chgParam);

		/*
		 * We should ReScan this node manually since
		 * ExecProcNode won't do this for us in this case.
		 */
		if (bms_is_empty(ps->chgParam))
			ExecReScan(ps);

		child->content.plan_state = ps;
	}
}

static ChildScanCommon *
select_required_plans(HTAB *children_table, Oid *parts, int nparts, int *nres)
{
	int					allocated = 10;
	int					used = 0;
	ChildScanCommon	   *result = palloc(10 * sizeof(ChildScanCommon));
	int					i;

	for (i = 0; i < nparts; i++)
	{
		ChildScanCommon child = hash_search(children_table,
											(const void *) &parts[i],
											HASH_FIND, NULL);
		if (!child)
			continue; /* no plan for this partition */

		if (allocated <= used)
		{
			allocated *= 2;
			result = repalloc(result, allocated * sizeof(ChildScanCommon));
		}

		result[used++] = child;
	}

	*nres = used;
	return result;
}

/* Transform partition ranges into plain array of partition Oids */
static Oid *
get_partition_oids(List *ranges, int *n, PartRelationInfo *prel)
{
	ListCell   *range_cell;
	int			allocated = 10;
	int			used = 0;
	Oid		   *result = palloc(allocated * sizeof(Oid));
	Oid		   *children = dsm_array_get_pointer(&prel->children);

	foreach (range_cell, ranges)
	{
		int i;
		int a = irange_lower(lfirst_irange(range_cell));
		int b = irange_upper(lfirst_irange(range_cell));

		for (i = a; i <= b; i++)
		{
			if (allocated <= used)
			{
				allocated *= 2;
				result = repalloc(result, allocated * sizeof(Oid));
			}

			Assert(i < prel->children_count);
			result[used++] = children[i];
		}
	}

	*n = used;
	return result;
}

static void
pack_runtimeappend_private(CustomScan *cscan, RuntimeAppendPath *path)
{
	ChildScanCommon    *children = path->children;
	int					nchildren = path->nchildren;
	List			   *custom_private = NIL;
	List			   *custom_oids = NIL;
	int					i;

	for (i = 0; i < nchildren; i++)
	{
		/* We've already filled 'custom_paths' in create_runtimeappend_path */
		custom_oids = lappend_oid(custom_oids, children[i]->relid);
		pfree(children[i]);
	}

	/* Save main table and partition relids as first element of 'custom_private' */
	custom_private = lappend(custom_private,
							 list_make2(list_make1_oid(path->relid),
										custom_oids));

	cscan->custom_private = custom_private;
}

static void
unpack_runtimeappend_private(RuntimeAppendState *scan_state, CustomScan *cscan)
{
	ListCell   *oid_cell;
	ListCell   *plan_cell;
	List	   *runtimeappend_private = linitial(cscan->custom_private);
	List	   *custom_oids = (List *) lsecond(runtimeappend_private);
	int			nchildren = list_length(custom_oids);
	HTAB	   *children_table;
	HASHCTL	   *children_table_config = &scan_state->children_table_config;
	int			i;

	memset(children_table_config, 0, sizeof(HASHCTL));
	children_table_config->keysize = sizeof(Oid);
	children_table_config->entrysize = sizeof(ChildScanCommonData);

	children_table = hash_create("Plan storage", nchildren,
								 children_table_config,
							     HASH_ELEM | HASH_BLOBS);

	i = 0;
	forboth (oid_cell, custom_oids, plan_cell, cscan->custom_plans)
	{
		bool				child_found;
		Oid					cur_oid = lfirst_oid(oid_cell);

		ChildScanCommon		child = hash_search(children_table,
												(const void *) &cur_oid,
												HASH_ENTER, &child_found);

		Assert(!child_found); /* there should be no collisions */

		child->content_type = CHILD_PLAN;
		child->content.plan = (Plan *) lfirst(plan_cell);
		child->original_order = i++; /* will be used in EXPLAIN */
	}

	scan_state->children_table = children_table;
	scan_state->relid = linitial_oid(linitial(runtimeappend_private));
}

Path *
create_append_path_common(PlannerInfo *root,
						  AppendPath *inner_append,
						  ParamPathInfo *param_info,
						  List *runtime_clauses,
						  CustomPathMethods *path_methods,
						  uint32 size,
						  double sel)
{
	RelOptInfo		   *innerrel = inner_append->path.parent;
	ListCell		   *lc;
	int					i;

	RangeTblEntry	   *inner_entry = root->simple_rte_array[innerrel->relid];

	RuntimeAppendPath  *result;

	result = palloc0(size);
	NodeSetTag(result, T_CustomPath);

	result->cpath.path.pathtype = T_CustomScan;
	result->cpath.path.parent = innerrel;
	result->cpath.path.param_info = param_info;
	result->cpath.path.pathkeys = inner_append->path.pathkeys;
#if PG_VERSION_NUM >= 90600
	result->cpath.path.pathtarget = inner_append->path.pathtarget;
#endif
	result->cpath.path.rows = inner_append->path.rows * sel;
	result->cpath.flags = 0;
	result->cpath.methods = path_methods;

	/* TODO: real costs */
	result->cpath.path.startup_cost = 0.0;
	result->cpath.path.total_cost = 0.0;

	/* Set 'partitioned column'-related clauses */
	result->cpath.custom_private = runtime_clauses;
	result->cpath.custom_paths = NIL;

	Assert(inner_entry->relid != 0);
	result->relid = inner_entry->relid;

	result->nchildren = list_length(inner_append->subpaths);
	result->children = palloc(result->nchildren * sizeof(ChildScanCommon));
	i = 0;
	foreach (lc, inner_append->subpaths)
	{
		Path			   *path = lfirst(lc);
		Index				relindex = path->parent->relid;
		ChildScanCommon		child = palloc(sizeof(ChildScanCommonData));

		result->cpath.path.startup_cost += path->startup_cost;
		result->cpath.path.total_cost += path->total_cost;

		child->content_type = CHILD_PATH;
		child->content.path = path;
		child->relid = root->simple_rte_array[relindex]->relid;
		Assert(child->relid != InvalidOid);

		result->cpath.custom_paths = lappend(result->cpath.custom_paths,
											 child->content.path);
		result->children[i] = child;

		i++;
	}

	result->cpath.path.startup_cost *= sel;
	result->cpath.path.total_cost *= sel;

	return &result->cpath.path;
}

Plan *
create_append_plan_common(PlannerInfo *root, RelOptInfo *rel,
						  CustomPath *best_path, List *tlist,
						  List *clauses, List *custom_plans,
						  CustomScanMethods *scan_methods)
{
	RuntimeAppendPath  *gpath = (RuntimeAppendPath *) best_path;
	CustomScan		   *cscan;

	cscan = makeNode(CustomScan);
	cscan->scan.plan.qual = NIL;
	cscan->scan.plan.targetlist = tlist;
	cscan->custom_scan_tlist = tlist;
	cscan->scan.scanrelid = 0;

	cscan->custom_exprs = gpath->cpath.custom_private;
	cscan->custom_plans = custom_plans;

	cscan->methods = scan_methods;

	pack_runtimeappend_private(cscan, gpath);

	return &cscan->scan.plan;
}

Node *
create_append_scan_state_common(CustomScan *node,
								CustomExecMethods *exec_methods,
								uint32 size)
{
	RuntimeAppendState *scan_state = palloc0(size);

	NodeSetTag(scan_state, T_CustomScanState);
	scan_state->css.flags = node->flags;
	scan_state->css.methods = exec_methods;
	scan_state->custom_exprs = node->custom_exprs;

	unpack_runtimeappend_private(scan_state, node);

	/* Fill in relation info using main table's relid */
	scan_state->prel = get_pathman_relation_info(scan_state->relid, NULL);
	Assert(scan_state->prel);

	scan_state->cur_plans = NULL;
	scan_state->ncur_plans = 0;
	scan_state->running_idx = 0;

	return (Node *) scan_state;
}

void
begin_append_common(CustomScanState *node, EState *estate, int eflags)
{
	RuntimeAppendState *scan_state = (RuntimeAppendState *) node;

	scan_state->custom_expr_states =
		(List *) ExecInitExpr((Expr *) scan_state->custom_exprs,
							  (PlanState *) scan_state);
}

void
end_append_common(CustomScanState *node)
{
	RuntimeAppendState *scan_state = (RuntimeAppendState *) node;

	clear_plan_states(&scan_state->css);
	hash_destroy(scan_state->children_table);
}

void
rescan_append_common(CustomScanState *node)
{
	RuntimeAppendState *scan_state = (RuntimeAppendState *) node;
	ExprContext		   *econtext = node->ss.ps.ps_ExprContext;
	PartRelationInfo   *prel = scan_state->prel;
	List			   *ranges;
	ListCell		   *lc;
	Oid				   *parts;
	int					nparts;
	WalkerContext		wcxt;

	ranges = list_make1_int(make_irange(0, prel->children_count - 1, false));

	wcxt.prel = prel;
	wcxt.econtext = econtext;
	wcxt.hasLeast = false;
	wcxt.hasGreatest = false;

	foreach (lc, scan_state->custom_exprs)
	{
		WrapperNode	   *wn;

		wn = walk_expr_tree((Expr *) lfirst(lc), &wcxt);

		ranges = irange_list_intersect(ranges, wn->rangeset);
	}

	/* Get Oids of the required partitions */
	parts = get_partition_oids(ranges, &nparts, prel);

	/* Select new plans for this run using 'parts' */
	if (scan_state->cur_plans)
		pfree(scan_state->cur_plans); /* shallow free since cur_plans
									   * belong to children_table  */
	scan_state->cur_plans = select_required_plans(scan_state->children_table,
												  parts, nparts,
												  &scan_state->ncur_plans);
	pfree(parts);

	/* Transform selected plans into executable plan states */
	transform_plans_into_states(scan_state,
								scan_state->cur_plans,
								scan_state->ncur_plans,
								scan_state->css.ss.ps.state);

	scan_state->running_idx = 0;
}

void
explain_append_common(CustomScanState *node, HTAB *children_table, ExplainState *es)
{
	/* Construct excess PlanStates */
	if (!es->analyze)
	{
		int					allocated = 10;
		int					used = 0;
		ChildScanCommon	   *custom_ps = palloc(allocated * sizeof(ChildScanCommon));
		ChildScanCommon		child;
		HASH_SEQ_STATUS		seqstat;
		int					i;

		/* There can't be any nodes since we're not scanning anything */
		Assert(!node->custom_ps);

		/* Iterate through node's ChildScanCommon table */
		hash_seq_init(&seqstat, children_table);

		while ((child = (ChildScanCommon) hash_seq_search(&seqstat)))
		{
			if (allocated <= used)
			{
				allocated *= 2;
				custom_ps = repalloc(custom_ps, allocated * sizeof(ChildScanCommon));
			}

			custom_ps[used++] = child;
		}

		/*
		 * We have to restore the original plan order
		 * which has been lost within the hash table
		 */
		qsort(custom_ps, used, sizeof(ChildScanCommon),
			  cmp_child_scan_common_by_orig_order);

		/*
		 * These PlanStates will be used by EXPLAIN,
		 * end_append_common will destroy them eventually
		 */
		for (i = 0; i < used; i++)
			node->custom_ps = lappend(node->custom_ps,
									  ExecInitNode(custom_ps[i]->content.plan,
												   node->ss.ps.state,
												   0));
	}
}