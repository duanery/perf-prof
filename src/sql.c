#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <monitor.h>
#include <tep.h>

#include <sqlite/ext.h>

struct sql_ctx {
    sqlite3 *sql;
    struct tp_list *tp_list;
    struct tep_handle *tep;
    struct sql_tp_ctx *tp_ctx;
    int nr_query;
    struct query_ctx {
        sqlite3_stmt *stmt;
        sqlite3_stmt *verify_stmt;
        int *col_widths;
        const char *next_query;
    } *query;

    sqlite3_stmt *update_metadata_stmt;

    sqlite3 *verify_sql;
    struct sql_tp_ctx *verify_tp_ctx;
    sqlite3_stmt *verify_update_metadata_stmt;

    /* Transaction optimization fields */
    bool in_transaction;
    int pending_inserts;
    int batch_size;

    /* Performance statistics */
    uint64_t total_inserts;
    uint64_t total_commits;
    uint64_t total_sql_exec_cost; // ns
    uint64_t sql_exec_cost; // ns
};

static int sqlite_profile(unsigned trace, void *context, void *stmt, void *elapse)
{
    struct sql_ctx *ctx = context;
    if (trace == SQLITE_TRACE_PROFILE) {
        ctx->sql_exec_cost += *(uint64_t *)elapse;
        ctx->total_sql_exec_cost += *(uint64_t *)elapse;
    }
    return 0;
}

static sqlite3 *open_db(const char *filename)
{
    sqlite3 *sql;

    if (sqlite3_open(filename ? : ":memory:", &sql) != SQLITE_OK)
        return NULL;

    /* Single-threaded serial write optimization */
    sqlite3_exec(sql, "PRAGMA page_size = 65536;", NULL, NULL, NULL);
    sqlite3_exec(sql, "PRAGMA journal_mode = OFF;", NULL, NULL, NULL);
    sqlite3_exec(sql, "PRAGMA synchronous = OFF;", NULL, NULL, NULL);
    sqlite3_exec(sql, "PRAGMA locking_mode = EXCLUSIVE;", NULL, NULL, NULL);
    sqlite3_exec(sql, "PRAGMA temp_store = MEMORY;", NULL, NULL, NULL);

    if (filename) {
        /* File database: aggressive optimization for maximum performance */
        sqlite3_exec(sql, "PRAGMA cache_size = -131072;", NULL, NULL, NULL);  /* 128MB cache */
        sqlite3_exec(sql, "PRAGMA mmap_size = 536870912;", NULL, NULL, NULL); /* 512MB mmap */
    } else {
        /* Memory database: lighter configuration */
        sqlite3_exec(sql, "PRAGMA cache_size = -65536;", NULL, NULL, NULL);   /* 64MB cache */
    }
    return sql;
}

