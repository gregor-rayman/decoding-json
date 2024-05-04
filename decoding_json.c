#include "postgres.h"

#include "access/genam.h"
#include "access/sysattr.h"

#include "catalog/pg_class.h"
#include "catalog/pg_type.h"

#include "nodes/parsenodes.h"

#include "replication/output_plugin.h"
#include "replication/logical.h"

#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/relcache.h"
#include "utils/syscache.h"
#include "utils/typcache.h"

PG_MODULE_MAGIC;

extern void _PG_init(void);
extern void _PG_output_plugin_init(OutputPluginCallbacks* cb);

typedef struct _DecodingJsonData DecodingJsonData;
struct _DecodingJsonData {
  MemoryContext context;
  bool xact_wrote_changes;

};

typedef struct FilterTable
{
  bool  add;
	char	*schema_prefix;
  int   schema_prefix_len;
	char	*schema_suffix;
  int   schema_suffic_len;
  char  *schema_name;
  bool  all_schemas;

  char	*table_prefix;
  int   table_prefix_len;
	char	*table_suffix;
  int   table_suffic_len;
  char  *table_name;
  bool  all_tables;

} FilterTable;

static void pg_decode_startup(LogicalDecodingContext* ctx, OutputPluginOptions* opt, bool is_init);
static void pg_decode_shutdown(LogicalDecodingContext* ctx);
static void pg_decode_begin_txn(LogicalDecodingContext* ctx, ReorderBufferTXN* txn);
static void pg_output_begin(LogicalDecodingContext* ctx, DecodingJsonData* data, ReorderBufferTXN* txn, bool last_write);
static void pg_decode_commit_txn(LogicalDecodingContext* ctx, ReorderBufferTXN* txn, XLogRecPtr commit_lsn);
static void pg_decode_change(LogicalDecodingContext* ctx, ReorderBufferTXN* txn, Relation rel, ReorderBufferChange* change);
static void pg_decode_truncate(LogicalDecodingContext* ctx, ReorderBufferTXN* txn, int nrelations, Relation relations[], ReorderBufferChange* change);

void _PG_init(void) {
}

void _PG_output_plugin_init(OutputPluginCallbacks *cb) {
  AssertVariableIsOfType(&_PG_output_plugin_init, LogicalOutputPluginInit);

  cb->startup_cb = pg_decode_startup;
  cb->begin_cb = pg_decode_begin_txn;
  cb->change_cb = pg_decode_change;
  cb->truncate_cb = pg_decode_truncate;
  cb->commit_cb = pg_decode_commit_txn;
  cb->shutdown_cb = pg_decode_shutdown;
}

static void pg_decode_startup(LogicalDecodingContext* ctx, OutputPluginOptions* opt, bool is_init) {
  DecodingJsonData* data;

  data = palloc0(sizeof(DecodingJsonData));
  data->context = AllocSetContextCreate(
    ctx->context,
    "text conversion context",
    ALLOCSET_DEFAULT_MINSIZE,
    ALLOCSET_DEFAULT_INITSIZE,
    ALLOCSET_DEFAULT_MAXSIZE
  );

  ctx->output_plugin_private = data;
  opt->output_type = OUTPUT_PLUGIN_TEXTUAL_OUTPUT;
}

static void pg_decode_shutdown(LogicalDecodingContext* ctx) {
  DecodingJsonData* data = ctx->output_plugin_private;
  MemoryContextDelete(data->context);
}

static void pg_decode_begin_txn(LogicalDecodingContext* ctx, ReorderBufferTXN* txn) {
  DecodingJsonData* data = ctx->output_plugin_private;
  data->xact_wrote_changes = false;
  pg_output_begin(ctx, data, txn, true);
}

static void pg_output_begin(LogicalDecodingContext* ctx, DecodingJsonData* data, ReorderBufferTXN* txn, bool last_write) {
  OutputPluginPrepareWrite(ctx, last_write);
  appendStringInfo(
    ctx->out,
    "{\"type\":\"transaction.begin\",\"xid\":\"%u\",\"committed\":\"%s\"}",
    txn->xid,
    timestamptz_to_str(txn->xact_time.commit_time)
  );
  OutputPluginWrite(ctx, last_write);
}

static void pg_decode_commit_txn(LogicalDecodingContext* ctx, ReorderBufferTXN* txn, XLogRecPtr commit_lsn) {
  OutputPluginPrepareWrite(ctx, true);
  appendStringInfo(
    ctx->out,
    "{\"type\":\"transaction.commit\",\"xid\":\"%u\",\"committed\":\"%s\"}",
    txn->xid,
    timestamptz_to_str(txn->xact_time.commit_time)
  );
  OutputPluginWrite(ctx, true);
}

