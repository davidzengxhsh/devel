/*
 * gpupreagg.c
 *
 * Aggregate Pre-processing with GPU acceleration
 * ----
 * Copyright 2011-2014 (C) KaiGai Kohei <kaigai@kaigai.gr.jp>
 * Copyright 2014 (C) The PG-Strom Development Team
 *
 * This software is an extension of PostgreSQL; You can use, copy,
 * modify or distribute it under the terms of 'LICENSE' included
 * within this package.
 */
#include "postgres.h"
#include "access/nbtree.h"
#include "access/sysattr.h"
#include "catalog/namespace.h"
#include "catalog/pg_aggregate.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "parser/parse_func.h"
#include "optimizer/clauses.h"
#include "optimizer/cost.h"
#include "optimizer/var.h"
#include "parser/parsetree.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"
#include <math.h>
#include "pg_strom.h"
#include "opencl_gpupreagg.h"

static CustomPlanMethods		gpupreagg_plan_methods;
static bool						enable_gpupreagg;

typedef struct
{
	CustomPlan		cplan;
	int				numCols;		/* number of grouping columns */
	AttrNumber	   *grpColIdx;		/* their indexes in the target list */
	const char	   *kern_source;
	int				extra_flags;
	List		   *used_params;	/* referenced Const/Param */
	Bitmapset	   *outer_attrefs;	/* bitmap of referenced outer attributes */
	Bitmapset	   *tlist_attrefs;	/* bitmap of referenced tlist attributes */
} GpuPreAggPlan;

typedef struct
{
	CustomPlanState	cps;
	TupleDesc		scan_desc;
	TupleTableSlot *scan_slot;
	bool			outer_done;

	pgstrom_queue  *mqueue;
	Datum			dprog_key;
	kern_parambuf  *kparams;

	pgstrom_gpupreagg  *curr_chunk;
	cl_uint				curr_index;
	cl_uint				num_running;
	dlist_head			ready_chunks;

	pgstrom_perfmon		pfm;		/* performance counter */
} GpuPreAggState;

/* declaration of static functions */
//static void clserv_process_gpupreagg(pgstrom_message *message);

/*
 * Arguments of alternative functions.
 */
#define ALTFUNC_EXPR_NROWS			101	/* NROWS(X) */
#define ALTFUNC_EXPR_PMIN			102	/* PMIN(X) */
#define ALTFUNC_EXPR_PMAX			103	/* PMAX(X) */
#define ALTFUNC_EXPR_PSUM			104	/* PSUM(X) */
#define ALTFUNC_EXPR_PSUM_X2		105	/* PSUM_X2(X) = PSUM(X^2) */
#define ALTFUNC_EXPR_PCOV_X			106	/* PCOV_X(X,Y) */
#define ALTFUNC_EXPR_PCOV_Y			107	/* PCOV_Y(X,Y) */
#define ALTFUNC_EXPR_PCOV_X2		108	/* PCOV_X2(X,Y) */
#define ALTFUNC_EXPR_PCOV_Y2		109	/* PCOV_Y2(X,Y) */
#define ALTFUNC_EXPR_PCOV_XY		110	/* PCOV_XY(X,Y) */

/*
 * List of supported aggregate functions
 */
typedef struct {
	/* aggregate function can be preprocessed */
	const char *aggfn_name;
	int			aggfn_nargs;
	Oid			aggfn_argtypes[4];
	/* alternative function to generate same result.
	 * prefix indicates the schema that stores the alternative functions
	 * c: pg_catalog ... the system default
	 * s: pgstrom    ... PG-Strom's special ones
	 */
	const char *altfn_name;
	int			altfn_nargs;
	Oid			altfn_argtypes[8];
	int			altfn_argexprs[8];
} aggfunc_catalog_t;
static aggfunc_catalog_t  aggfunc_catalog[] = {
	/* AVG(X) = EX_AVG(NROWS(), PSUM(X)) */
	{ "avg",    1, {INT2OID},
	  "s:avg",  2, {INT4OID, INT8OID},
	  {ALTFUNC_EXPR_NROWS, ALTFUNC_EXPR_PSUM}
	},
	{ "avg",    1, {INT4OID},
	  "s:avg",  2, {INT4OID, INT8OID},
	  {ALTFUNC_EXPR_NROWS, ALTFUNC_EXPR_PSUM}
	},
	{ "avg",    1, {INT8OID},
	  "s:avg",  2, {INT4OID, INT8OID},
	  {ALTFUNC_EXPR_NROWS, ALTFUNC_EXPR_PSUM}
	},
	{ "avg",    1, {FLOAT4OID},
	  "s:avg",  2, {INT4OID, FLOAT8OID},
	  {ALTFUNC_EXPR_NROWS, ALTFUNC_EXPR_PSUM}
	},
	{ "avg",    1, {FLOAT8OID},
	  "s:avg",  2, {INT4OID, FLOAT8OID},
	  {ALTFUNC_EXPR_NROWS, ALTFUNC_EXPR_PSUM}
	},
	/* COUNT(*) = SUM(NROWS(*|X)) */
	{ "count", 0, {},       "c:sum", 1, {INT4OID}, {ALTFUNC_EXPR_NROWS}},
	{ "count", 1, {ANYOID}, "c:sum", 1, {INT4OID}, {ALTFUNC_EXPR_NROWS}},
	/* MAX(X) = MAX(PMAX(X)) */
	{ "max", 1, {INT2OID},   "c:max", 1, {INT2OID},   {ALTFUNC_EXPR_PMAX}},
	{ "max", 1, {INT4OID},   "c:max", 1, {INT4OID},   {ALTFUNC_EXPR_PMAX}},
	{ "max", 1, {INT8OID},   "c:max", 1, {INT8OID},   {ALTFUNC_EXPR_PMAX}},
	{ "max", 1, {FLOAT4OID}, "c:max", 1, {FLOAT4OID}, {ALTFUNC_EXPR_PMAX}},
	{ "max", 1, {FLOAT8OID}, "c:max", 1, {FLOAT8OID}, {ALTFUNC_EXPR_PMAX}},
	/* MIX(X) = MIN(PMIN(X)) */
	{ "min", 1, {INT2OID},   "c:min", 1, {INT2OID},   {ALTFUNC_EXPR_PMIN}},
	{ "min", 1, {INT4OID},   "c:min", 1, {INT4OID},   {ALTFUNC_EXPR_PMIN}},
	{ "min", 1, {INT8OID},   "c:min", 1, {INT8OID},   {ALTFUNC_EXPR_PMIN}},
	{ "min", 1, {FLOAT4OID}, "c:min", 1, {FLOAT4OID}, {ALTFUNC_EXPR_PMIN}},
	{ "min", 1, {FLOAT8OID}, "c:min", 1, {FLOAT8OID}, {ALTFUNC_EXPR_PMIN}},
	/* SUM(X) = SUM(PSUM(X)) */
	{ "sum", 1, {INT2OID},   "s:sum", 1, {INT8OID},   {ALTFUNC_EXPR_PSUM}},
	{ "sum", 1, {INT4OID},   "s:sum", 1, {INT8OID},   {ALTFUNC_EXPR_PSUM}},
	{ "sum", 1, {FLOAT4OID}, "c:sum", 1, {FLOAT8OID}, {ALTFUNC_EXPR_PSUM}},
	{ "sum", 1, {FLOAT8OID}, "c:sum", 1, {FLOAT8OID}, {ALTFUNC_EXPR_PSUM}},
	/* STDDEV(X) = EX_STDDEV(NROWS(),PSUM(X),PSUM(X*X)) */
	{ "stddev", 1, {FLOAT4OID},
	  "s:stddev", 3, {INT4OID, FLOAT8OID, FLOAT8OID},
	  {ALTFUNC_EXPR_NROWS,
	   ALTFUNC_EXPR_PSUM,
	   ALTFUNC_EXPR_PSUM_X2}},
	{ "stddev", 1, {FLOAT8OID},
	  "s:stddev", 3, {INT4OID, FLOAT8OID, FLOAT8OID},
	  {ALTFUNC_EXPR_NROWS,
	   ALTFUNC_EXPR_PSUM,
	   ALTFUNC_EXPR_PSUM_X2}},
	{ "stddev_pop", 1, {FLOAT4OID},
	  "s:stddev_pop", 3, {INT4OID, FLOAT8OID, FLOAT8OID},
	  {ALTFUNC_EXPR_NROWS,
	   ALTFUNC_EXPR_PSUM,
	   ALTFUNC_EXPR_PSUM_X2}},
	{ "stddev_pop", 1, {FLOAT8OID},
	  "s:stddev_pop", 3, {INT4OID, FLOAT8OID, FLOAT8OID},
	  {ALTFUNC_EXPR_NROWS,
	   ALTFUNC_EXPR_PSUM,
	   ALTFUNC_EXPR_PSUM_X2}},
	{ "stddev_samp", 1, {FLOAT4OID},
	  "s:stddev_samp", 3, {INT4OID, FLOAT8OID, FLOAT8OID},
	  {ALTFUNC_EXPR_NROWS,
	   ALTFUNC_EXPR_PSUM,
	   ALTFUNC_EXPR_PSUM_X2}},
	{ "stddev_samp", 1, {FLOAT8OID},
	  "s:stddev_samp", 3, {INT4OID, FLOAT8OID, FLOAT8OID},
	  {ALTFUNC_EXPR_NROWS,
	   ALTFUNC_EXPR_PSUM,
	   ALTFUNC_EXPR_PSUM_X2}},
	/* VARIANCE(X) = PGSTROM.VARIANCE(NROWS(), PSUM(X),PSUM(X^2)) */
	{ "variance", 1, {FLOAT4OID},
	  "s:variance", 3, {INT4OID, FLOAT8OID, FLOAT8OID},
	  {ALTFUNC_EXPR_NROWS,
	   ALTFUNC_EXPR_PSUM,
	   ALTFUNC_EXPR_PSUM_X2}},
	{ "variance", 1, {FLOAT8OID},
	  "s:variance", 3, {INT4OID, FLOAT8OID, FLOAT8OID},
	  {ALTFUNC_EXPR_NROWS,
	   ALTFUNC_EXPR_PSUM,
	   ALTFUNC_EXPR_PSUM_X2}},
	{ "var_pop", 1, {FLOAT4OID},
	  "s:var_pop", 3, {INT4OID, FLOAT8OID, FLOAT8OID},
	  {ALTFUNC_EXPR_NROWS,
	   ALTFUNC_EXPR_PSUM,
	   ALTFUNC_EXPR_PSUM_X2}},
	{ "var_samp", 1, {FLOAT8OID},
	  "s:var_samp", 3, {INT4OID, FLOAT8OID, FLOAT8OID},
	  {ALTFUNC_EXPR_NROWS,
	   ALTFUNC_EXPR_PSUM,
	   ALTFUNC_EXPR_PSUM_X2}},
	/* CORR(X,Y) = PGSTROM.CORR(NROWS(X,Y),
	 *                          PCOV_X(X,Y),  PCOV_Y(X,Y)
	 *                          PCOV_X2(X,Y), PCOV_Y2(X,Y),
	 *                          PCOV_XY(X,Y))
	 */
	{ "corr", 2, {FLOAT8OID, FLOAT8OID},
	  "s:corr", 6,
	  {INT4OID, FLOAT8OID, FLOAT8OID, FLOAT8OID, FLOAT8OID, FLOAT8OID},
	  {ALTFUNC_EXPR_NROWS,
	   ALTFUNC_EXPR_PCOV_X,
	   ALTFUNC_EXPR_PCOV_Y,
	   ALTFUNC_EXPR_PCOV_X2,
	   ALTFUNC_EXPR_PCOV_Y2,
	   ALTFUNC_EXPR_PCOV_XY}},
	{ "covar_pop", 2, {FLOAT8OID, FLOAT8OID},
	  "s:covar_pop", 6,
	  {INT4OID, FLOAT8OID, FLOAT8OID, FLOAT8OID, FLOAT8OID, FLOAT8OID},
	  {ALTFUNC_EXPR_NROWS,
	   ALTFUNC_EXPR_PCOV_X,
	   ALTFUNC_EXPR_PCOV_Y,
	   ALTFUNC_EXPR_PCOV_X2,
	   ALTFUNC_EXPR_PCOV_Y2,
	   ALTFUNC_EXPR_PCOV_XY}},
	{ "covar_samp", 2, {FLOAT8OID, FLOAT8OID},
	  "s:covar_samp", 6,
	  {INT4OID, FLOAT8OID, FLOAT8OID, FLOAT8OID, FLOAT8OID, FLOAT8OID},
	  {ALTFUNC_EXPR_NROWS,
	   ALTFUNC_EXPR_PCOV_X,
	   ALTFUNC_EXPR_PCOV_Y,
	   ALTFUNC_EXPR_PCOV_X2,
	   ALTFUNC_EXPR_PCOV_Y2,
	   ALTFUNC_EXPR_PCOV_XY}},
};