static int monitor_ctx_init(struct prof_dev *dev)
{
    struct env *env = dev->env;
    struct sql_ctx *ctx;

    if (env->verbose)
        printf("SQLite %s %s\n", SQLITE_VERSION, SQLITE_SOURCE_ID);

    if (!env->event)
        return -1;

    /*
     * Valid usage scenarios matrix:
     *
     * -i  --query --output2  Behavior
     * --  ------- ---------  --------
     * 0   0       0          Events stored in memory, never used (BLOCKED)
     * 0   0       1          Events saved to database file
     * 0   1       0          Events stored in memory, query on exit (BLOCKED)
     * 0   1       1          Events saved to file, query on exit
     *
     * 1   0       0          Events stored in memory, never used (BLOCKED)
     * 1   0       1          Events saved to file periodically
     * 1   1       0          Periodic query, memory cleared after each query
     * 1   1       1          Periodic query, file table cleared after each query
     *
     * Prevent useless scenarios that consume large amounts of memory:
     * 1. Neither --query nor --output2 is specified.
     *    Events stored in memory database but never queried or persisted.
     * 2. --query without -i (periodic output) and without --output2.
     *    Events accumulated in memory database until program exit.
     */
    if ((!env->query || !env->query[0]) && !env->output2) {
        fprintf(stderr, "Error: Must specify either --query or --output2 (or both).\n");
        fprintf(stderr, "  --query: Execute SQL queries on collected events\n");
        fprintf(stderr, "  --output2: Save events to a database file\n");
        fprintf(stderr, "Without either option, events will be stored in memory with no output.\n");
        return -1;
    }

    if ((env->query && env->query[0]) && !env->output2 && !env->interval) {
        fprintf(stderr, "Error: --query without --output2 requires -i (periodic interval).\n");
        fprintf(stderr, "  Without -i, events accumulate in memory until program exit.\n");
        fprintf(stderr, "  Use -i to periodically execute queries and clear memory,\n");
        fprintf(stderr, "  or use --output2 to persist events to a database file.\n");
        return -1;
    }

    ctx = zalloc(sizeof(*ctx));
    if (!ctx)
        return -1;

    if (env->query && env->query[0]) {
        char *query = env->query, *sep;
        ctx->nr_query = 1;
        while ((sep = next_sep(query, ';'))) {
            query = sep + 1;
            if (query[0])
                ctx->nr_query ++;
        }
    }

    if (ctx->nr_query) {
        ctx->query = calloc(ctx->nr_query, sizeof(*ctx->query));
        if (!ctx->query)
            goto failed;
    }

    ctx->tep = tep__ref_light();

    ctx->tp_list = tp_list_new(dev, env->event);
    if (!ctx->tp_list)
        goto failed;

    ctx->sql = open_db(env->output2);
    if (!ctx->sql)
        goto failed;

    if (env->output2)
        ctx->tp_ctx = sql_tp_file(ctx->sql, ctx->tp_list);
    else
        ctx->tp_ctx = sql_tp_mem(ctx->sql, ctx->tp_list, env->query);
    if (!ctx->tp_ctx)
        goto failed;

    sqlite3_trace_v2(ctx->sql, SQLITE_TRACE_PROFILE, sqlite_profile, ctx);

    if (env->verify) {
        if (!env->output2) {
            const char *verify_filename = "verify_temp.db";
            printf("Creating verification database: %s\n", verify_filename);
            ctx->verify_sql = open_db(verify_filename);
            if (!ctx->verify_sql)
                goto failed;
            unlink(verify_filename);

            ctx->verify_tp_ctx = sql_tp_file(ctx->verify_sql, ctx->tp_list);
            if (!ctx->verify_tp_ctx)
                goto failed;

            sqlite3_trace_v2(ctx->verify_sql, SQLITE_TRACE_PROFILE, sqlite_profile, ctx);
        } else
            printf("--verify disabled: cannot verify when SQL output is saved to file\n");
    }

    ctx->in_transaction = false;
    ctx->pending_inserts = 0;
    ctx->batch_size = (env->output2 || ctx->verify_sql) ? 2000 : INT_MAX;  /* Very Larger batch for memory db */

    ctx->total_inserts = 0;
    ctx->total_commits = 0;
    ctx->total_sql_exec_cost = 0;

    dev->private = ctx;
    return 0;

failed:
    if (ctx->verify_sql)
        sqlite3_close(ctx->verify_sql);
    if (ctx->tp_ctx)
        sql_tp_free(ctx->tp_ctx);
    if (ctx->sql)
        sqlite3_close(ctx->sql);
    if (ctx->tp_list)
        tp_list_free(ctx->tp_list);
    if (ctx->tep)
        tep__unref();
    if (ctx->query)
        free(ctx->query);
    free(ctx);
    return -1;
}

static void monitor_ctx_exit(struct prof_dev *dev)
{
    struct sql_ctx *ctx = dev->private;
    int i;

    if (ctx->tp_ctx)
        sql_tp_free(ctx->tp_ctx);

    if (ctx->update_metadata_stmt)
        sqlite3_finalize(ctx->update_metadata_stmt);
    if (ctx->query) {
        for (i = 0; i < ctx->nr_query; i++) {
            if (ctx->query[i].stmt)
                sqlite3_finalize(ctx->query[i].stmt);
            if (ctx->query[i].verify_stmt)
                sqlite3_finalize(ctx->query[i].verify_stmt);
            if (ctx->query[i].col_widths)
                free(ctx->query[i].col_widths);
        }
        free(ctx->query);
    }

    if (ctx->verify_tp_ctx)
        sql_tp_free(ctx->verify_tp_ctx);
    if (ctx->verify_update_metadata_stmt)
        sqlite3_finalize(ctx->verify_update_metadata_stmt);
    if (ctx->verify_sql)
        sqlite3_close(ctx->verify_sql);

    sqlite3_close(ctx->sql);
    tp_list_free(ctx->tp_list);
    tep__unref();
    free(ctx);
}