static void print_literal(StringInfo s, Oid typid, char* outputstr) {
  const char* valptr;

  switch (typid) {
    case INT2OID:
    case INT4OID:
    case INT8OID:
    case OIDOID:
    case FLOAT4OID:
    case FLOAT8OID:
    case NUMERICOID:
      if (pg_strncasecmp(outputstr, "NaN", 3) == 0 ||
        				pg_strncasecmp(outputstr, "Infinity", 8) == 0 ||
    					pg_strncasecmp(outputstr, "-Infinity", 9) == 0)
    					{
    						appendStringInfoChar(s, '"');
    						appendStringInfoString(s, outputstr);
    						appendStringInfoChar(s, '"');
    					}
      else
        appendStringInfoString(s, outputstr);
      break;

    case BITOID:
    case VARBITOID:
      appendStringInfo(s, "\"B'%s'\"", outputstr);
      break;

    case BOOLOID:
      if (strcmp(outputstr, "t") == 0)
        appendStringInfoString(s, "true");
      else
        appendStringInfoString(s, "false");
      break;

    default:
      appendStringInfoChar(s, '"');
      for (valptr = outputstr; *valptr; valptr++) {
        char ch = *valptr;
        if (ch == '\n') {
          appendStringInfoString(s, "\\n");
        } else if (ch == '\r') {
          appendStringInfoString(s, "\\r");
        } else if (ch == '\t') {
          appendStringInfoString(s, "\\t");
        } else if (ch == '"') {
          appendStringInfoString(s, "\\\"");
        } else if (ch == '\\') {
          appendStringInfoString(s, "\\\\");
        } else {
          appendStringInfoChar(s, ch);
        }
      }
      appendStringInfoChar(s, '"');
      break;
  }
}

static void print_value(StringInfo s, TupleDesc tupdesc, HeapTuple tuple, int i) {
  bool typisvarlena;
  bool isnull;
  Oid typoutput;
  Form_pg_attribute attr = &tupdesc->attrs[i];
  Datum origval = fastgetattr(tuple, i + 1, tupdesc, &isnull);
  Oid typid = attr->atttypid;
  getTypeOutputInfo(typid, &typoutput, &typisvarlena);
  if (isnull) {
    appendStringInfoString(s, "null");
  } else if (typisvarlena && VARATT_IS_EXTERNAL_ONDISK(origval)) {
    appendStringInfoString(s, "\"???unchanged-toast-datum???\"");
  } else if (!typisvarlena) {
    print_literal(s, typid, OidOutputFunctionCall(typoutput, origval));
  } else {
    Datum val = PointerGetDatum(PG_DETOAST_DATUM(origval));
    print_literal(s, typid, OidOutputFunctionCall(typoutput, val));
  }
}

static void tuple_to_stringinfo(StringInfo s, TupleDesc tupdesc, HeapTuple tuple, bool skip_nulls) {
  int i;
  bool skip_comma = true;

  for (i = 0; i < tupdesc->natts; i++) {
    Form_pg_attribute attr = &tupdesc->attrs[i];

    if (attr->attisdropped || attr->attnum < 0) {
      continue;
    }

    if (i > 0 && !skip_comma) appendStringInfoChar(s, ',');
    appendStringInfo(s, "\"%s\":", NameStr(attr->attname));
    print_value(s, tupdesc, tuple, i);

    if (skip_comma) {
      skip_comma = false;
    }
  }
}

static 
int string_ends_with(char * str, char * suffix) {
  int str_len = strlen(str);
  int suffix_len = strlen(suffix);

  return 
    (str_len >= suffix_len) &&
    (0 == strcmp(str + (str_len-suffix_len), suffix));
}