static const aggfunc_catalog_t *
aggfunc_lookup_by_oid(Oid aggfnoid)
{
	Form_pg_proc	proform;
	HeapTuple		htup;
	int				i;

	htup = SearchSysCache1(PROCOID, ObjectIdGetDatum(aggfnoid));
	if (!HeapTupleIsValid(htup))
		elog(ERROR, "cache lookup failed for function %u", aggfnoid);
	proform = (Form_pg_proc) GETSTRUCT(htup);

	for (i=0; i < lengthof(aggfunc_catalog); i++)
	{
		aggfunc_catalog_t  *catalog = &aggfunc_catalog[i];

		if (strcmp(catalog->aggfn_name, NameStr(proform->proname)) == 0 &&
			catalog->aggfn_nargs == proform->pronargs &&
			memcmp(catalog->aggfn_argtypes,
				   proform->proargtypes.values,
				   sizeof(Oid) * catalog->aggfn_nargs) == 0)
		{
			ReleaseSysCache(htup);
			return catalog;
		}
	}
	ReleaseSysCache(htup);
	return NULL;
}





/*
 * cost_gpupreagg
 *
 * cost estimation of Aggregate if GpuPreAgg is injected
 */
#define LOG2(x)		(log(x) / 0.693147180559945)

static void
cost_gpupreagg(Agg *agg, GpuPreAggPlan *gpreagg,
			   List *agg_tlist, List *agg_quals,
			   Cost *p_total_cost, Cost *p_startup_cost,
			   Cost *p_total_sort, Cost *p_startup_sort)
{
	Plan	   *sort_plan = NULL;
	Plan	   *outer_plan;
	Cost		startup_cost;
	Cost		run_cost;
	Cost		comparison_cost;
	QualCost	pagg_cost;
	int			pagg_width;
	double		num_rows;
	double		rows_per_chunk;
	double		num_chunks;
	Path		dummy;
	AggClauseCosts agg_costs;
	ListCell   *cell;

	outer_plan = outerPlan(agg);
	if (agg->aggstrategy == AGG_SORTED)
	{
		sort_plan = outer_plan;
		Assert(IsA(sort_plan, Sort));
		outer_plan = outerPlan(sort_plan);
	}

	/*
	 * GpuPreAgg internally takes partial sort and aggregation
	 * on GPU devices. It is a factor of additional calculation,
	 * but reduce number of rows to be processed on the later
	 * stage.
	 * Items to be considered is:
	 * - cost for sorting by GPU
	 * - cost for aggregation by GPU
	 * - number of rows being reduced.
	 */
	startup_cost = outer_plan->startup_cost;
	run_cost = outer_plan->total_cost - startup_cost;
	num_rows = outer_plan->plan_rows;

	/*
	 * fixed cost to kick GPU feature
	 */
	startup_cost += pgstrom_gpu_setup_cost;

	/*
	 * cost estimation of internal sorting by GPU.
	 */
	if ((double)NUM_ROWS_PER_COLSTORE >= num_rows)
	{
		rows_per_chunk = num_rows;
		num_chunks = 1.0;
	}
	else
	{
		rows_per_chunk = (double) NUM_ROWS_PER_COLSTORE;
		num_chunks = num_rows / (double) NUM_ROWS_PER_COLSTORE + 1.0;
	}
	comparison_cost = 2.0 * pgstrom_gpu_operator_cost;
	startup_cost += (comparison_cost *
					 rows_per_chunk *
					 LOG2(rows_per_chunk) *
					 num_chunks);
	run_cost += pgstrom_gpu_operator_cost * num_rows;

	/*
	 * cost estimation of partial aggregate by GPU
	 */
	memset(&pagg_cost, 0, sizeof(QualCost));
	pagg_width = 0;
	foreach (cell, gpreagg->cplan.plan.targetlist)
	{
		TargetEntry	   *tle = lfirst(cell);
		QualCost		cost;

		/* no code uses PlannerInfo here. NULL may be OK */
		cost_qual_eval_node(&cost, (Node *) tle->expr, NULL);
		pagg_cost.startup += cost.startup;
		pagg_cost.per_tuple += cost.per_tuple;

		pagg_width += get_typavgwidth(exprType((Node *) tle->expr),
									  exprTypmod((Node *) tle->expr));
	}
	startup_cost += pagg_cost.startup;
    run_cost += (pagg_cost.per_tuple *
				 pgstrom_gpu_operator_cost /
				 cpu_operator_cost *
				 LOG2(rows_per_chunk) *
				 num_chunks);
	/*
	 * set cost values on GpuPreAgg
	 */
	gpreagg->cplan.plan.startup_cost = startup_cost;
	gpreagg->cplan.plan.total_cost = startup_cost + run_cost;
	gpreagg->cplan.plan.plan_rows =
		(agg->plan.plan_rows / num_rows)	/* reduction ratio */
		* rows_per_chunk					/* rows per chunk */
		* num_chunks;						/* number of chunks */
	gpreagg->cplan.plan.plan_width = pagg_width;

	/*
	 * Update estimated sorting cost, if any.
	 * Calculation logic is cost_sort() as built-in code doing.
	 */
	if (agg->aggstrategy == AGG_SORTED)
	{
		cost_sort(&dummy,
				  NULL,		/* PlannerInfo is not referenced! */
				  NIL, 		/* NIL is acceptable */
				  gpreagg->cplan.plan.total_cost,
				  gpreagg->cplan.plan.plan_rows,
				  gpreagg->cplan.plan.plan_width,
				  0.0,
				  work_mem,
				  -1.0);
		*p_startup_sort = dummy.startup_cost;
		*p_total_sort   = dummy.total_cost;
		startup_cost    = dummy.startup_cost;
		run_cost        = dummy.total_cost - dummy.startup_cost;
	}

	/*
	 * Update estimated aggregate cost.
	 * Calculation logic is cost_agg() as built-in code doing.
	 */
	memset(&agg_costs, 0, sizeof(AggClauseCosts));
	count_agg_clauses(NULL, (Node *) agg_tlist, &agg_costs);
	count_agg_clauses(NULL, (Node *) agg_quals, &agg_costs);
	cost_agg(&dummy,
			 NULL,		/* PlannerInfo is not referenced! */
			 agg->aggstrategy,
			 &agg_costs,
			 agg->numCols,
			 (double) agg->numGroups,
			 startup_cost,
			 startup_cost + run_cost,
			 gpreagg->cplan.plan.plan_rows);
	*p_startup_cost = dummy.startup_cost;
	*p_total_cost = dummy.total_cost;

	elog(INFO, "Agg cost = %.2f..%.2f nrows (%.0f => %.0f)",
		 dummy.startup_cost, dummy.total_cost,
		 num_rows,
		 gpreagg->cplan.plan.plan_rows);
}

/*
 * makeZeroConst - create zero constant
 */
static Const *
makeZeroConst(Oid consttype, int32 consttypmod, Oid constcollid)
{
	int16		typlen;
	bool		typbyval;
	Datum		zero_datum;

	get_typlenbyval(consttype, &typlen, &typbyval);
	switch (consttype)
	{
		case INT4OID:
			zero_datum = Int32GetDatum(0);
			break;
		case INT8OID:
			zero_datum = Int64GetDatum(0);
			break;
		case FLOAT4OID:
			zero_datum = Float4GetDatum(0.0);
			break;
		case FLOAT8OID:
			zero_datum = Float8GetDatum(0.0);
			break;
		default:
			elog(ERROR, "type (%u) is not expected", consttype);
			break;
	}
	return makeConst(consttype,
					 consttypmod,
					 constcollid,
					 (int) typlen,
					 zero_datum,
					 false,
					 typbyval);
}

/*
 * functions to make expression node of alternative aggregate/functions
 *
 * make_expr_conditional() - makes the supplied expression conditional
 *   using CASE WHEN ... THEN ... ELSE ... END clause.
 * make_altfunc_expr() - makes alternative function expression
 * make_altfunc_nrows_expr() - makes expression node of number or rows.
 * make_altfunc_expr_pcov() - makes expression node of covariances.
 */
static Expr *
make_expr_conditional(Expr *expr, Expr *filter, Expr *defresult)
{
	CaseWhen   *case_when;
	CaseExpr   *case_expr;

	Assert(exprType((Node *) filter) == BOOLOID);
	if (defresult)
		defresult = copyObject(defresult);
	else
	{
		defresult = (Expr *) makeNullConst(exprType((Node *) expr),
										   exprTypmod((Node *) expr),
										   exprCollation((Node *) expr));
	}
	Assert(exprType((Node *) expr) == exprType((Node *) defresult));

	/* in case when the 'filter' is matched */
	case_when = makeNode(CaseWhen);
	case_when->expr = copyObject(filter);
	case_when->result = copyObject(expr);
	case_when->location = -1;

	/* case body */
	case_expr = makeNode(CaseExpr);
	case_expr->casetype = exprType((Node *) expr);
	case_expr->arg = NULL;
	case_expr->args = list_make1(case_when);
	case_expr->defresult = defresult;
	case_expr->location = -1;

	return (Expr *) case_expr;
}

static Expr *
make_altfunc_expr(const char *func_name, List *args)
{
	Oid			namespace_oid = get_namespace_oid("pgstrom", false);
	Oid			typebuf[8];
	oidvector  *func_argtypes;
	HeapTuple	tuple;
	Form_pg_proc proc_form;
	Expr	   *expr;
	ListCell   *cell;
	int			i = 0;

	/* set up oidvector */
	foreach (cell, args)
		typebuf[i++] = exprType((Node *) lfirst(cell));
	func_argtypes = buildoidvector(typebuf, i);

	/* find an alternative aggregate function */
	tuple = SearchSysCache3(PROCNAMEARGSNSP,
							PointerGetDatum(func_name),
							PointerGetDatum(func_argtypes),
							ObjectIdGetDatum(namespace_oid));
	if (!HeapTupleIsValid(tuple))
		return NULL;
	proc_form = (Form_pg_proc) GETSTRUCT(tuple);
	expr = (Expr *) makeFuncExpr(HeapTupleGetOid(tuple),
								 proc_form->prorettype,
								 args,
								 InvalidOid,
								 InvalidOid,
								 COERCE_EXPLICIT_CALL);
	ReleaseSysCache(tuple);
	return expr;
}