static int create_metadata_table(struct sql_ctx *ctx, sqlite3 *sql, sqlite3_stmt **update_metadata_stmt)
{
    struct tp *tp;
    int i;
    const char *metadata_table_fmt =
        "CREATE TABLE IF NOT EXISTS event_metadata ("
            "table_name TEXT PRIMARY KEY, "
            "event_system TEXT NOT NULL, "
            "event_name TEXT NOT NULL, "
            "event_id INTEGER NOT NULL, "
            "filter_expression TEXT, "
            "has_stack BOOLEAN DEFAULT FALSE, "
            "max_stack INTEGER, "
            "field_count INTEGER NOT NULL, "
            "created_time INTEGER NOT NULL, "
            "sample_count INTEGER DEFAULT 0, "
            "first_sample_time INTEGER, "
            "last_sample_time INTEGER, "
            "function_list TEXT "
        ");";

    const char *insert_metadata_fmt =
        "INSERT OR REPLACE INTO event_metadata VALUES "
        "(?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?);";

    const char *update_metadata_fmt =
        "UPDATE event_metadata SET "
            "created_time = ?, "
            "sample_count = ?, "
            "first_sample_time = ?, "
            "last_sample_time = ? "
        "WHERE table_name = ?;";

    sqlite3_stmt *stmt;
    char *errmsg = NULL;

    /* Create metadata table */
    if (sqlite3_exec(sql, metadata_table_fmt, NULL, NULL, &errmsg) != SQLITE_OK) {
        fprintf(stderr, "Failed to create metadata table: %s\n", errmsg);
        sqlite3_free(errmsg);
        return -1;
    }

    /* Prepare insert statement */
    if (sqlite3_prepare_v3(sql, insert_metadata_fmt, -1,
                           SQLITE_PREPARE_PERSISTENT, &stmt, NULL) != SQLITE_OK) {
        fprintf(stderr, "Failed to prepare metadata insert statement: %s\n",
                sqlite3_errmsg(sql));
        return -1;
    }

    /* Insert metadata for each event */
    for_each_real_tp(ctx->tp_list, tp, i) {
        struct tp_private *priv = tp->private;

        sqlite3_bind_text(stmt, 1, priv->table_name, -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 2, tp->sys, -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 3, tp->name, -1, SQLITE_STATIC);
        sqlite3_bind_int(stmt, 4, tp->id);
        sqlite3_bind_text(stmt, 5, tp->filter, -1, SQLITE_STATIC);
        sqlite3_bind_int(stmt, 6, tp->stack ? 1 : 0);
        sqlite3_bind_int(stmt, 7, tp->max_stack);
        sqlite3_bind_int(stmt, 8, priv->nr_fields);
        sqlite3_bind_int64(stmt, 9, priv->created_time); /* created_time */
        sqlite3_bind_int64(stmt, 10, 0);  /* sample_count */
        sqlite3_bind_null(stmt, 11);    /* first_sample_time */
        sqlite3_bind_null(stmt, 12);    /* last_sample_time */
        sqlite3_bind_text(stmt, 13, priv->function_list, -1, SQLITE_STATIC);

        if (sqlite3_step(stmt) != SQLITE_DONE) {
            fprintf(stderr, "Failed to insert metadata for %s: %s\n",
                    priv->table_name, sqlite3_errmsg(sql));
        }

        sqlite3_reset(stmt);
    }

    sqlite3_finalize(stmt);

    /* Prepare update statement */
    if (sqlite3_prepare_v3(sql, update_metadata_fmt, -1,
                           SQLITE_PREPARE_PERSISTENT, update_metadata_stmt, NULL) != SQLITE_OK) {
        fprintf(stderr, "Failed to prepare metadata update statement: %s\n",
                sqlite3_errmsg(sql));
        return -1;
    }
    return 0;
}

static int sql_create_metadata_table(struct prof_dev *dev)
{
    struct sql_ctx *ctx = dev->private;

    if (create_metadata_table(ctx, ctx->sql, &ctx->update_metadata_stmt) < 0)
        return -1;

    if (unlikely(ctx->verify_sql) &&
        create_metadata_table(ctx, ctx->verify_sql, &ctx->verify_update_metadata_stmt) < 0)
        return -1;

    return 0;
}

static int update_metadata_table(struct sql_ctx *ctx, sqlite3 *sql, sqlite3_stmt *update_stmt)
{
    struct tp *tp;
    int i;

    /* Update metadata statistics */
    for_each_real_tp(ctx->tp_list, tp, i) {
        struct tp_private *priv = tp->private;

        sqlite3_reset(update_stmt);
        sqlite3_bind_int64(update_stmt, 1, priv->created_time); /* created_time */
        sqlite3_bind_int64(update_stmt, 2, priv->sample_count);  /* sample_count */
        sqlite3_bind_int64(update_stmt, 3, priv->first_sample_time); /* first_sample_time */
        sqlite3_bind_int64(update_stmt, 4, priv->last_sample_time); /* last_sample_time */
        sqlite3_bind_text(update_stmt, 5, priv->table_name, -1, SQLITE_STATIC);

        if (sqlite3_step(update_stmt) != SQLITE_DONE) {
            fprintf(stderr, "Failed to insert metadata for %s: %s\n",
                    priv->table_name, sqlite3_errmsg(sql));
        }
    }
    return 0;
}

static int sql_update_metadata_table(struct prof_dev *dev)
{
    struct sql_ctx *ctx = dev->private;
    int ret, verify_ret = 0;

    ret = update_metadata_table(ctx, ctx->sql, ctx->update_metadata_stmt);
    if (unlikely(ctx->verify_sql))
        verify_ret = update_metadata_table(ctx, ctx->verify_sql, ctx->verify_update_metadata_stmt);

    return (ret == 0 && verify_ret == 0) ? 0 : -1;
}

