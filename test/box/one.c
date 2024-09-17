#include <stdint.h>
#include <string.h>

#include "msgpuck.h"
#include "module.h"

int
get_rv(box_function_ctx_t *ctx, const char *args, const char *args_end)
{
	(void)ctx;
	(void)args_end;
	char name[64];
	uint32_t str_len;
	mp_decode_array(&args);
	const char *str = mp_decode_str(&args, &str_len);
	memcpy(name, str, str_len);
	name[str_len] = '\0';
	void *rvp = box_raw_read_view_new(name);
	uint64_t rv = (uint64_t)rvp;
	char res[16];
	char *end = mp_encode_uint(res, rv);
	box_return_mp(ctx, res, end);
	return 0;
}

int
get_stmt(box_function_ctx_t *ctx, const char *args, const char *args_end)
{
	(void)ctx;
	(void)args_end;
	char sql[1024];
	uint32_t str_len;
	mp_decode_array(&args);
	const char *str = mp_decode_str(&args, &str_len);
	memcpy(sql, str, str_len);
	sql[str_len] = '\0';
	void *stmtp = sql_rv_prepare(sql);
	uint64_t stmt = (uint64_t)stmtp;
	char res[16];
	char *end = mp_encode_uint(res, stmt);
	box_return_mp(ctx, res, end);
	return 0;
}

int
exec_rv_stmt(box_function_ctx_t *ctx, const char *args, const char *args_end)
{
	(void)ctx;
	(void)args_end;
	mp_decode_array(&args);
	uint64_t stmt = mp_decode_uint(&args);
	uint64_t rv = mp_decode_uint(&args);
	const char *res = sql_rv_execute((void *)stmt, (void *)rv);
	const char *end = res;
	mp_next(&end);
	box_return_mp(ctx, res, end);
	return 0;
}