static Expr *
make_altfunc_nrows_expr(Aggref *aggref)
{
	List	   *nrows_args = NIL;
	ListCell   *cell;

	if (aggref->aggfilter)
		nrows_args = lappend(nrows_args, copyObject(aggref->aggfilter));
	foreach (cell, aggref->args)
	{
		TargetEntry *tle = lfirst(cell);
		NullTest	*ntest = makeNode(NullTest);

		Assert(IsA(tle, TargetEntry));
		ntest->arg = copyObject(tle->expr);
		ntest->nulltesttype = IS_NOT_NULL;
		ntest->argisrow = false;

		nrows_args = lappend(nrows_args, ntest);
	}
	return make_altfunc_expr("nrows", nrows_args);
}

/*
 * make_altfunc_pcov_expr - constructs an expression node for partial
 * covariance aggregates.
 */
static Expr *
make_altfunc_pcov_expr(Aggref *aggref, const char *func_name)
{
	Expr		*filter;
	TargetEntry *tle_1 = linitial(aggref->args);
	TargetEntry *tle_2 = lsecond(aggref->args);

	Assert(IsA(tle_1, TargetEntry) && IsA(tle_2, TargetEntry));
	if (!aggref->aggfilter)
		filter = (Expr *) makeBoolConst(true, false);
	else
		filter = copyObject(aggref->aggfilter);
	return make_altfunc_expr(func_name, list_make3(filter,
												   linitial(aggref->args),
												   lsecond(aggref->args)));
}

/*
 * make_gpupreagg_refnode
 *
 * It tries to construct an alternative Aggref node that references
 * partially aggregated results on the target-list of GpuPreAgg node.
 */
static Aggref *
make_gpupreagg_refnode(Aggref *aggref, List **prep_tlist)
{
	const aggfunc_catalog_t *aggfn_cat;
	const char *altfn_name;
	oidvector  *altfn_argtypes;
	Aggref	   *altnode;
	ListCell   *cell;
	Oid			namespace_oid;
	HeapTuple	tuple;
	Form_pg_proc proc_form;
	int			i;

	/* Only aggregated functions listed on the catalog above is supported. */
	aggfn_cat = aggfunc_lookup_by_oid(aggref->aggfnoid);
	if (!aggfn_cat)
		return NULL;

	/* MEMO: Right now, functions below are not supported, so should not
	 * be on the aggfunc_catalog.
	 * - ordered-set aggregate function
	 * - aggregate function that takes VARIADIC argument
	 * - length of arguments are less than 2.
	 */
	Assert(!aggref->aggdirectargs &&
		   !aggref->aggvariadic &&
		   list_length(aggref->args) <= 2);

	/*
	 * Expression node that is executed in the device kernel has to be
	 * supported by codegen.c
	 */
	foreach (cell, aggref->args)
	{
		TargetEntry *tle = lfirst(cell);
		if (!pgstrom_codegen_available_expression(tle->expr))
			return NULL;
	}
	if (!pgstrom_codegen_available_expression(aggref->aggfilter))
		return NULL;

	/*
	 * pulls the definition of alternative aggregate functions from
	 * the catalog. we expect these are installed in "pgstrom" schema.
	 */
	if (strncmp(aggfn_cat->altfn_name, "c:", 2) == 0)
		namespace_oid = PG_CATALOG_NAMESPACE;
	else if (strncmp(aggfn_cat->altfn_name, "s:", 2) == 0)
	{
		namespace_oid = get_namespace_oid("pgstrom", true);
		if (!OidIsValid(namespace_oid))
		{
			ereport(NOTICE,
					(errcode(ERRCODE_UNDEFINED_SCHEMA),
					 errmsg("schema \"pgstrom\" was not found"),
					 errhint("Try to run: CREATE EXTENSION pg_strom")));
			return NULL;
		}
	}
	else
		elog(ERROR, "Bug? unexpected namespace of alternative aggregate");

	altfn_name = aggfn_cat->altfn_name + 2;
	altfn_argtypes = buildoidvector(aggfn_cat->altfn_argtypes,
									aggfn_cat->altfn_nargs);
	tuple = SearchSysCache3(PROCNAMEARGSNSP,
							PointerGetDatum(altfn_name),
							PointerGetDatum(altfn_argtypes),
							ObjectIdGetDatum(namespace_oid));
	if (!HeapTupleIsValid(tuple))
	{
		ereport(NOTICE,
				(errcode(ERRCODE_UNDEFINED_FUNCTION),
				 errmsg("no alternative aggregate function \"%s\" exists",
						funcname_signature_string(altfn_name,
												  aggfn_cat->altfn_nargs,
												  NIL,
												  aggfn_cat->altfn_argtypes)),
				 errhint("Try to run: CREATE EXTENSION pg_strom")));
		return NULL;
	}
	proc_form = (Form_pg_proc) GETSTRUCT(tuple);

	/* sanity checks */
	if (proc_form->prorettype != aggref->aggtype)
		elog(ERROR, "bug? alternative function has different result type");

	/*
	 * construct an Aggref node that represent alternative aggregate
	 * function with preprocessed arguments.
	 */
	altnode = makeNode(Aggref);
	altnode->aggfnoid      = HeapTupleGetOid(tuple);
	altnode->aggtype       = aggref->aggtype;
	altnode->aggcollid     = aggref->aggcollid;
	altnode->inputcollid   = aggref->inputcollid;
	altnode->aggdirectargs = NIL;	/* see the checks above */
	altnode->args          = NIL;	/* to be set below */
	altnode->aggorder      = aggref->aggorder;
	altnode->aggdistinct   = aggref->aggdistinct;
	altnode->aggfilter     = NULL;	/* moved to GpuPreAgg */
	altnode->aggstar       = false;	/* all the alt-agg takes arguments */
	altnode->aggvariadic   = false;	/* see the checks above */
	altnode->aggkind       = aggref->aggkind;
	altnode->agglevelsup   = aggref->agglevelsup;
	altnode->location      = aggref->location;

	ReleaseSysCache(tuple);

	/*
	 * construct arguments of alternative aggregate function. It references
	 * an entry of prep_tlist, so we put expression node on the tlist on
	 * demand.
	 */
	for (i=0; i < aggfn_cat->altfn_nargs; i++)
	{
		int			code = aggfn_cat->altfn_argexprs[i];
		Oid			rettype_oid = aggfn_cat->altfn_argtypes[i];
		TargetEntry *tle;
		Expr	   *expr;
		Var		   *varref;
		AttrNumber	resno;

		switch (code)
		{
			case ALTFUNC_EXPR_NROWS:
				expr = make_altfunc_nrows_expr(aggref);
				break;
			case ALTFUNC_EXPR_PMIN:
				tle = linitial(aggref->args);
				Assert(IsA(tle, TargetEntry));
				expr = tle->expr;
				if (aggref->aggfilter)
					expr = make_expr_conditional(expr, aggref->aggfilter,
												 NULL);
				expr = make_altfunc_expr("pmin", list_make1(expr));
				break;
			case ALTFUNC_EXPR_PMAX:
				tle = linitial(aggref->args);
				Assert(IsA(tle, TargetEntry));
				expr = tle->expr;
				if (aggref->aggfilter)
					expr = make_expr_conditional(expr, aggref->aggfilter,
												 NULL);
				expr = make_altfunc_expr("pmax", list_make1(expr));
				break;
			case ALTFUNC_EXPR_PSUM:
				tle = linitial(aggref->args);
				Assert(IsA(tle, TargetEntry));
				expr = tle->expr;
				if (aggref->aggfilter)
				{
					Expr   *defresult
						= (Expr *) makeZeroConst(exprType((Node *) expr),
												 exprTypmod((Node *) expr),
												 exprCollation((Node *) expr));
					expr = make_expr_conditional(expr, aggref->aggfilter, defresult);
				}
				expr = make_altfunc_expr("psum", list_make1(expr));
				break;
			case ALTFUNC_EXPR_PSUM_X2:
				tle = linitial(aggref->args);
				Assert(IsA(tle, TargetEntry));
				expr = tle->expr;
				if (aggref->aggfilter)
				{
					Expr   *defresult
						= (Expr *) makeZeroConst(exprType((Node *) expr),
												 exprTypmod((Node *) expr),
												 exprCollation((Node *) expr));
					expr = make_expr_conditional(expr, aggref->aggfilter, defresult);
				}
				expr = make_altfunc_expr("psum_x2", list_make1(expr));
				break;
			case ALTFUNC_EXPR_PCOV_X:
				expr = make_altfunc_pcov_expr(aggref, "pcov_x");
				break;
			case ALTFUNC_EXPR_PCOV_Y:
				expr = make_altfunc_pcov_expr(aggref, "pcov_y");
				break;
			case ALTFUNC_EXPR_PCOV_X2:
				expr = make_altfunc_pcov_expr(aggref, "pcov_x2");
				break;
			case ALTFUNC_EXPR_PCOV_Y2:
				expr = make_altfunc_pcov_expr(aggref, "pcov_y2");
				break;
			case ALTFUNC_EXPR_PCOV_XY:
				expr = make_altfunc_pcov_expr(aggref, "pcov_xy");
				break;
			default:
				elog(ERROR, "Bug? unexpected ALTFUNC_EXPR_* label");
		}
		/* does aggregate function contained unsupported expression? */
		if (!expr)
			return NULL;

		/* check return type of the alternative functions */
		if (rettype_oid != exprType((Node *) expr))
		{
			elog(NOTICE, "Bug? result type is \"%s\", but \"%s\" is expected",
				 format_type_be(exprType((Node *) expr)),
				 format_type_be(rettype_oid));
			return NULL;
		}

		/* add this expression node on the prep_tlist */
		foreach (cell, *prep_tlist)
		{
			tle = lfirst(cell);

			if (equal(tle->expr, expr))
			{
				resno = tle->resno;
				break;
			}
		}
		if (!cell)
		{
			tle = makeTargetEntry(expr,
								  list_length(*prep_tlist) + 1,
								  NULL,
								  false);
			*prep_tlist = lappend(*prep_tlist, tle);
			resno = tle->resno;
		}
		/*
		 * alternative aggregate function shall reference this resource.
		 */
		varref = makeVar(OUTER_VAR,
						 resno,
						 exprType((Node *) expr),
						 exprTypmod((Node *) expr),
						 exprCollation((Node *) expr),
						 (Index) 0);
		tle = makeTargetEntry((Expr *) varref,
							  list_length(altnode->args) + 1,
							  NULL,
							  false);
		altnode->args = lappend(altnode->args, tle);
	}
	return altnode;
}