static int sql_init(struct prof_dev *dev)
{
    struct perf_evlist *evlist = dev->evlist;
    struct sql_ctx *ctx;
    struct perf_event_attr attr = {
        .type          = PERF_TYPE_TRACEPOINT,
        .config        = 0,
        .size          = sizeof(struct perf_event_attr),
        .sample_period = 1,
        .sample_type   = PERF_SAMPLE_TID | PERF_SAMPLE_TIME | PERF_SAMPLE_ID | PERF_SAMPLE_CPU | PERF_SAMPLE_PERIOD |
                         PERF_SAMPLE_RAW,
        .read_format   = PERF_FORMAT_ID,
        .pinned        = 1,
        .disabled      = 1,
        .watermark     = 1,
    };
    struct perf_evsel *evsel;
    struct tp *tp;
    int i;

    if (monitor_ctx_init(dev) < 0)
        return -1;
    ctx = dev->private;

    prof_dev_env2attr(dev, &attr);

    for_each_real_tp(ctx->tp_list, tp, i) {
        evsel = tp_evsel_new(tp, &attr);
        if (!evsel) {
            goto failed;
        }
        perf_evlist__add(evlist, evsel);
    }

    /* Create and populate metadata table */
    if (sql_create_metadata_table(dev) < 0)
        goto failed;

    return 0;

failed:
    monitor_ctx_exit(dev);
    return -1;
}

static int sql_filter(struct prof_dev *dev)
{
    struct sql_ctx *ctx = dev->private;
    return tp_list_apply_filter(dev, ctx->tp_list);
}
static void sql_interval(struct prof_dev *dev);
static void sql_exit(struct prof_dev *dev)
{
    /* Final flush before exit */
    sql_interval(dev);
    monitor_ctx_exit(dev);
}

static long sql_ftrace_filter(struct prof_dev *dev, union perf_event *event, int cpu, int tid)
{
    struct sql_ctx *ctx = dev->private;
    struct sql_sample_type *data = (void *)event->sample.array;
    struct perf_evsel *evsel = perf_evlist__id_to_evsel(dev->evlist, data->id, NULL);
    struct tp *tp = tp_from_evsel(evsel, ctx->tp_list);

    if (!tp) return 0;
    if (!tp->ftrace_filter)
        return 1;
    return tp_prog_run(tp, tp->ftrace_filter,
           GLOBAL(data->cpu_entry.cpu, data->tid_entry.pid, data->raw.data, data->raw.size));
}

static void commit_transaction(struct sql_ctx *ctx, bool new_transaction)
{
    if (ctx->in_transaction) {
        if (!new_transaction) {
             sqlite3_exec(ctx->sql, "COMMIT;", NULL, NULL, NULL);
             if (unlikely(ctx->verify_sql))
                sqlite3_exec(ctx->verify_sql, "COMMIT;", NULL, NULL, NULL);
             ctx->in_transaction = false;
        } else if (ctx->pending_inserts > 0) {
             sqlite3_exec(ctx->sql, "COMMIT; BEGIN IMMEDIATE TRANSACTION;", NULL, NULL, NULL);
             if (unlikely(ctx->verify_sql))
                sqlite3_exec(ctx->verify_sql, "COMMIT; BEGIN IMMEDIATE TRANSACTION;", NULL, NULL, NULL);
        }

        ctx->total_commits++;
        ctx->pending_inserts = 0;
    }
}

static void ensure_transaction(struct sql_ctx *ctx)
{
    if (!ctx->in_transaction) {
        sqlite3_exec(ctx->sql, "BEGIN IMMEDIATE TRANSACTION;", NULL, NULL, NULL);
        if (unlikely(ctx->verify_sql))
            sqlite3_exec(ctx->verify_sql, "BEGIN IMMEDIATE TRANSACTION;", NULL, NULL, NULL);
        ctx->in_transaction = true;
        ctx->pending_inserts = 0;
    }
}

static void sql_sample(struct prof_dev *dev, union perf_event *event, int cpu, int tid)
{
    struct sql_ctx *ctx = dev->private;
    struct sql_sample_type *data = (void *)event->sample.array;
    struct perf_evsel *evsel;
    struct tp *tp;

    ensure_transaction(ctx);

    evsel = perf_evlist__id_to_evsel(dev->evlist, data->id, NULL);
    tp = tp_from_evsel(evsel, ctx->tp_list);
    if (tp) {
        if (ctx->tp_ctx->sample(ctx->tp_ctx, tp, event) == 0) {
            ctx->total_inserts++;
            ctx->pending_inserts++;
        }
        if (unlikely(ctx->verify_tp_ctx))
            ctx->verify_tp_ctx->sample(ctx->verify_tp_ctx, tp, event);
    }

    /* Optimized commit strategy: batch size only (no time check for single-threaded) */
    if (ctx->pending_inserts >= ctx->batch_size)
        commit_transaction(ctx, true);
}

