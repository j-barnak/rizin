// SPDX-FileCopyrightText: 2019 thestr4ng3r <info@florianmaerkl.de>
// SPDX-License-Identifier: MIT

#include "sdb.h"

RZ_API int sdb_diff_format(char *str, int size, const SdbDiff *diff) {
	int r = 0;
#define APPENDF(...) \
	do { \
		int sr = snprintf(str, size, __VA_ARGS__); \
		if (sr < 0) { \
			return sr; \
		} \
		r += sr; \
		if (sr >= size) { \
			/* no space left, only measure from now on */ \
			str = NULL; \
			size = 0; \
		} else { \
			str += sr; \
			size -= sr; \
		} \
	} while (0)

	APPENDF("%c%s ", diff->add ? '+' : '-', diff->v ? "  " : "NS");

	RzListIter *it;
	const char *component;
	rz_list_foreach (diff->path, it, component) {
		APPENDF("%s/", component);
	}

	if (diff->v) {
		APPENDF("%s=%s", diff->k, diff->v);
	} else {
		APPENDF("%s", diff->k);
	}

#undef APPENDF
	return r;
}

typedef struct sdb_diff_ctx_t {
	Sdb *a;
	Sdb *b;
	bool equal;
	VALUE_EQ_F eq;
	RzList /*<char *>*/ *path;
	SdbDiffCallback cb;
	void *cb_user;
} SdbDiffCtx;

#define DIFF(ctx, c, ret) \
	do { \
		(ctx)->equal = false; \
		if ((ctx)->cb) { \
			c \
		} else { \
			/* we already know it's not equal and don't care about the rest of the diff */ \
			return ret; \
		} \
	} while (0)

static void sdb_diff_report_ns(SdbDiffCtx *ctx, SdbNs *ns, bool add) {
	SdbDiff diff = { ctx->path, ns->name, NULL, add };
	ctx->cb(&diff, ctx->cb_user);
}

static void sdb_diff_report_kv(SdbDiffCtx *ctx, const char *k, const char *v, bool add) {
	SdbDiff diff = { ctx->path, k, v, add };
	ctx->cb(&diff, ctx->cb_user);
}

typedef struct sdb_diff_kv_cb_ctx {
	SdbDiffCtx *ctx;
	bool add;
} SdbDiffKVCbCtx;

static bool sdb_diff_report_kv_cb(void *user, const SdbKv *kv) {
	const SdbDiffKVCbCtx *ctx = user;
	sdb_diff_report_kv(ctx->ctx, sdbkv_key(kv), sdbkv_value(kv), ctx->add);
	return true;
}

/**
 * just report everything from sdb to buf with prefix
 */
static void sdb_diff_report(SdbDiffCtx *ctx, Sdb *sdb, bool add) {
	RzListIter *it;
	SdbNs *ns;
	rz_list_foreach (sdb->ns, it, ns) {
		sdb_diff_report_ns(ctx, ns, add);
		rz_list_append(ctx->path, ns->name);
		sdb_diff_report(ctx, ns->sdb, add);
		rz_list_pop(ctx->path);
	}
	SdbDiffKVCbCtx cb_ctx = { ctx, add };
	sdb_foreach(sdb, sdb_diff_report_kv_cb, &cb_ctx);
}

static bool sdb_diff_kv_cb(void *user, const SdbKv *kv) {
	const SdbDiffKVCbCtx *ctx = user;
	const char *k = sdbkv_key(kv);
	const char *v = sdbkv_value(kv);
	Sdb *other = ctx->add ? ctx->ctx->a : ctx->ctx->b;
	const char *other_val = sdb_const_get(other, k);
	if (!other_val || !*other_val) {
		DIFF(ctx->ctx,
			sdb_diff_report_kv(ctx->ctx, k, v, ctx->add);
			, false);
	} else if (!ctx->add &&
		(ctx->ctx->eq ? !ctx->ctx->eq(v, other_val) : (strcmp(v, other_val) != 0))) {
		DIFF(ctx->ctx,
			sdb_diff_report_kv(ctx->ctx, k, v, false);
			sdb_diff_report_kv(ctx->ctx, k, other_val, true);
			, false);
	}
	return true;
}

static void sdb_diff_ctx(SdbDiffCtx *ctx) {
	RzListIter *it;
	SdbNs *ns;
	rz_list_foreach (ctx->a->ns, it, ns) {
		Sdb *b_ns = sdb_ns(ctx->b, ns->name, false);
		if (!b_ns) {
			DIFF(ctx,
				sdb_diff_report_ns(ctx, ns, false);
				rz_list_append(ctx->path, ns->name);
				sdb_diff_report(ctx, ns->sdb, false);
				rz_list_pop(ctx->path);
				, );
			continue;
		}
		Sdb *a = ctx->a;
		Sdb *b = ctx->b;
		ctx->a = ns->sdb;
		ctx->b = b_ns;
		rz_list_append(ctx->path, ns->name);
		sdb_diff_ctx(ctx);
		rz_list_pop(ctx->path);
		ctx->a = a;
		ctx->b = b;
	}
	rz_list_foreach (ctx->b->ns, it, ns) {
		if (!sdb_ns(ctx->a, ns->name, false)) {
			DIFF(ctx,
				sdb_diff_report_ns(ctx, ns, true);
				rz_list_append(ctx->path, ns->name);
				sdb_diff_report(ctx, ns->sdb, true);
				rz_list_pop(ctx->path);
				, );
		}
	}
	SdbDiffKVCbCtx kv_ctx = { ctx, false };
	if (!sdb_foreach(ctx->a, sdb_diff_kv_cb, &kv_ctx)) {
		return;
	}
	kv_ctx.add = true;
	sdb_foreach(ctx->b, sdb_diff_kv_cb, &kv_ctx);
}

RZ_API bool sdb_diff(Sdb *a, Sdb *b, SdbDiffCallback cb, void *cb_user) {
	return sdb_diff_eq(a, b, NULL, cb, cb_user);
}

RZ_API bool sdb_diff_eq(Sdb *a, Sdb *b, VALUE_EQ_F eq, SdbDiffCallback cb, void *cb_user) {
	SdbDiffCtx ctx = { 0 };
	ctx.a = a;
	ctx.b = b;
	ctx.equal = true;
	ctx.eq = eq;
	ctx.cb = cb;
	ctx.cb_user = cb_user;
	ctx.path = rz_list_new();
	if (!ctx.path) {
		return false;
	}
	sdb_diff_ctx(&ctx);
	rz_list_free(ctx.path);
	return ctx.equal;
}