static bool
gpupreagg_rewrite_expr(Agg *agg,
					   List **p_agg_tlist,
					   List **p_agg_quals,
					   List **p_pre_tlist)
{
	Plan	   *outer_plan = outerPlan(agg);
	List	   *pre_tlist = NIL;
	List	   *agg_tlist = NIL;
	List	   *agg_quals = NIL;
	ListCell   *cell;
	int			i;

	/* In case of sort-aggregate, it has an underlying Sort node on top
	 * of the scan node. GpuPreAgg shall be injected under the Sort node
	 * to reduce burden of CPU sorting.
	 */
	if (agg->aggstrategy == AGG_SORTED)
	{
		Assert(IsA(outer_plan, Sort));
		outer_plan = outerPlan(outer_plan);
	}

	/* Head of target-list keeps original order not to adjust expression 
	 * nodes in the Agg (and Sort if exists) node, but replaced to NULL
	 * except for group-by key because all the non-key variables have to
	 * be partial calculation result.
	 */
	i = 0;
	foreach (cell, outer_plan->targetlist)
	{
		TargetEntry	*tle = lfirst(cell);
		TargetEntry *tle_new;
		Oid			type_oid = exprType((Node *) tle->expr);
		int32		type_mod = exprTypmod((Node *) tle->expr);
		Oid			type_coll = exprCollation((Node *) tle->expr);
		char	   *resname = (tle->resname ? pstrdup(tle->resname) : NULL);

		Assert(IsA(tle, TargetEntry));
		if (i < agg->numCols && tle->resno == agg->grpColIdx[i])
		{
			devtype_info   *dtype = pgstrom_devtype_lookup(type_oid);
			Expr		   *var;

			/* grouping key must comparison function in kernel */
			if (!OidIsValid(dtype->type_cmpfunc) ||
				!pgstrom_devfunc_lookup(dtype->type_cmpfunc))
				return false;

			var = (Expr *) makeVar(OUTER_VAR,
								   tle->resno,
								   type_oid,
								   type_mod,
								   type_coll,
								   0);
			tle_new = makeTargetEntry(var,
									  list_length(pre_tlist) + 1,
									  resname,
									  tle->resjunk);
			pre_tlist = lappend(pre_tlist, tle_new);
			i++;
		}
		else
		{
			Const  *cnst = makeNullConst(type_oid, type_mod, type_coll);

			tle_new = makeTargetEntry((Expr *) cnst,
									  list_length(pre_tlist) + 1,
									  resname,
									  tle->resjunk);
			pre_tlist = lappend(pre_tlist, tle_new);
		}
	}

	/* On the next, replace aggregate functions in tlist of Agg node
	 * according to the aggfunc_catalog[] definition.
	 */
	foreach (cell, agg->plan.targetlist)
	{
		TargetEntry *tle = lfirst(cell);

		if (IsA(tle->expr, Aggref))
		{
			/* aggregate shall be replaced to the alternative one */
			Aggref *altagg = make_gpupreagg_refnode((Aggref *) tle->expr,
													&pre_tlist);
			if (!altagg)
				return false;

			tle = flatCopyTargetEntry(tle);
			tle->expr = (Expr *) altagg;
			agg_tlist = lappend(agg_tlist, tle);
		}
		else
		{
			/* Except for Aggref, node shall be an expression node that
			 * contains references to group-by keys. No needs to replace.
			 */
			Bitmapset  *tempset = NULL;
			int			col;

			pull_varattnos((Node *) tle->expr, OUTER_VAR, &tempset);
			while ((col = bms_first_member(tempset)) >= 0)
			{
				col += FirstLowInvalidHeapAttributeNumber;

				for (i=0; i < agg->numCols; i++)
				{
					if (col == agg->grpColIdx[i])
						break;
				}
				if (i == agg->numCols)
					elog(ERROR, "Bug? references to out of grouping key");
			}
			bms_free(tempset);
			agg_tlist = lappend(agg_tlist, copyObject(tle));
			continue;
		}
	}

	/* At the last, replace aggregate functions in qual of Agg node (that
	 * is used to handle HAVING clause), according to the catalog definition.
	 */
	foreach (cell, agg->plan.qual)
	{
		Expr	   *expr = lfirst(cell);

		if (IsA(expr, Aggref))
		{
			Aggref *altagg = make_gpupreagg_refnode((Aggref *) expr,
													&pre_tlist);
			if (!altagg)
				return false;
			agg_quals = lappend(agg_quals, altagg);
		}
		else
		{
			/* Except for Aggref, all expression can reference are columns
			 * being grouped. 
			 */
			Bitmapset  *tempset = NULL;
			int			col;

			pull_varattnos((Node *) expr, OUTER_VAR, &tempset);
			while ((col = bms_first_member(tempset)) >= 0)
			{
				col += FirstLowInvalidHeapAttributeNumber;

				for (i=0; i < agg->numCols; i++)
				{
					if (col == agg->grpColIdx[i])
						break;
				}
				if (i == agg->numCols)
					elog(ERROR, "Bug? references to out of grouping key");
			}
			bms_free(tempset);
			agg_quals = lappend(agg_quals, copyObject(expr));
		}
	}
	*p_pre_tlist = pre_tlist;
	*p_agg_tlist = agg_tlist;
	*p_agg_quals = agg_quals;
	return true;
}

/*
 * gpupreagg_codegen_keycomp - code generator of kernel gpupreagg_keycomp();
 * that compares two records indexed by x_index and y_index in kern_data_store,
 * then returns -1 if X < Y, 0 if X = Y or 1 if X > Y.
 *
 * static cl_int
 * gpupreagg_keycomp(__private cl_int *errcode,
 *                   __global kern_data_store *kds,
 *                   __global kern_toastbuf *ktoast,
 *                   size_t x_index,
 *                   size_t y_index);
 */
static char *
gpupreagg_codegen_keycomp(GpuPreAggPlan *gpreagg, codegen_context *context)
{
	StringInfoData	str;
	StringInfoData	decl;
	StringInfoData	body;
	int			i;

	initStringInfo(&str);
	initStringInfo(&decl);
    initStringInfo(&body);

	for (i=0; i < gpreagg->numCols; i++)
	{
		TargetEntry	   *tle;
		AttrNumber		resno = gpreagg->grpColIdx[i];
		Var			   *var;
		devtype_info   *dtype;
		devfunc_info   *dfunc;

		tle = get_tle_by_resno(gpreagg->cplan.plan.targetlist, resno);
		var = (Var *) tle->expr;
		if (!IsA(var, Var) || var->varno != OUTER_VAR)
			elog(ERROR, "Bug? A simple Var node is expected for group key: %s",
				 nodeToString(var));

		/* find a function to compare this data-type */
		/* find a datatype for comparison */
		dtype = pgstrom_devtype_lookup_and_track(var->vartype, context);
		if (!OidIsValid(dtype->type_cmpfunc))
			elog(ERROR, "Bug? type (%u) has no comparison function", var->vartype);
		dfunc = pgstrom_devfunc_lookup_and_track(dtype->type_cmpfunc, context);

		/* variable declarations */
		appendStringInfo(&decl,
						 "  pg_%s_t xkeyval_%u;\n"
						 "  pg_%s_t ykeyval_%u;\n",
						 dtype->type_name, resno,
						 dtype->type_name, resno);
		/* comparison logic */
		appendStringInfo(
			&body,
			"  xkeyval_%u = pg_%s_vref(kds,ktoast,errcode,%u,x_index);\n"
			"  ykeyval_%u = pg_%s_vref(kds,ktoast,errcode,%u,y_index);\n"
			"  if (!xkeyval_%u.isnull && !ykeyval_%u.isnull)\n"
			"  {\n"
			"    comp = pgfn_%s(errcode, xkeyval_%u, ykeyval_%u);\n"
			"    if (!comp.isnull && comp.value != 0)\n"
			"      return comp.value;\n"
			"  }\n"
			"  else if (xkeyval_%u.isnull  && !ykeyval_%u.isnull)\n"
			"    return -1;\n"
			"  else if (!xkeyval_%u.isnull &&  ykeyval_%u.isnull)\n"
			"    return 1;\n",
			resno, dtype->type_name, resno - 1,
			resno, dtype->type_name, resno - 1,
			resno, resno,
			dfunc->func_name, resno, resno,
			resno, resno,
			resno, resno);
	}
	/* make a whole key-compare function */
	appendStringInfo(&str,
					 "static cl_int\n"
					 "gpusort_comp(__private int *errcode,\n"
					 "             __global kern_data_store *kds,\n"
					 "             __global kern_toastbuf *ktoast,\n"
					 "             size_t x_index,\n"
					 "             size_t y_index)\n"
					 "{\n"
					 "  %s\n"
					 "  pg_int4_t comp;\n"
					 "\n"
					 "%s"
					 "  return 0;\n"
					 "}\n",
					 decl.data,
					 body.data);
	pfree(decl.data);
	pfree(body.data);

	return str.data;
}

/*
 * gpupreagg_codegen_aggcalc - code generator of kernel gpupreagg_aggcalc();
 * that implements an operation to calculate partial aggregation.
 * The supplied accum is operated by newval, according to resno.
 *
 * static void
 * gpupreagg_aggcalc(__private cl_int *errcode,
 *                 cl_int resno,
 *                 __local pagg_datum *accum,
 *                 __local pagg_datum *newval);
 */
static inline const char *
typeoid_to_pagg_field_name(Oid type_oid)
{
	switch (type_oid)
	{
		case INT4OID:
			return "int_val";
		case INT8OID:
			return "long_val";
		case FLOAT4OID:
			return "float_val";
		case FLOAT8OID:
			return "double_val";
	}
	elog(ERROR, "unexpected partial aggregate data-type");
}