static void sql_verify_interval(struct prof_dev *dev)
{
    struct env *env = dev->env;
    struct sql_ctx *ctx = dev->private;
    const char *query = env->query;
    int nr_query = 0;
    uint64_t fullscan_step = 0, sort = 0, vm_step = 0;

    while (1) {
        sqlite3_stmt *stmt = ctx->query[nr_query].stmt;
        sqlite3_stmt *verify_stmt = ctx->query[nr_query].verify_stmt;
        const char *next_query = ctx->query[nr_query].next_query;
        int err = SQLITE_OK, verify_err = SQLITE_OK;

        if ((stmt && verify_stmt) ||
            ((err = sqlite3_prepare_v3(ctx->sql, query, -1, SQLITE_PREPARE_PERSISTENT, &stmt, &next_query)) == SQLITE_OK &&
             (verify_err = sqlite3_prepare_v3(ctx->verify_sql, query, -1, SQLITE_PREPARE_PERSISTENT, &verify_stmt, &next_query)) == SQLITE_OK)) {
            int column_count = sqlite3_column_count(stmt);
            int *col_widths = ctx->query[nr_query].col_widths;
            int j, k, width, step_result, verify_step_result = 0;

            printf("=== %.*s ===\n", (int)(next_query - query), query);

            if (column_count != sqlite3_column_count(verify_stmt))
                fprintf(stderr, "Column count mismatch: main=%d, verify=%d\n",
                        column_count, sqlite3_column_count(verify_stmt));

            // Allocate memory and initialize column widths
            if (!col_widths) {
                ctx->query[nr_query].stmt = stmt;
                ctx->query[nr_query].verify_stmt = verify_stmt;
                ctx->query[nr_query].next_query = next_query;

                col_widths = malloc(column_count * sizeof(int));
                if (!col_widths)
                    goto cleanup;

                for (j = 0; j < column_count; j++)
                    col_widths[j] = strlen(sqlite3_column_name(stmt, j));
                ctx->query[nr_query].col_widths = col_widths;
            }

            // Print column headers with proper alignment
            for (j = 0; j < column_count; j++) {
                if (j > 0) printf(" | ");
                printf("%-*s", col_widths[j], sqlite3_column_name(stmt, j));
            }
            if (column_count) printf("\n");

            // Print separator
            for (j = 0; j < column_count; j++) {
                if (j > 0) printf("-+-");
                for (k = 0; k < col_widths[j]; k++)
                    printf("-");
            }
            if (column_count) printf("\n");

            for (j = 0; j < column_count; j++) {
                if (strcmp(sqlite3_column_name(stmt, j), sqlite3_column_name(verify_stmt, j)) != 0)
                    fprintf(stderr, "Column %d name mismatch: main='%s', verify='%s'\n", j,
                            sqlite3_column_name(stmt, j), sqlite3_column_name(verify_stmt, j));
            }

            // Print rows data
            while ((step_result = sqlite3_step(stmt)) == SQLITE_ROW &&
                   (verify_step_result = sqlite3_step(verify_stmt)) == SQLITE_ROW) {
                for (j = 0; j < column_count; j++) {
                    if (j > 0) printf(" | ");
                    switch (sqlite3_column_type(stmt, j)) {
                    case SQLITE_INTEGER:
                        width = printf("%-*lld", col_widths[j], sqlite3_column_int64(stmt, j));
                        break;
                    case SQLITE_FLOAT:
                        width = printf("%-*.6f", col_widths[j], sqlite3_column_double(stmt, j));
                        break;
                    case SQLITE_TEXT:
                        width = printf("%-*s", col_widths[j], sqlite3_column_text(stmt, j));
                        break;
                    case SQLITE_BLOB:
                        width = printf("[BLOB:%d]", sqlite3_column_bytes(stmt, j));
                        break;
                    case SQLITE_NULL:
                        width = printf("NULL");
                        break;
                    default:
                        width = printf("?");
                        break;
                    }

                    if (width < col_widths[j])
                        printf("%-*s", col_widths[j] - width, "");
                    else
                        col_widths[j] = width;
                }
                printf("\n");

                for (j = 0; j < column_count; j++) {
                    if (sqlite3_column_type(stmt, j) != sqlite3_column_type(verify_stmt, j))
                            fprintf(stderr, "Column %d type mismatch: main=%d, verify=%d\n", j,
                                    sqlite3_column_type(stmt, j), sqlite3_column_type(verify_stmt, j));

                    switch (sqlite3_column_type(stmt, j)) {
                    case SQLITE_INTEGER:
                        if (sqlite3_column_int64(stmt, j) != sqlite3_column_int64(verify_stmt, j))
                            fprintf(stderr, "Column %d INTEGER mismatch: main=%lld, verify=%lld\n", j,
                                    sqlite3_column_int64(stmt, j), sqlite3_column_int64(verify_stmt, j));
                        break;
                    case SQLITE_FLOAT:
                        if (sqlite3_column_double(stmt, j) != sqlite3_column_double(verify_stmt, j))
                            fprintf(stderr, "Column %d FLOAT mismatch: main=%.6f, verify=%.6f\n", j,
                                    sqlite3_column_double(stmt, j), sqlite3_column_double(verify_stmt, j));
                        break;
                    case SQLITE_TEXT:
                        if (sqlite3_column_text(stmt, j) && sqlite3_column_text(verify_stmt, j) &&
                            strcmp((const char *)sqlite3_column_text(stmt, j), (const char *)sqlite3_column_text(verify_stmt, j)) != 0)
                            fprintf(stderr, "Column %d TEXT mismatch: main='%s', verify='%s'\n", j,
                                    (const char *)sqlite3_column_text(stmt, j), (const char *)sqlite3_column_text(verify_stmt, j));
                        break;
                    case SQLITE_BLOB:
                        if (sqlite3_column_bytes(stmt, j) != sqlite3_column_bytes(verify_stmt, j))
                            fprintf(stderr, "Column %d BLOB size mismatch: main=%d, verify=%d\n", j,
                                    sqlite3_column_bytes(stmt, j), sqlite3_column_bytes(verify_stmt, j));
                        if (memcmp(sqlite3_column_blob(stmt, j), sqlite3_column_blob(verify_stmt, j),
                                   sqlite3_column_bytes(stmt, j)) != 0)
                            fprintf(stderr, "Column %d BLOB data mismatch\n", j);
                        break;
                    default:
                        break;
                    }
                }
            }
            printf("\n");

            /* Check if loop ended due to error */
            if (step_result != SQLITE_DONE) {
                fprintf(stderr, "Main query execution failed: %s\n", sqlite3_errmsg(ctx->sql));
            } else
                verify_step_result = sqlite3_step(verify_stmt);

            if (verify_step_result != SQLITE_DONE) {
                fprintf(stderr, "Verify query execution failed: %s\n", sqlite3_errmsg(ctx->verify_sql));
            }

            if (env->verbose) {
                fullscan_step += sqlite3_stmt_status(stmt, SQLITE_STMTSTATUS_FULLSCAN_STEP, 1);
                sort += sqlite3_stmt_status(stmt, SQLITE_STMTSTATUS_SORT, 1);
                vm_step += sqlite3_stmt_status(stmt, SQLITE_STMTSTATUS_VM_STEP, 1);
            }

        cleanup:
            sqlite3_reset(stmt);
            sqlite3_reset(verify_stmt);
        } else {
            if (err != SQLITE_OK)
                fprintf(stderr, "Failed to prepare main query: %s\n", sqlite3_errmsg(ctx->sql));
            else if (stmt)
                sqlite3_finalize(stmt);
            if (verify_err != SQLITE_OK)
                fprintf(stderr, "Failed to prepare verify query: %s\n", sqlite3_errmsg(ctx->verify_sql));
            else if (verify_stmt)
                sqlite3_finalize(verify_stmt);
            break;
        }

        nr_query ++;
        if (*next_query)
            query = next_query;
        else
            break;
    }

    if (env->verbose) {
        printf("SQL query cost: %lu ms\n", ctx->sql_exec_cost/1000000);
        printf("SQL stmt status: fullscan_step %lu sort %lu vm_step %lu\n", fullscan_step, sort, vm_step);
        ctx->sql_exec_cost = 0;
    }

    if (ctx->tp_ctx->reset)
        ctx->tp_ctx->reset(ctx->tp_ctx);
    if (ctx->verify_tp_ctx->reset)
        ctx->verify_tp_ctx->reset(ctx->verify_tp_ctx);
}