static void pg_decode_change(LogicalDecodingContext* ctx, ReorderBufferTXN* txn, Relation relation, ReorderBufferChange* change) {
  DecodingJsonData* data;
  Form_pg_class class_form;
  TupleDesc  tupdesc;
  HeapTuple tuple;
  MemoryContext old;
  char* table_name;
  char* schema_name;

  data = ctx->output_plugin_private;

  data->xact_wrote_changes = true;

  class_form = RelationGetForm(relation);
  tupdesc = RelationGetDescr(relation);

  table_name = NameStr(class_form->relname);
  schema_name = get_namespace_name(
      get_rel_namespace(
        RelationGetRelid(relation)
      )
    );
  
  if (strncmp(table_name, "pg_temp_", 8) == 0) {
    /* ignore */
    return;
  }

  /* [ Cloudfarms specific table filters */

  if (strncmp(schema_name, "farm_", 5) != 0 && strncmp(schema_name, "root_", 5) != 0 && strcmp(schema_name, "public")) {
    /* ignore */
    return;
  }

  if (!strcmp(table_name, "journal_invalid") || string_ends_with(table_name, "_saved")) {
    /* ignore */
    return;
  }

  /* ] Cloudfarms specific table filters */


  old = MemoryContextSwitchTo(data->context);

  OutputPluginPrepareWrite(ctx, true);

  appendStringInfoString(ctx->out, "{\"type\":\"table\"");
  appendStringInfo(
    ctx->out,
    ",\"schema\":\"%s\"", schema_name); /* TODO: Properly escape the schema name */
  appendStringInfo(ctx->out, ",\"name\":\"%s\"", table_name); /* TODO: Properly escape the table name */
  appendStringInfo(
    ctx->out,
    ",\"change\":\"%s\"",
    change->action == REORDER_BUFFER_CHANGE_INSERT
      ? "INSERT"
      : change->action == REORDER_BUFFER_CHANGE_UPDATE
        ? "UPDATE"
        : change->action == REORDER_BUFFER_CHANGE_DELETE
          ? "DELETE"
          : "FIXME"
  );

  if (change->action == REORDER_BUFFER_CHANGE_UPDATE || change->action == REORDER_BUFFER_CHANGE_DELETE || change->action == REORDER_BUFFER_CHANGE_INSERT) {
    appendStringInfoString(ctx->out, ",\"key\":{");
    RelationGetIndexList(relation);
    if (OidIsValid(relation->rd_replidindex)) {
      int i;
      Relation index = index_open(relation->rd_replidindex, ShareLock);
      tuple =
        change->data.tp.oldtuple
          ? &change->data.tp.oldtuple->tuple
          : &change->data.tp.newtuple->tuple;
      for (i = 0; i < index->rd_index->indnatts; i++) {
        int j = index->rd_index->indkey.values[i];
        Form_pg_attribute attr = &tupdesc->attrs[j - 1];
        if (i > 0) appendStringInfoChar(ctx->out, ',');
        appendStringInfo(ctx->out, "\"%s\":", NameStr(attr->attname));
        print_value(ctx->out, tupdesc, tuple, j - 1);
      }
      index_close(index, NoLock);
    } else {
      appendStringInfoString(ctx->out, "\"***FIXME***\":true");
    }
    appendStringInfoChar(ctx->out, '}');
  }

  if (change->action == REORDER_BUFFER_CHANGE_UPDATE || change->action == REORDER_BUFFER_CHANGE_INSERT) {
    appendStringInfoString(ctx->out, ",\"data\":{");
    tuple_to_stringinfo(ctx->out, tupdesc, &change->data.tp.newtuple->tuple, false);
    appendStringInfoChar(ctx->out, '}');
  }
  appendStringInfoChar(ctx->out, '}');

  MemoryContextSwitchTo(old);
  MemoryContextReset(data->context);

  OutputPluginWrite(ctx, true);
}
static void pg_decode_truncate(LogicalDecodingContext* ctx, ReorderBufferTXN* txn, int nrelations, Relation relations[], ReorderBufferChange* change) {
  for (int i = 0; i < nrelations; ++i) {
    DecodingJsonData* data;
    Form_pg_class class_form;
    // TupleDesc  tupdesc;
    //HeapTuple tuple;
    MemoryContext old;
    char* table_name;
    char* schema_name;

    data = ctx->output_plugin_private;

    data->xact_wrote_changes = true;

    class_form = RelationGetForm(relations[i]);
    //tupdesc = RelationGetDescr(relations[i]);

    table_name = NameStr(class_form->relname);
    schema_name = get_namespace_name(
        get_rel_namespace(
          RelationGetRelid(relations[i])
        )
      );
    
    if (strncmp(table_name, "pg_temp_", 8) == 0) {
      /* ignore */
      return;
    }

    /* [ Cloudfarms specific table filters */

    if (strncmp(schema_name, "farm_", 5) != 0 && strncmp(schema_name, "root_", 5) != 0 && strcmp(schema_name, "public")) {
      /* ignore */
      return;
    }

    if (!strcmp(table_name, "journal_invalid") || 
        !strcmp(table_name, "journal") ||
        !strcmp(table_name, "logins") || 
        !strcmp(table_name, "farmchore") ||
        !strcmp(table_name, "appchore") ||
        !strcmp(table_name, "mobiledevice") ||
        !strcmp(table_name,"danavl_update_logger") ||
        string_ends_with(table_name, "_bak") ||         
        string_ends_with(table_name, "_saved")) {
      /* ignore */
      return;
    }

    /* ] Cloudfarms specific table filters */


    old = MemoryContextSwitchTo(data->context);

    OutputPluginPrepareWrite(ctx, true);

    appendStringInfoString(ctx->out, "{\"type\":\"table\"");
    appendStringInfo(
      ctx->out,
      ",\"schema\":\"%s\"", schema_name); /* TODO: Properly escape the schema name */
    appendStringInfo(ctx->out, ",\"name\":\"%s\"", table_name); /* TODO: Properly escape the table name */
    appendStringInfo(
      ctx->out,
      ",\"change\":\"%s\"",
      "TRUNCATE"
    );

    appendStringInfoChar(ctx->out, '}');

    MemoryContextSwitchTo(old);
    MemoryContextReset(data->context);

    OutputPluginWrite(ctx, true);
  }
}

/* adapted from test_decoding.c */

/*-------------------------------------------------------------------------
*
* test_decoding.c
*      example logical decoding output plugin
*
* Copyright (c) 2012-2014, PostgreSQL Global Development Group
*
* IDENTIFICATION
*      contrib/test_decoding/test_decoding.c
*
*-------------------------------------------------------------------------
*/