static char *
gpupreagg_codegen_aggcalc(GpuPreAggPlan *gpreagg, codegen_context *context)
{
	Oid				namespace_oid = get_namespace_oid("pgstrom", false);
    StringInfoData	body;
	ListCell	   *cell;

    initStringInfo(&body);
	appendStringInfo(
		&body,
		"static void\n"
		"gpupreagg_aggcalc(__private cl_int *errcode,\n"
		"                  cl_int resno,\n"
		"                  __local pagg_datum *accum,\n"
		"                  __local pagg_datum *newval)\n"
		"{\n"
		"  switch (resno)\n"
		"  {\n"
		);

	/* NOTE: The targetList of GpuPreAgg are either Const (as NULL),
	 * Var node (as grouping key), or FuncExpr (as partial aggregate
	 * calculation).
	 */
	foreach (cell, gpreagg->cplan.plan.targetlist)
	{
		TargetEntry	   *tle = lfirst(cell);
		FuncExpr	   *func;
		Oid				type_oid;
		const char	   *func_name;
		const char	   *field_name;

		if (!IsA(tle->expr, FuncExpr))
		{
			Assert(IsA(tle->expr, Const) || IsA(tle->expr, Var));
			continue;
		}
		func = (FuncExpr *) tle->expr;
		
		if (namespace_oid != get_func_namespace(func->funcid))
		{
			elog(NOTICE, "Bug? function not in pgstrom schema");
			continue;
		}
		func_name = get_func_name(func->funcid);

		if (strcmp(func_name, "nrows") == 0)
		{
			/* XXX - nrows() should not have NULL */
			field_name = typeoid_to_pagg_field_name(INT4OID);
			appendStringInfo(
				&body,
				"  case %d:\n"
				"    accum->%s += newval->%s;\n"
				"    break;\n",
				tle->resno,
				field_name, field_name);
		}
		else if (strcmp(func_name, "pmax") == 0 ||
				 strcmp(func_name, "pmin") == 0)
		{
			Assert(list_length(func->args) == 1);
			type_oid = exprType(linitial(func->args));
			field_name = typeoid_to_pagg_field_name(type_oid);
			appendStringInfo(
				&body,
				"  case %d:\n"
				"    if (!newval->isnull)\n"
				"    {\n"
				"      if (accum->isnull)\n"
				"        accum->%s = newval->%s;\n"
				"      else\n"
				"        accum->%s = %s(accum->%s, newval->%s);\n"
				"      accum->isnull = false;\n"
				"    }\n"
				"    break;\n",
				tle->resno,
				field_name, field_name,
				field_name, func_name + 1, field_name, field_name);
		}
		else if (strcmp(func_name, "psum") == 0    ||
				 strcmp(func_name, "psum_x2") == 0)
		{
			/* it should never be NULL */
			Assert(list_length(func->args) == 1);
			type_oid = exprType(linitial(func->args));
			field_name = typeoid_to_pagg_field_name(type_oid);
			appendStringInfo(
				&body,
				"  case %d:\n"
				"      if (!newval->isnull)\n"
				"      {\n"
				"        accum->%s += newval->%s;\n"
				"        accum->isnull = false;\n"
				"      }\n"
				"    break;\n",
				tle->resno,
				field_name, field_name);
		}
		else if (strcmp(func_name, "pcov_x") == 0  ||
				 strcmp(func_name, "pcov_y") == 0  ||
				 strcmp(func_name, "pcov_x2") == 0 ||
				 strcmp(func_name, "pcov_y2") == 0 ||
				 strcmp(func_name, "pcov_xy") == 0)
		{
			/* covariance takes only float8 datatype */
			/* it should never be NULL */
			field_name = typeoid_to_pagg_field_name(FLOAT8OID);
			appendStringInfo(
				&body,
				"  case %d:\n"
				"    if (!newval->isnull)\n"
				"    {\n"
				"      accum->%s += newval->%s;\n"
				"      accum->isnull = false;\n"
				"    }\n"
				"    break;\n",
				tle->resno,
				field_name, field_name);
		}
		else
		{
			elog(NOTICE, "Bug? unexpected function: %s", func_name);
		}
	}
	appendStringInfo(
		&body,
		"  default:\n"
		"    break;\n"
		"  }\n"
		"}\n");
	return body.data;
}

/*
 * static void
 * gpupreagg_projection(__private cl_int *errcode,
 *                      __global kern_data_store *kds_in,
 *                      __global kern_data_store *kds_out,
 *                      __global kern_toastbuf *ktoast,
 *                      size_t kds_index);
 */
static char *
gpupreagg_codegen_projection(GpuPreAggPlan *gpreagg, codegen_context *context)
{
	Oid				namespace_oid = get_namespace_oid("pgstrom", false);
	StringInfoData	str;
	StringInfoData	decl;
    StringInfoData	body;
	Expr		   *clause;
	ListCell	   *cell;
	Bitmapset	   *attr_refs = NULL;
	devtype_info   *dtype;
	devfunc_info   *dfunc;
	Plan		   *outer_plan;
	AttrNumber		anum;
	bool			use_temp_int4 = false;
	bool			use_temp_int8 = false;
	bool			use_temp_float8x = false;
	bool			use_temp_float8y = false;

	initStringInfo(&str);
	initStringInfo(&decl);
	initStringInfo(&body);
	foreach (cell, gpreagg->cplan.plan.targetlist)
	{
		TargetEntry	   *tle = lfirst(cell);

		if (IsA(tle->expr, Var))
		{
			Var	   *var = (Var *) tle->expr;

			Assert(var->varno == OUTER_VAR);
			Assert(var->varattno > 0);
			attr_refs = bms_add_member(attr_refs, var->varattno -
									   FirstLowInvalidHeapAttributeNumber);
			dtype = pgstrom_devtype_lookup_and_track(var->vartype, context);
			appendStringInfo(&body,
							 "  /* projection for resource %u */\n",
							 tle->resno);
			appendStringInfo(&body,
							 "  pg_%s_vstore(kds_out, ktoast, errcode,\n"
							 "               %u,kds_index,\n"
							 "               KVAR_%u,local_workbuf);\n",
							 dtype->type_name,
							 tle->resno - 1,
							 var->varattno);
		}
		else if (IsA(tle->expr, Const))
		{
			/* no need to care about NULL variables */
		}
		else if (IsA(tle->expr, FuncExpr))
		{
			FuncExpr   *func = (FuncExpr *) tle->expr;
			const char *func_name;

			appendStringInfo(&body,
							 "  /* projection for resource %u */\n",
							 tle->resno);
			if (namespace_oid != get_func_namespace(func->funcid))
				elog(ERROR, "Bug? unexpected FuncExpr: %s",
					 nodeToString(func));

			pull_varattnos((Node *)func, OUTER_VAR, &attr_refs);

			func_name = get_func_name(func->funcid);
			if (strcmp(func_name, "nrows") == 0)
			{
				dtype = pgstrom_devtype_lookup_and_track(INT4OID, context);
				use_temp_int4 = true;
				appendStringInfo(&body, "  temp_int4.isnull = false;\n");
				if (list_length(func->args) > 0)
				{
					ListCell   *lc;

					appendStringInfo(&body, "  if (");
					foreach (lc, func->args)
					{
						if (lc != list_head(func->args))
							appendStringInfo(&body,
											 " ||\n"
											 "      ");
						appendStringInfo(&body, "EVAL(%s)",
										 pgstrom_codegen_expression(lfirst(lc),
																	context));
					}
					appendStringInfo(&body,
									 ")\n"
									 "    temp_int4.value = 1;\n"
									 "  else\n"
									 "    temp_int4.value = 0;\n");
				}
				else
					appendStringInfo(&body, "  temp_int4.value = 1;\n");
				appendStringInfo(&body,
								 "  pg_%s_vstore(kds_out, ktoast, errcode,\n"
								 "               %u,kds_index,\n"
								 "               temp_int4,local_workbuf);\n",
								 dtype->type_name, tle->resno - 1);
			}
			else if (strcmp(func_name, "pmax") == 0 ||
					 strcmp(func_name, "pmin") == 0 ||
					 strcmp(func_name, "psum") == 0)
			{
				/* Store the original value as-is. In case when clause is conditional
				 * and its result is false, NULL shall be set on "pmax" and "pmin",
				 * or 0 shall be set on "psum".
				 */
				Assert(list_length(func->args) == 1);
				clause = linitial(func->args);
				dtype = pgstrom_devtype_lookup_and_track(exprType((Node *)clause),
														 context);
				Assert(dtype != NULL);

				appendStringInfo(&body,
								 "  pg_%s_vstore(kds_out, ktoast, errcode,\n"
								 "               %u,kds_index,\n"
								 "               %s,local_workbuf);\n",
								 dtype->type_name,
								 tle->resno - 1,
								 pgstrom_codegen_expression((Node *)clause, context));
			}
			else if (strcmp(func_name, "psum_x2") == 0)
			{
				Assert(exprType(linitial(func->args)) == FLOAT8OID);
				use_temp_float8x = true;
				dfunc = pgstrom_devfunc_lookup_and_track(F_FLOAT8MUL, context);
				dtype = pgstrom_devtype_lookup_and_track(FLOAT8OID, context);

				appendStringInfo(&body,
								 "  temp_float8x = %s;\n",
								 pgstrom_codegen_expression((Node *)clause, context));
				appendStringInfo(
					&body,
					"  pg_%s_vstore(kds_out, ktoast, errcode,\n"
					"               %u, kds_index,\n"
					"               pgfn_%s(temp_float8x, temp_float8x),\n"
					"               local_workbuf);\n",
					dtype->type_name,
					tle->resno - 1,
					dfunc->func_name);
			}
			else if (strcmp(func_name, "pcov_x") == 0 ||
					 strcmp(func_name, "pcov_y") == 0 ||
					 strcmp(func_name, "pcov_x2") == 0 ||
					 strcmp(func_name, "pcov_y2") == 0 ||
					 strcmp(func_name, "pcov_xy") == 0)
			{
				Expr   *filter = linitial(func->args);
				Expr   *x_clause = lsecond(func->args);
				Expr   *y_clause = lthird(func->args);

				use_temp_float8x = use_temp_float8y = true;

				if (IsA(filter, Const))
				{
					Const  *cons = (Const *) filter;
					if (cons->consttype == BOOLOID &&
						!cons->constisnull &&
						DatumGetBool(cons->constvalue))
						filter = NULL;	/* no filter, actually */
				}
				appendStringInfo(
					&body,
					"  temp_float8x = %s;\n"
					"  temp_float8y = %s;\n",
					pgstrom_codegen_expression((Node *) x_clause, context),
					pgstrom_codegen_expression((Node *) y_clause, context));
				appendStringInfo(
					&body,
					"  if (temp_float8x.isnull ||\n"
					"      temp_float8y.isnull");
				if (filter)
					appendStringInfo(
						&body,
						" ||\n"
						"      EVAL(%s)",
						pgstrom_codegen_expression((Node *) filter, context));
				appendStringInfo(
					&body,
					")\n"
					"  {\n"
					"    temp_float8x.isnull = false;\n"
					"    temp_float8x.value = 0.0;\n"
					"  }\n");
				/* initial value according to the function */
				if (strcmp(func_name, "pcov_y") == 0)
					appendStringInfo(
						&body,
						"  else\n"
						"    temp_float8y = temp_float8x;\n");
				else if (strcmp(func_name, "pcov_x2") == 0)
					appendStringInfo(
						&body,
						"  else\n"
						"    temp_float8x.value = temp_float8x.value *\n"
						"                         temp_float8x.value;\n");
				else if (strcmp(func_name, "pcov_y2") == 0)
					appendStringInfo(
						&body,
						"  else\n"
						"    temp_float8x.value = temp_float8y.value *\n"
						"                         temp_float8y.value;\n");
				else if (strcmp(func_name, "pcov_y2") == 0)
					appendStringInfo(
						&body,
						"  else\n"
						"    temp_float8x.value = temp_float8x.value *\n"
						"                         temp_float8y.value;\n");
				else if (strcmp(func_name, "pcov_x") != 0)
					elog(ERROR, "unexpected partial covariance function: %s",
						 func_name);

				dtype = pgstrom_devtype_lookup_and_track(FLOAT8OID, context);
				appendStringInfo(
					&body,
					"  pg_%s_vstore(kds_out, ktoast, errcode,\n"
					"               %u,kds_index,\n"
					"               temp_float8x, local_workbuf);\n",
					dtype->type_name,
					tle->resno - 1);
			}
			else 
				elog(ERROR, "Bug? unexpected partial aggregate function: %s",
					 func_name);
		}
		else
			elog(ERROR, "bug? unexpected node type: %s",
				 nodeToString(tle->expr));
	}

	/* declaration of variables */
	outer_plan = outerPlan(gpreagg);
	while ((anum = bms_first_member(attr_refs)) >= 0)
	{
		TargetEntry	   *tle;

		anum += FirstLowInvalidHeapAttributeNumber;

		tle = get_tle_by_resno(outer_plan->targetlist, anum);
		dtype = pgstrom_devtype_lookup_and_track(exprType((Node *) tle->expr),
												 context);
		appendStringInfo(
			&decl,
			"  pg_%s_t KVAR_%u = pg_%s_vref(kds_in,ktoast,errcode,colidx,rowidx);\n",
			dtype->type_name,
			anum,
			dtype->type_name);
	}
	if (use_temp_int4)
		appendStringInfo(&decl, "  pg_int4_t temp_int4;\n");
	if (use_temp_int8)
		appendStringInfo(&decl, "  pg_int8_t temp_int8;\n");
	if (use_temp_float8x)
		appendStringInfo(&decl, "  pg_float8_t temp_float8x;\n");
	if (use_temp_float8y)
		appendStringInfo(&decl, "  pg_float8_t temp_float8y;\n");

	appendStringInfo(
		&str,
		"static void\n"
		"gpupreagg_projection(__private cl_int *errcode,\n"
		"            __global kern_data_store *kds_in,\n"
		"            __global kern_data_store *kds_out,\n"
		"            __global kern_toastbuf *ktoast,\n"
		"            size_t kds_index)\n"
		"{\n"
		"%s"
		"\n"
		"%s"
		"}\n",
		decl.data,
		body.data);

	return str.data;
}