static void sql_interval(struct prof_dev *dev)
{
    struct env *env = dev->env;
    struct sql_ctx *ctx = dev->private;
    const char *query = env->query;
    int nr_query = 0;
    uint64_t fullscan_step = 0, sort = 0, vm_step = 0;

    /* Update metadata table */
    sql_update_metadata_table(dev);

    /* Commit pending transaction */
    commit_transaction(ctx, false);

    if (!env->query || !env->query[0]) {
        printf("Total Inserts: %lu\n", ctx->total_inserts);
        return;
    }

    print_time(stdout);
    printf("\n");

    if (unlikely(ctx->verify_sql))
        return sql_verify_interval(dev);

    while (1) {
        sqlite3_stmt *stmt = ctx->query[nr_query].stmt;
        const char *next_query = ctx->query[nr_query].next_query;

        if (stmt ||
            sqlite3_prepare_v3(ctx->sql, query, -1, SQLITE_PREPARE_PERSISTENT, &stmt, &next_query) == SQLITE_OK) {
            int column_count = sqlite3_column_count(stmt);
            int *col_widths = ctx->query[nr_query].col_widths;
            int j, k, width, step_result;

            printf("=== %.*s ===\n", (int)(next_query - query), query);

            // Allocate memory and initialize column widths
            if (!col_widths) {
                ctx->query[nr_query].stmt = stmt;
                ctx->query[nr_query].next_query = next_query;

                col_widths = malloc(column_count * sizeof(int));
                if (!col_widths)
                    goto cleanup;

                for (j = 0; j < column_count; j++)
                    col_widths[j] = strlen(sqlite3_column_name(stmt, j));
                ctx->query[nr_query].col_widths = col_widths;
            }

            // Print column headers with proper alignment
            for (j = 0; j < column_count; j++) {
                if (j > 0) printf(" | ");
                printf("%-*s", col_widths[j], sqlite3_column_name(stmt, j));
            }
            if (column_count) printf("\n");

            // Print separator
            for (j = 0; j < column_count; j++) {
                if (j > 0) printf("-+-");
                for (k = 0; k < col_widths[j]; k++)
                    printf("-");
            }
            if (column_count) printf("\n");

            // Print rows data
            while ((step_result = sqlite3_step(stmt)) == SQLITE_ROW) {
                for (j = 0; j < column_count; j++) {
                    if (j > 0) printf(" | ");
                    switch (sqlite3_column_type(stmt, j)) {
                    case SQLITE_INTEGER:
                        width = printf("%-*lld", col_widths[j], sqlite3_column_int64(stmt, j));
                        break;
                    case SQLITE_FLOAT:
                        width = printf("%-*.6f", col_widths[j], sqlite3_column_double(stmt, j));
                        break;
                    case SQLITE_TEXT:
                        width = printf("%-*s", col_widths[j], sqlite3_column_text(stmt, j));
                        break;
                    case SQLITE_BLOB:
                        width = printf("[BLOB:%d]", sqlite3_column_bytes(stmt, j));
                        break;
                    case SQLITE_NULL:
                        width = printf("NULL");
                        break;
                    default:
                        width = printf("?");
                        break;
                    }

                    if (width < col_widths[j])
                        printf("%-*s", col_widths[j] - width, "");
                    else
                        col_widths[j] = width;
                }
                printf("\n");
            }

            /* Check if loop ended due to error */
            if (step_result != SQLITE_DONE) {
                fprintf(stderr, "Error executing query: %s\n", sqlite3_errmsg(ctx->sql));
            }

            printf("\n");

            if (env->verbose) {
                fullscan_step += sqlite3_stmt_status(stmt, SQLITE_STMTSTATUS_FULLSCAN_STEP, 1);
                sort += sqlite3_stmt_status(stmt, SQLITE_STMTSTATUS_SORT, 1);
                vm_step += sqlite3_stmt_status(stmt, SQLITE_STMTSTATUS_VM_STEP, 1);
            }

        cleanup:
            sqlite3_reset(stmt);
        } else {
            fprintf(stderr, "Failed to prepare query: %s\n", sqlite3_errmsg(ctx->sql));
            break;
        }

        nr_query ++;
        if (*next_query)
            query = next_query;
        else
            break;
    }

    if (env->verbose) {
        printf("SQL query cost: %lu ms\n", ctx->sql_exec_cost/1000000);
        printf("SQL stmt status: fullscan_step %lu sort %lu vm_step %lu\n", fullscan_step, sort, vm_step);
        ctx->sql_exec_cost = 0;
    }

    if (ctx->tp_ctx->reset)
        ctx->tp_ctx->reset(ctx->tp_ctx);
}