static char *
gpupreagg_codegen(GpuPreAggPlan *gpreagg, codegen_context *context)
{
	StringInfoData	str;
	const char	   *fn_keycomp;
	const char	   *fn_aggcalc;
	const char	   *fn_projection;
	ListCell	   *cell;

	memset(context, 0, sizeof(codegen_context));
	/*
	 * System constants for GpuPreAgg
	 * KPARAM_0 is header portion of kern_data_store of source stream;
	 * thus its table structure reflects outer relation's one.
	 * KPARAM_1 is haeder portion of kern_toastbuf to be referenced by
	 * both of source and destination stream (not that we don't support
	 * partial aggregate that newly generate a varlena value).
	 * KPARAM_2 is header portion of kern_data_store of destination
	 * stream, thus its table structure reflects target-list of this
	 * relation.
	 */
	context->used_params = list_make3(makeNullConst(BYTEAOID, -1, InvalidOid),
									  makeNullConst(BYTEAOID, -1, InvalidOid),
									  makeNullConst(BYTEAOID, -1, InvalidOid));
	context->type_defs = list_make1(pgstrom_devtype_lookup(BYTEAOID));

	/* generate a key comparison function */
	fn_keycomp = gpupreagg_codegen_keycomp(gpreagg, context);
	/* generate a partial aggregate function */
	fn_aggcalc = gpupreagg_codegen_aggcalc(gpreagg, context);
	/* generate an initial data loading function */
	fn_projection = gpupreagg_codegen_projection(gpreagg, context);

	/* OK, add type/function declarations */
	initStringInfo(&str);
	appendStringInfo(&str, "%s", pgstrom_codegen_type_declarations(context));
	foreach (cell, context->type_defs)
	{
		devtype_info   *dtype = lfirst(cell);

		if (dtype->type_flags & DEVTYPE_IS_VARLENA)
		{
			appendStringInfo(&str,
							 "STROMCL_VARLENA_VARSTORE_TEMPLATE(%s);\n",
							 dtype->type_name);
		}
		else
		{
			appendStringInfo(&str,
							 "STROMCL_SIMPLE_VARSTORE_TEMPLATE(%s,%s);\n",
							 dtype->type_name, dtype->type_base);
		}
	}
	appendStringInfo(&str, "\n%s%s",
					 pgstrom_codegen_func_declarations(context),
					 pgstrom_codegen_param_declarations(context, 3));
	appendStringInfo(&str,
					 "%s\n"		/* gpupreagg_keycomp() */
					 "%s\n"		/* gpupreagg_aggcalc() */
					 "%s\n",	/* gpupreagg_projection() */
					 fn_keycomp,
					 fn_aggcalc,
					 fn_projection);
	return str.data;
}

/*
 * pgstrom_try_insert_gpupreagg
 *
 * Entrypoint of the gpupreagg. It checks whether the supplied Aggregate node
 * is consists of all supported expressions.
 */
void
pgstrom_try_insert_gpupreagg(PlannedStmt *pstmt, Agg *agg)
{
	GpuPreAggPlan  *gpreagg;
	List		   *pre_tlist = NIL;
	List		   *agg_quals = NIL;
	List		   *agg_tlist = NIL;
	ListCell	   *cell;
	Cost			startup_cost;
	Cost			total_cost;
	Cost			startup_sort;
	Cost			total_sort;
	const char	   *kern_source;
	codegen_context context;

	/* nothing to do, if feature is turned off */
	if (!pgstrom_enabled || !enable_gpupreagg)
		return;

	/* Try to construct target-list of both Agg and GpuPreAgg node.
	 * If unavailable to construct, it indicates this aggregation
	 * does not support partial aggregation.
	 */
	if (!gpupreagg_rewrite_expr(agg, &agg_tlist, &agg_quals, &pre_tlist))
		return;

	/*
	 * Try to construct a GpuPreAggPlan node.
	 * If aggregate node contains any unsupported expression, we give up
	 * to insert GpuPreAgg node.
	 */
	gpreagg = palloc0(sizeof(GpuPreAggPlan));
	NodeSetTag(gpreagg, T_CustomPlan);
	gpreagg->cplan.methods = &gpupreagg_plan_methods;
	gpreagg->cplan.plan.targetlist = pre_tlist;
	gpreagg->cplan.plan.qual = NIL;
	gpreagg->numCols = agg->numCols;
	gpreagg->grpColIdx = pmemcpy(agg->grpColIdx,
								 sizeof(AttrNumber) * agg->numCols);
	if (agg->aggstrategy != AGG_SORTED)
		outerPlan(gpreagg) = outerPlan(agg);
	else
	{
		Sort   *sort_plan = (Sort *) outerPlan(agg);

		Assert(IsA(sort_plan, Sort));
		outerPlan(gpreagg) = outerPlan(sort_plan);
	}

	/*
	 * Cost estimation of Aggregate node if GpuPreAgg is injected.
	 * Then, compare the two cases. If partial aggregate does not
	 * help the overall aggregation, nothing to do. Elsewhere, we
	 * inject a GpuPreAgg node under the Agg (or Sort) node to
	 * reduce number of rows to be processed.
	 */
	cost_gpupreagg(agg, gpreagg,
				   agg_tlist, agg_quals,
				   &startup_cost, &total_cost,
				   &startup_sort, &total_sort);
	if (agg->plan.total_cost < total_cost)
		return;

	/*
	 * construction of kernel code, according to the above query
	 * rewrites.
	 */
	kern_source = gpupreagg_codegen(gpreagg, &context);
	gpreagg->kern_source = kern_source;
	gpreagg->extra_flags = context.extra_flags;
	gpreagg->used_params = context.used_params;
	pull_varattnos((Node *)context.used_vars,
				   OUTER_VAR,
				   &gpreagg->outer_attrefs);
	foreach (cell, gpreagg->cplan.plan.targetlist)
	{
		TargetEntry	   *tle = lfirst(cell);

		if (IsA(tle->expr, Const))
			continue;
		gpreagg->tlist_attrefs = bms_add_member(gpreagg->tlist_attrefs, tle->resno -
												FirstLowInvalidHeapAttributeNumber);
	}

	/* OK, inject it */
	agg->plan.startup_cost = startup_cost;
	agg->plan.total_cost = total_cost;
	agg->plan.targetlist = agg_tlist;
	agg->plan.qual = agg_quals;

	if (agg->aggstrategy != AGG_SORTED)
		outerPlan(agg) = &gpreagg->cplan.plan;
	else
	{
		Sort   *sort_plan = (Sort *) outerPlan(agg);

		Assert(IsA(sort_plan, Sort));
		sort_plan->plan.startup_cost = startup_sort;
		sort_plan->plan.total_cost = total_sort;
		sort_plan->plan.plan_rows = gpreagg->cplan.plan.plan_rows;
		sort_plan->plan.plan_width = gpreagg->cplan.plan.plan_width;
		sort_plan->plan.targetlist = copyObject(pre_tlist);
		outerPlan(sort_plan) = &gpreagg->cplan.plan;
	}
}

static CustomPlanState *
gpupreagg_begin(CustomPlan *node, EState *estate, int eflags)
{
	GpuPreAggPlan  *gpreagg = (GpuPreAggPlan *) node;
	GpuPreAggState *gpas;
	List		   *used_params;
	TupleDesc		tupdesc;
	Const		   *kparam_0;
	Const		   *kparam_1;
	Const		   *kparam_2;
	bytea		   *kds_head;
	bytea		   *ktoast_head;

	/*
	 * construct a state structure
	 */
	gpas = palloc0(sizeof(GpuPreAggState));
	NodeSetTag(gpas, T_CustomPlanState);
	gpas->cps.ps.plan = &node->plan;
	gpas->cps.ps.state = estate;
	gpas->cps.methods = &gpupreagg_plan_methods;

	/*
	 * create expression context
	 */
	ExecAssignExprContext(estate, &gpas->cps.ps);

	/*
	 * initialize child expression
	 */
	gpas->cps.ps.targetlist = (List *)
		ExecInitExpr((Expr *) node->plan.targetlist, &gpas->cps.ps);
	gpas->cps.ps.qual = NIL;	/* never has qualifier here */

	/*
	 * initialize child node
	 */
	outerPlanState(gpas) = ExecInitNode(outerPlan(gpreagg), estate, eflags);
	gpas->scan_desc = ExecGetResultType(outerPlanState(gpas));
	gpas->scan_slot = ExecAllocTableSlot(&estate->es_tupleTable);
	ExecSetSlotDescriptor(gpas->scan_slot, gpas->scan_desc);
	gpas->outer_done = false;

	/*
	 * initialize result tuple type and projection info
	 */
	ExecInitResultTupleSlot(estate, &gpas->cps.ps);
	ExecAssignResultTypeFromTL(&gpas->cps.ps);
	if (tlist_matches_tupdesc(&gpas->cps.ps,
							  gpas->cps.ps.plan->targetlist,
							  OUTER_VAR,
							  gpas->scan_desc))
		gpas->cps.ps.ps_ProjInfo = NULL;
	else
		gpas->cps.ps.ps_ProjInfo =
			ExecBuildProjectionInfo(gpas->cps.ps.targetlist,
									gpas->cps.ps.ps_ExprContext,
									gpas->cps.ps.ps_ResultTupleSlot,
									gpas->scan_desc);
	/*
	 * construction of both of kds_head (for source and destination) and
	 * ktoast_head template.
	 */
	used_params = copyObject(gpreagg->used_params);
	Assert(list_length(used_params) >= 3);

	/* kds_head of the source relation */
	tupdesc = outerPlanState(gpas)->ps_ResultTupleSlot->tts_tupleDescriptor;
	kparam_0 = (Const *)linitial(used_params);
	Assert(IsA(kparam_0, Const) &&
		   kparam_0->consttype == BYTEAOID &&
		   kparam_0->constisnull);
	kds_head = kparam_make_kds_head(tupdesc, gpreagg->outer_attrefs, 0);
	kparam_0->constvalue = PointerGetDatum(kds_head);
	kparam_0->constisnull = false;

	/* ktoast_head of the source relation, but common for the destination */
	kparam_1 = (Const *)lsecond(used_params);
	Assert(IsA(kparam_1, Const) &&
		   kparam_1->consttype == BYTEAOID &&
		   kparam_1->constisnull);
	ktoast_head = kparam_make_ktoast_head(tupdesc, 0);
	kparam_1->constvalue = PointerGetDatum(ktoast_head);
	kparam_1->constisnull = false;

	/* kds_head of the destination relation, but ktoast_head is shared */
	tupdesc = gpas->cps.ps.ps_ResultTupleSlot->tts_tupleDescriptor;
	kparam_2 = (Const *)lthird(used_params);
    Assert(IsA(kparam_2, Const) &&
           kparam_2->consttype == BYTEAOID &&
           kparam_2->constisnull);
    kds_head = kparam_make_kds_head(tupdesc, gpreagg->tlist_attrefs, 0);
    kparam_2->constvalue = PointerGetDatum(kds_head);
    kparam_2->constisnull = false;

	gpas->kparams = pgstrom_create_kern_parambuf(used_params,
												 gpas->cps.ps.ps_ExprContext);
	/*
	 * Setting up kernel program and message queue
	 */
	gpas->dprog_key = pgstrom_get_devprog_key(gpreagg->kern_source,
											  gpreagg->extra_flags);
	pgstrom_track_object((StromObject *)gpas->dprog_key, 0);
	gpas->mqueue = pgstrom_create_queue();
	pgstrom_track_object(&gpas->mqueue->sobj, 0);

	/*
	 * init misc stuff
	 */
	gpas->curr_chunk = NULL;
	gpas->curr_index = 0;
	dlist_init(&gpas->ready_chunks);

	/*
	 * Is perfmon needed?
	 */
	gpas->pfm.enabled = pgstrom_perfmon_enabled;

	return &gpas->cps;
}