static void sql_print_dev(struct prof_dev *dev, int indent)
{
    struct sql_ctx *ctx = dev->private;
    dev_printf("Total Inserts: %lu\n", ctx->total_inserts);
    dev_printf("Total Commits: %lu\n", ctx->total_commits);
    dev_printf("Total SQL Cost: %lu ms\n", ctx->total_sql_exec_cost / 1000000);
}

static const char *sql_desc[] = PROFILER_DESC("sql",
    "[OPTION...] -e EVENT [--query 'SQL_STATEMENT'] [--output2 DB_FILE] [-i INT]",
    "SQL aggregation analysis tool based on SQLite.",
    "",
    "SYNOPSIS",
    "    Store trace events as SQLite database tables and execute aggregation queries",
    "    for complex statistical analysis on large volumes of events.",
    "",
    "    Dual storage modes:",
    "      Memory mode (default): Uses Virtual Table for zero-copy event access,",
    "          supports constraint pushdown to kernel filters, single-field index",
    "          optimization, and ORDER BY GROUP BY index optimization.",
    "      File mode (--output2): Stores events to database file for persistent",
    "          analysis and larger datasets.",
    "",
    "    Use cases: flexible data queries, multi-dimensional data pivoting, event",
    "    statistics, and data mining for complex analysis scenarios.",
    "    Use --query to execute custom SQL queries on the collected data.",
    "",
    "EVENT TABLE",
    "  Table name: event name (e.g., sched_wakeup) or alias if specified.",
    "  System columns (all tables):",
    "      _pid              Process ID",
    "      _tid              Thread ID",
    "      _time             Event timestamp (nanoseconds)",
    "      _cpu              CPU number",
    "      _period           Sample period",
    "      common_flags      Trace event common flags",
    "      common_preempt_count  Preempt count",
    "      common_pid        Common PID field",
    "  Event-specific columns: mapped from trace event fields",
    "      Use '"PROGRAME" trace -e EVENT help' to view event fields",
    "",
    "METADATA TABLE (event_metadata)",
    "  Columns:",
    "      table_name        Event table name (primary key)",
    "      event_system      Event system (e.g., sched)",
    "      event_name        Event name (e.g., sched_wakeup)",
    "      event_id          Event ID (common_type field)",
    "      filter_expression Filter expression if set",
    "      has_stack         Stack enabled (0/1)",
    "      max_stack         Maximum stack depth",
    "      field_count       Total field count",
    "      created_time      Table creation time (Unix timestamp)",
    "      sample_count      Total sample count",
    "      first_sample_time First sample time (perf timestamp, ns)",
    "      last_sample_time  Last sample time (perf timestamp, ns)",
    "      function_list     Available SQL functions for this event",
    "",
    "BUILT-IN FUNCTIONS",
    "  Functions are auto-registered based on event print_fmt.",
    "  Query available functions:",
    "      SELECT table_name, function_list FROM event_metadata",
    "  Common functions:",
    "      symbolic(field, value) - Convert value to symbol string",
    "      ksymbol(addr)          - Convert kernel address to symbol",
    "      syscall(nr)            - Convert syscall number to name",
    "      ipv4_str(blob)         - Convert IPv4 address to string",
    "      ipv6_str(blob)         - Convert IPv6 address to string",
    "      ipsa_str(blob)         - Convert sockaddr to string",
    "      mac_str(blob)          - Convert MAC address to string",
    "",
    "EVENT ATTR",
    "  alias=STR      Set custom table name (overrides default event name)",
    "                 This allows shorter, more readable table names and helps",
    "                 distinguish multiple same events from the system.",
    "",
    "  index=FIELD    Replace default index column selected by Virtual Table with",
    "                 specified field. This optimizes queries filtering/sorting on",
    "                 the indexed field. Only one index per event is supported.",
    "",
    "EXAMPLES",
    "    "PROGRAME" sql -e sched:sched_wakeup -i 1000 \\",
    "        --query 'SELECT comm, COUNT(*) FROM sched_wakeup GROUP BY comm ORDER BY COUNT(*) DESC'",
    "    "PROGRAME" sql -e sched:sched_wakeup,sched:sched_switch -i 1000 \\",
    "        --query 'SELECT * FROM sched_wakeup WHERE pid > 1000'",
    "    "PROGRAME" sql -e sched:sched_wakeup --output2 events.db -i 10000",
    "    "PROGRAME" sql -e irq:softirq_entry -i 1000 \\",
    "        --query \"SELECT symbolic('vec', vec), COUNT(*) FROM softirq_entry GROUP BY vec\"",
    "    "PROGRAME" sql -e sched:sched_wakeup//alias=wake/,sched:sched_wakeup//index=comm/ -i 1000 \\",
    "        --query 'SELECT comm, COUNT(*) FROM wake GROUP BY comm; SELECT comm, COUNT(*) FROM sched_wakeup WHERE pid > 10 AND pid < 1000 GROUP BY comm'",
    "    "PROGRAME" sql -e sched:sched_wakeup -i 1000 \\",
    "        --query 'SELECT table_name, function_list FROM event_metadata'");
static const char *sql_argv[] = PROFILER_ARGV("sql",
    PROFILER_ARGV_OPTION,
    PROFILER_ARGV_PROFILER, "event", "query", "output2\nSpecify DB file path", "verify");
static profiler sql = {
    .name = "sql",
    .desc = sql_desc,
    .argv = sql_argv,
    .pages = 8,
    .init = sql_init,
    .filter = sql_filter,
    .deinit = sql_exit,
    .print_dev = sql_print_dev,
    .interval = sql_interval,
    .ftrace_filter = sql_ftrace_filter,
    .sample = sql_sample,
};
PROFILER_REGISTER(sql);