static pgstrom_gpupreagg *
gpupreagg_load_next_outer(GpuPreAggState *gpas)
{
	return NULL;
}

static bool
gpupreagg_next_tuple(GpuPreAggState *gpas, TupleTableSlot *slot)
{
	return false;
}

static TupleTableSlot *
gpupreagg_exec(CustomPlanState *node)
{
	GpuPreAggState	   *gpas = (GpuPreAggState *) node;
	TupleTableSlot	   *slot = gpas->cps.ps.ps_ResultTupleSlot;
	pgstrom_gpupreagg  *gpreagg;

	while (!gpas->curr_chunk ||
		   !gpupreagg_next_tuple(gpas, slot))
	{
		pgstrom_message	   *msg;
		dlist_node		   *dnode;

		/* release current gpupreagg chunk being already fetched */
		if (gpas->curr_chunk)
		{
			msg = &gpas->curr_chunk->msg;
			if (msg->pfm.enabled)
				pgstrom_perfmon_add(&gpas->pfm, &msg->pfm);
			Assert(msg->refcnt == 1);
			pgstrom_untrack_object(&msg->sobj);
			pgstrom_put_message(msg);
			gpas->curr_chunk = NULL;
			gpas->curr_index = 0;
		}

		/*
		 * dequeue the running gpupreagg chunk being already processed.
		 */
		while ((msg = pgstrom_try_dequeue_message(gpas->mqueue)) != NULL)
		{
			Assert(gpas->num_running > 0);
			gpas->num_running--;
			dlist_push_tail(&gpas->ready_chunks, &msg->chain);
		}

		/*
		 * Keep number of asynchronous partial aggregate request a particular
		 * level unless it does not exceed pgstrom_max_async_chunks and any
		 * new response is not replied during the loading.
		 */
		while (!gpas->outer_done &&
			   gpas->num_running <= pgstrom_max_async_chunks)
		{
			gpreagg = gpupreagg_load_next_outer(gpas);
			if (!gpreagg)
				break;	/* outer scan reached to end of the relation */

			if (!pgstrom_enqueue_message(&gpreagg->msg))
			{
				pgstrom_put_message(&gpreagg->msg);
				elog(ERROR, "failed to enqueue pgstrom_gpuhashjoin message");
			}
            gpas->num_running++;

			msg = pgstrom_try_dequeue_message(gpas->mqueue);
			if (msg)
			{
				gpas->num_running--;
				dlist_push_tail(&gpas->ready_chunks, &msg->chain);
				break;
			}
		}

		/*
		 * wait for server's response if no available chunks were replied
		 */
		if (dlist_is_empty(&gpas->ready_chunks))
		{
			/* OK, no more request should be fetched */
			if (gpas->num_running == 0)
				break;
			msg = pgstrom_dequeue_message(gpas->mqueue);
			if (!msg)
				elog(ERROR, "message queue wait timeout");
			gpas->num_running--;
			dlist_push_tail(&gpas->ready_chunks, &msg->chain);
		}

		/*
		 * picks up next available chunks, if any
		 */
		Assert(!dlist_is_empty(&gpas->ready_chunks));
		dnode = dlist_pop_head_node(&gpas->ready_chunks);
		gpreagg = dlist_container(pgstrom_gpupreagg, msg.chain, dnode);

		/*
		 * Raise an error, if significan error was reported
		 */
		if (gpreagg->msg.errcode != StromError_Success)
		{
			if (gpreagg->msg.errcode == CL_BUILD_PROGRAM_FAILURE)
			{
				const char *buildlog
					= pgstrom_get_devprog_errmsg(gpas->dprog_key);
				const char *kern_source
					= ((GpuPreAggPlan *)node->ps.plan)->kern_source;

				ereport(ERROR,
						(errcode(ERRCODE_INTERNAL_ERROR),
						 errmsg("PG-Strom: OpenCL execution error (%s)\n%s",
								pgstrom_strerror(gpreagg->msg.errcode),
								kern_source),
						 errdetail("%s", buildlog)));
			}
			else
			{
				ereport(ERROR,
						(errcode(ERRCODE_INTERNAL_ERROR),
						 errmsg("PG-Strom: OpenCL execution error (%s)",
								pgstrom_strerror(gpreagg->msg.errcode))));
			}
		}
		gpas->curr_chunk = gpreagg;
		gpas->curr_index = 0;
	}
	return slot;
}

static void
gpupreagg_end(CustomPlanState *node)
{
	GpuPreAggState *gpas = (GpuPreAggState *) node;

	/* Clean up subtree */
	ExecEndNode(outerPlanState(node));

	/* Clean out the tuple table */
	ExecClearTuple(node->ps.ps_ResultTupleSlot);
	ExecClearTuple(gpas->scan_slot);

	/* Free the exprcontext */
    ExecFreeExprContext(&node->ps);

	/* Clean up strom objects */
	pgstrom_untrack_object((StromObject *)gpas->dprog_key);
	pgstrom_put_devprog_key(gpas->dprog_key);
	pgstrom_untrack_object(&gpas->mqueue->sobj);
	pgstrom_close_queue(gpas->mqueue);
}

static void
gpupreagg_rescan(CustomPlanState *node)
{}

static void
gpupreagg_explain(CustomPlanState *node, List *ancestors, ExplainState *es)
{
	GpuPreAggState *gpas = (GpuPreAggState *) node;

	show_device_kernel(gpas->dprog_key, es);
	if (es->analyze && gpas->pfm.enabled)
		pgstrom_perfmon_explain(&gpas->pfm, es);
}

static Bitmapset *
gpupreagg_get_relids(CustomPlanState *node)
{
	/* nothing to do in GpuPreAgg */
	return NULL;
}

static void
gpupreagg_textout_plan(StringInfo str, const CustomPlan *node)
{

}

static CustomPlan *
gpupreagg_copy_plan(const CustomPlan *from)
{
	GpuPreAggPlan  *oldnode = (GpuPreAggPlan *) from;
	GpuPreAggPlan  *newnode;

	newnode = palloc0(sizeof(GpuPreAggPlan));
	CopyCustomPlanCommon((Node *) oldnode, (Node *) newnode);

	return &newnode->cplan;
}

/*
 * entrypoint of GpuPreAgg
 */
void
pgstrom_init_gpupreagg(void)
{
	/* enable_gpupreagg parameter */
	DefineCustomBoolVariable("enable_gpupreagg",
							 "Enables the use of GPU preprocessed aggregate",
							 NULL,
							 &enable_gpupreagg,
							 true,
							 PGC_USERSET,
							 GUC_NOT_IN_SAMPLE,
							 NULL, NULL, NULL);

	/* initialization of plan method table */
	memset(&gpupreagg_plan_methods, 0, sizeof(CustomPlanMethods));
	gpupreagg_plan_methods.CustomName          = "GpuPreAgg";
	gpupreagg_plan_methods.BeginCustomPlan     = gpupreagg_begin;
	gpupreagg_plan_methods.ExecCustomPlan      = gpupreagg_exec;
	gpupreagg_plan_methods.EndCustomPlan       = gpupreagg_end;
	gpupreagg_plan_methods.ReScanCustomPlan    = gpupreagg_rescan;
	gpupreagg_plan_methods.ExplainCustomPlan   = gpupreagg_explain;
	gpupreagg_plan_methods.GetRelidsCustomPlan = gpupreagg_get_relids;
	gpupreagg_plan_methods.TextOutCustomPlan   = gpupreagg_textout_plan;
	gpupreagg_plan_methods.CopyCustomPlan      = gpupreagg_copy_plan;
}




/* ----------------------------------------------------------------
 *
 * NOTE: below is the code being run on OpenCL server context
 *
 * ---------------------------------------------------------------- */

typedef struct
{
	pgstrom_gpupreagg  *gpreagg;
	cl_command_queue	kcmdq;
	cl_program			program;
	cl_kernel			kernel;

	cl_int				ev_index;
	cl_event			events[1];
} clstate_gpupreagg;


#if 0
static void
clserv_process_gpupreagg(pgstrom_message *message)
{}
#endif



/* ----------------------------------------------------------------
 *
 * NOTE: below is the function to process enhanced aggregate operations
 *
 * ---------------------------------------------------------------- */

/* gpupreagg_partial_nrows - placeholder function that generate number
 * of rows being included in this partial group.
 */
Datum
gpupreagg_partial_nrows(PG_FUNCTION_ARGS)
{
	int		i;

	for (i=0; i < PG_NARGS(); i++)
	{
		if (PG_ARGISNULL(i) || !PG_GETARG_BOOL(i))
			PG_RETURN_INT32(0);
	}
	PG_RETURN_INT32(1);
}

/* gpupreagg_pseudo_expr - placeholder function that returns the supplied
 * variable as is (even if it is NULL). Used to MIX(), MAX() placeholder.
 */
Datum
gpupreagg_pseudo_expr(PG_FUNCTION_ARGS)
{
	PG_RETURN_DATUM(PG_GETARG_DATUM(0));
}
PG_FUNCTION_INFO_V1(gpupreagg_pseudo_expr);

/* gpupreagg_psum_* - placeholder function that generates partial sum
 * of the arguments. _x2 generates square value of the input
 */
Datum
gpupreagg_psum_int(PG_FUNCTION_ARGS)
{
	Assert(PG_NARGS() == 1);
	if (PG_ARGISNULL(0))
		PG_RETURN_INT64(0);
	PG_RETURN_INT64(PG_GETARG_INT64(0));
}
PG_FUNCTION_INFO_V1(gpupreagg_psum_int);

Datum
gpupreagg_psum_float(PG_FUNCTION_ARGS)
{
	Assert(PG_NARGS() == 1);
	if (PG_ARGISNULL(0))
		PG_RETURN_FLOAT8(0.0);
	PG_RETURN_FLOAT8(PG_GETARG_FLOAT8(0));
}
PG_FUNCTION_INFO_V1(gpupreagg_psum_float);

Datum
gpupreagg_psum_x2_float(PG_FUNCTION_ARGS)
{
	Assert(PG_NARGS() == 1);
	if (PG_ARGISNULL(0))
		PG_RETURN_FLOAT8(0.0);
	PG_RETURN_FLOAT8(PG_GETARG_FLOAT8(0) * PG_GETARG_FLOAT8(0));
}
PG_FUNCTION_INFO_V1(gpupreagg_psum_x2_float);

/* gpupreagg_corr_psum - placeholder function that generates partial sum
 * of the arguments. _x2 generates square value of the input
 */
Datum
gpupreagg_corr_psum_x(PG_FUNCTION_ARGS)
{
	Assert(PG_NARGS() == 3);
	/* Aggregate Filter */
	if (PG_ARGISNULL(0) || !PG_GETARG_BOOL(0))
		PG_RETURN_FLOAT8(0.0);
	/* NULL checks */
	if (PG_ARGISNULL(1) || PG_ARGISNULL(2))
		PG_RETURN_FLOAT8(0.0);
	PG_RETURN_FLOAT8(PG_GETARG_FLOAT8(0));
}
PG_FUNCTION_INFO_V1(gpupreagg_corr_psum_x);

Datum
gpupreagg_corr_psum_y(PG_FUNCTION_ARGS)
{
	Assert(PG_NARGS() == 3);
	/* Aggregate Filter */
	if (PG_ARGISNULL(0) || !PG_GETARG_BOOL(0))
		PG_RETURN_FLOAT8(0.0);
	/* NULL checks */
	if (PG_ARGISNULL(1) || PG_ARGISNULL(2))
		PG_RETURN_FLOAT8(0.0);
	PG_RETURN_FLOAT8(PG_GETARG_FLOAT8(1));
}
PG_FUNCTION_INFO_V1(gpupreagg_corr_psum_y);

Datum
gpupreagg_corr_psum_x2(PG_FUNCTION_ARGS)
{
	Assert(PG_NARGS() == 3);
	/* Aggregate Filter */
	if (PG_ARGISNULL(0) || !PG_GETARG_BOOL(0))
		PG_RETURN_FLOAT8(0.0);
	/* NULL checks */
	if (PG_ARGISNULL(1) || PG_ARGISNULL(2))
		PG_RETURN_FLOAT8(0.0);
	PG_RETURN_FLOAT8(PG_GETARG_FLOAT8(0) * PG_GETARG_FLOAT8(0));
}
PG_FUNCTION_INFO_V1(gpupreagg_corr_psum_x2);

Datum
gpupreagg_corr_psum_y2(PG_FUNCTION_ARGS)
{
	Assert(PG_NARGS() == 3);
	/* Aggregate Filter */
	if (PG_ARGISNULL(0) || !PG_GETARG_BOOL(0))
		PG_RETURN_FLOAT8(0.0);
	/* NULL checks */
	if (PG_ARGISNULL(1) || PG_ARGISNULL(2))
		PG_RETURN_FLOAT8(0.0);
	PG_RETURN_FLOAT8(PG_GETARG_FLOAT8(1) * PG_GETARG_FLOAT8(1));
}
PG_FUNCTION_INFO_V1(gpupreagg_corr_psum_y2);

Datum
gpupreagg_corr_psum_xy(PG_FUNCTION_ARGS)
{
	Assert(PG_NARGS() == 3);
	/* Aggregate Filter */
	if (PG_ARGISNULL(0) || !PG_GETARG_BOOL(0))
		PG_RETURN_FLOAT8(0.0);
	/* NULL checks */
	if (PG_ARGISNULL(1) || PG_ARGISNULL(2))
		PG_RETURN_FLOAT8(0.0);
	PG_RETURN_FLOAT8(PG_GETARG_FLOAT8(0) * PG_GETARG_FLOAT8(1));
}
PG_FUNCTION_INFO_V1(gpupreagg_corr_psum_xy);









/*
 * ex_avg() - an enhanced average calculation that takes two arguments;
 * number of rows in this group and partial sum of the value.
 * Then, it eventually generate mathmatically compatible average value.
 */
Datum
pgstrom_sum_int8_accum(PG_FUNCTION_ARGS)
{
	ArrayType  *transarray;
	int32		nrows = PG_GETARG_INT32(1);
	int64		psum = PG_GETARG_INT64(2);
	int64	   *transdata;

	if (AggCheckCallContext(fcinfo, NULL))
		transarray = PG_GETARG_ARRAYTYPE_P(0);
	else
		transarray = PG_GETARG_ARRAYTYPE_P_COPY(0);

	if (ARR_NDIM(transarray) != 1 ||
		ARR_DIMS(transarray)[0] != 2 ||
		ARR_HASNULL(transarray) ||
		ARR_ELEMTYPE(transarray) != INT8OID)
		elog(ERROR, "Two elements int8 array is expected");

	transdata = (int64 *) ARR_DATA_PTR(transarray);
	transdata[0] += (int64) nrows;	/* # of rows */
	transdata[1] += (int64) psum;	/* partial sum */

	PG_RETURN_ARRAYTYPE_P(transarray);
}
PG_FUNCTION_INFO_V1(pgstrom_int8_accum);

/*
 * The built-in final sum() function that accept int8 generates numeric
 * value, but it does not fit the specification of original int2/int4.
 * So, we put our original implementation that accepet nrows(int4) and
 * partial sum (int8) then generate total sum in int8 form.
 */
Datum
pgstrom_sum_int8_final(PG_FUNCTION_ARGS)
{
	ArrayType  *transarray = PG_GETARG_ARRAYTYPE_P(0);
	int64	   *transdata = (int64 *) ARR_DATA_PTR(transarray);

	if (ARR_NDIM(transarray) != 1 ||
		ARR_DIMS(transarray)[0] != 2 ||
		ARR_HASNULL(transarray) ||
		ARR_ELEMTYPE(transarray) != INT8OID)
		elog(ERROR, "Two elements int8 array is expected");

	transdata = (int64 *) ARR_DATA_PTR(transarray);

	PG_RETURN_INT64(transdata[1]);
}



/* logic copied from utils/adt/float.c */
static inline float8 *
check_float8_array(ArrayType *transarray, int nitems)
{
	/*
	 * We expect the input to be an N-element float array; verify that. We
	 * don't need to use deconstruct_array() since the array data is just
	 * going to look like a C array of N float8 values.
	 */
	if (ARR_NDIM(transarray) != 1 ||
		ARR_DIMS(transarray)[0] != nitems ||
		ARR_HASNULL(transarray) ||
		ARR_ELEMTYPE(transarray) != FLOAT8OID)
		elog(ERROR, "%d-elements float8 array is expected", nitems);
	return (float8 *) ARR_DATA_PTR(transarray);
}

/* logic copied from utils/adt/float.c */
static inline void
check_float8_valid(float8 value, bool inf_is_valid, bool zero_is_valid)
{
	if (isinf(value) && !inf_is_valid)
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("value out of range: overflow")));
	if (value == 0.0 && !zero_is_valid)
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("value out of range: underflow")));
}

Datum
pgstrom_sum_float8_accum(PG_FUNCTION_ARGS)
{
	ArrayType  *transarray;
	int32		nrows = PG_GETARG_INT32(1);
	float8		psum = PG_GETARG_FLOAT8(2);
	float8	   *transdata;
	float8		newsum;

	if (AggCheckCallContext(fcinfo, NULL))
		transarray = PG_GETARG_ARRAYTYPE_P(0);
	else
		transarray = PG_GETARG_ARRAYTYPE_P_COPY(0);

	transdata = check_float8_array(transarray, 3);
	transdata[0] += (float8) nrows;
	newsum = transdata[1] + psum;
	check_float8_valid(newsum, isinf(transdata[1]) || isinf(psum), true);
	transdata[1] = newsum;

    PG_RETURN_ARRAYTYPE_P(transarray);	
}
PG_FUNCTION_INFO_V1(pgstrom_float8_accum);

/*
 * variance and stddev - mathmatical compatible result can be lead using
 * nrows, psum(X) and psum(X*X). So, we track these variables.
 */
Datum
pgstrom_variance_float8_accum(PG_FUNCTION_ARGS)
{
	ArrayType  *transarray = PG_GETARG_ARRAYTYPE_P(0);
	int32		nrows  = PG_GETARG_INT32(1);
	float8		psumX  = PG_GETARG_FLOAT8(2);
	float8		psumX2 = PG_GETARG_FLOAT8(3);
	float8	   *transdata;
	float8		newsumX;
	float8		newsumX2;

	if (AggCheckCallContext(fcinfo, NULL))
		transarray = PG_GETARG_ARRAYTYPE_P(0);
	else
		transarray = PG_GETARG_ARRAYTYPE_P_COPY(0);

	transdata = check_float8_array(transarray, 3);
	transdata[0] += (float8) nrows;
	/* SUM(X) */
	newsumX = transdata[1] + psumX;
	check_float8_valid(newsumX, isinf(transdata[1]) || isinf(psumX), true);
	transdata[1] = newsumX;
	/* SUM(X*X) */
	newsumX2 = transdata[2] + psumX2;
	check_float8_valid(newsumX2, isinf(transdata[2]) || isinf(psumX2), true);
	transdata[2] = newsumX2;

	PG_RETURN_ARRAYTYPE_P(transarray);
}
PG_FUNCTION_INFO_V1(pgstrom_variance_float8_accum);

/*
 * covariance - mathmatical compatible result can be lead using
 * nrows, psum(X), psum(X*X), psum(Y), psum(Y*Y), psum(X*Y)
 */
Datum
pgstrom_covariance_float8_accum(PG_FUNCTION_ARGS)
{
	ArrayType  *transarray;
	int32		nrows  = PG_GETARG_INT32(1);
	float8		psumX  = PG_GETARG_FLOAT8(2);
	float8		psumX2 = PG_GETARG_FLOAT8(3);
	float8		psumY  = PG_GETARG_FLOAT8(4);
	float8		psumY2 = PG_GETARG_FLOAT8(5);
	float8		psumXY = PG_GETARG_FLOAT8(6);
	float8		newval;
	float8	   *transdata;

	if (AggCheckCallContext(fcinfo, NULL))
        transarray = PG_GETARG_ARRAYTYPE_P(0);
    else
        transarray = PG_GETARG_ARRAYTYPE_P_COPY(0);

	transdata = check_float8_array(transarray, 3);
    transdata[0] += (float8) nrows;

	/* SUM(X) */
	newval = transdata[1] + psumX;
	check_float8_valid(newval, isinf(transdata[1]) || isinf(psumX), true);
	transdata[1] = newval;

	/* SUM(X*X) */
	newval = transdata[2] + psumX2;
    check_float8_valid(newval, isinf(transdata[2]) || isinf(psumX2), true);
    transdata[2] = newval;

	/* SUM(Y) */
	newval = transdata[3] + psumY;
	check_float8_valid(newval, isinf(transdata[3]) || isinf(psumY), true);
	transdata[3] = newval;

	/* SUM(Y*Y) */
	newval = transdata[4] + psumY2;
	check_float8_valid(newval, isinf(transdata[4]) || isinf(psumY2), true);
	transdata[4] = newval;

	/* SUM(X*Y) */
	newval = transdata[5] + psumXY;
	check_float8_valid(newval, isinf(transdata[4]) || isinf(psumXY), true);
	transdata[5] = newval;

	PG_RETURN_ARRAYTYPE_P(transarray);
}
PG_FUNCTION_INFO_V1(pgstrom_covariance_float8_accum);
