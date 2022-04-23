/*
 * Copyright (c) 2018 Tim van der Molen <tim@kariliq.nl>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include "config.h"

#include <errno.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <openssl/evp.h>
#ifndef HAVE_EVP_MAC
#include <openssl/hmac.h>
#endif
#include <openssl/sha.h>

#include <sqlite3.h>

#include "sigbak.h"

#define SBK_IV_LEN		16
#define SBK_KEY_LEN		32
#define SBK_CIPHER_KEY_LEN	32
#define SBK_MAC_KEY_LEN		32
#define SBK_DERIV_KEY_LEN	(SBK_CIPHER_KEY_LEN + SBK_MAC_KEY_LEN)
#define SBK_MAC_LEN		10
#define SBK_ROUNDS		250000
#define SBK_HKDF_INFO		"Backup Export"

#define SBK_MENTION_PLACEHOLDER	"\357\277\274"	/* U+FFFC */
#define SBK_MENTION_PREFIX	"@"

/* Based on SignalDatabaseMigrations.kt in the Signal-Android repository */
#define SBK_DB_VERSION_RECIPIENT_IDS		24
#define SBK_DB_VERSION_REACTIONS		37
#define SBK_DB_VERSION_SPLIT_PROFILE_NAMES	43
#define SBK_DB_VERSION_MENTIONS			68
#define SBK_DB_VERSION_THREAD_AUTOINCREMENT	108
#define SBK_DB_VERSION_REACTION_REFACTOR	121

#define sbk_warnx(ctx, ...)	warnx(__VA_ARGS__)

struct sbk_file {
	long		 pos;
	uint32_t	 len;
	uint32_t	 counter;
};

struct sbk_attachment_entry {
	int64_t		 rowid;
	int64_t		 attachmentid;
	struct sbk_file	*file;
	RB_ENTRY(sbk_attachment_entry) entries;
};

RB_HEAD(sbk_attachment_tree, sbk_attachment_entry);

struct sbk_recipient_id {
	char		*old;	/* For older databases */
	int		 new;	/* For newer databases */
};

struct sbk_recipient_entry {
	struct sbk_recipient_id id;
	struct sbk_recipient recipient;
	RB_ENTRY(sbk_recipient_entry) entries;
};

RB_HEAD(sbk_recipient_tree, sbk_recipient_entry);

struct sbk_ctx {
	FILE		*fp;
	sqlite3		*db;
	unsigned int	 db_version;
	struct sbk_attachment_tree attachments;
	struct sbk_recipient_tree recipients;
	EVP_CIPHER_CTX	*cipher_ctx;
#ifdef HAVE_EVP_MAC
	EVP_MAC_CTX	*mac_ctx;
	OSSL_PARAM	 params[3];
#else
	HMAC_CTX	*hmac_ctx;
#endif
	unsigned char	 cipher_key[SBK_CIPHER_KEY_LEN];
	unsigned char	 mac_key[SBK_MAC_KEY_LEN];
	unsigned char	 iv[SBK_IV_LEN];
	uint32_t	 counter;
	unsigned char	*ibuf;
	size_t		 ibufsize;
	unsigned char	*obuf;
	size_t		 obufsize;
	int		 firstframe;
	int		 eof;
	char		*error;
};

static int	sbk_cmp_attachment_entries(struct sbk_attachment_entry *,
		    struct sbk_attachment_entry *);
static int	sbk_cmp_recipient_entries(struct sbk_recipient_entry *,
		    struct sbk_recipient_entry *);

RB_GENERATE_STATIC(sbk_attachment_tree, sbk_attachment_entry, entries,
    sbk_cmp_attachment_entries)

RB_GENERATE_STATIC(sbk_recipient_tree, sbk_recipient_entry, entries,
    sbk_cmp_recipient_entries)

static void
sbk_error_clear(struct sbk_ctx *ctx)
{
	free(ctx->error);
	ctx->error = NULL;
}

static void
sbk_error_set(struct sbk_ctx *ctx, const char *fmt, ...)
{
	va_list	 ap;
	char	*errmsg, *msg;
	int	 saved_errno;

	va_start(ap, fmt);
	saved_errno = errno;
	sbk_error_clear(ctx);
	errmsg = strerror(saved_errno);

	if (fmt == NULL || vasprintf(&msg, fmt, ap) == -1)
		ctx->error = strdup(errmsg);
	else if (asprintf(&ctx->error, "%s: %s", msg, errmsg) == -1)
		ctx->error = msg;
	else
		free(msg);

	errno = saved_errno;
	va_end(ap);
}

static void
sbk_error_setx(struct sbk_ctx *ctx, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	sbk_error_clear(ctx);

	if (fmt == NULL || vasprintf(&ctx->error, fmt, ap) == -1)
		ctx->error = NULL;

	va_end(ap);
}

static void
sbk_error_sqlite_vsetd(struct sbk_ctx *ctx, sqlite3 *db, const char *fmt,
    va_list ap)
{
	const char	*errmsg;
	char		*msg;

	sbk_error_clear(ctx);
	errmsg = sqlite3_errmsg(db);

	if (fmt == NULL || vasprintf(&msg, fmt, ap) == -1)
		ctx->error = strdup(errmsg);
	else if (asprintf(&ctx->error, "%s: %s", msg, errmsg) == -1)
		ctx->error = msg;
	else
		free(msg);
}

static void
sbk_error_sqlite_setd(struct sbk_ctx *ctx, sqlite3 *db, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	sbk_error_sqlite_vsetd(ctx, db, fmt, ap);
	va_end(ap);
}

static void
sbk_error_sqlite_set(struct sbk_ctx *ctx, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	sbk_error_sqlite_vsetd(ctx, ctx->db, fmt, ap);
	va_end(ap);
}

static int
sbk_enlarge_buffers(struct sbk_ctx *ctx, size_t size)
{
	unsigned char *buf;

	if (ctx->ibufsize < size) {
		if ((buf = realloc(ctx->ibuf, size)) == NULL) {
			sbk_error_set(ctx, NULL);
			return -1;
		}
		ctx->ibuf = buf;
		ctx->ibufsize = size;
	}

	if (size > SIZE_MAX - EVP_MAX_BLOCK_LENGTH) {
		sbk_error_setx(ctx, "Buffer size too large");
		return -1;
	}

	size += EVP_MAX_BLOCK_LENGTH;

	if (ctx->obufsize < size) {
		if ((buf = realloc(ctx->obuf, size)) == NULL) {
			sbk_error_set(ctx, NULL);
			return -1;
		}
		ctx->obuf = buf;
		ctx->obufsize = size;
	}

	return 0;
}

static int
sbk_decrypt_init(struct sbk_ctx *ctx, uint32_t counter)
{
#ifdef HAVE_EVP_MAC
	if (!EVP_MAC_init(ctx->mac_ctx, NULL, 0, ctx->params)) {
		sbk_error_setx(ctx, "Cannot initialise MAC");
		return -1;
	}
#else
	if (!HMAC_Init_ex(ctx->hmac_ctx, NULL, 0, NULL, NULL)) {
		sbk_error_setx(ctx, "Cannot initialise MAC");
		return -1;
	}
#endif

	ctx->iv[0] = counter >> 24;
	ctx->iv[1] = counter >> 16;
	ctx->iv[2] = counter >> 8;
	ctx->iv[3] = counter;

	if (!EVP_DecryptInit_ex(ctx->cipher_ctx, NULL, NULL, ctx->cipher_key,
	    ctx->iv)) {
		sbk_error_setx(ctx, "Cannot initialise cipher");
		return -1;
	}

	return 0;
}

static int
sbk_decrypt_update(struct sbk_ctx *ctx, size_t ibuflen, size_t *obuflen)
{
	int len;

#ifdef HAVE_EVP_MAC
	if (!EVP_MAC_update(ctx->mac_ctx, ctx->ibuf, ibuflen)) {
		sbk_error_setx(ctx, "Cannot compute MAC");
		return -1;
	}
#else
	if (!HMAC_Update(ctx->hmac_ctx, ctx->ibuf, ibuflen)) {
		sbk_error_setx(ctx, "Cannot compute MAC");
		return -1;
	}
#endif

	if (!EVP_DecryptUpdate(ctx->cipher_ctx, ctx->obuf, &len, ctx->ibuf,
	    ibuflen)) {
		sbk_error_setx(ctx, "Cannot decrypt data");
		return -1;
	}

	*obuflen = len;
	return 0;
}

static int
sbk_decrypt_final(struct sbk_ctx *ctx, size_t *obuflen,
    const unsigned char *theirmac)
{
	unsigned char	ourmac[EVP_MAX_MD_SIZE];
#ifdef HAVE_EVP_MAC
	size_t		ourmaclen;
#else
	unsigned int	ourmaclen;
#endif
	int		len;

#ifdef HAVE_EVP_MAC
	if (!EVP_MAC_final(ctx->mac_ctx, ourmac, &ourmaclen, sizeof ourmac)) {
		sbk_error_setx(ctx, "Cannot compute MAC");
		return -1;
	}
#else
	if (!HMAC_Final(ctx->hmac_ctx, ourmac, &ourmaclen)) {
		sbk_error_setx(ctx, "Cannot compute MAC");
		return -1;
	}
#endif

	if (memcmp(ourmac, theirmac, SBK_MAC_LEN) != 0) {
		sbk_error_setx(ctx, "MAC mismatch");
		return -1;
	}

	if (!EVP_DecryptFinal_ex(ctx->cipher_ctx, ctx->obuf + *obuflen,
	    &len)) {
		sbk_error_setx(ctx, "Cannot decrypt data");
		return -1;
	}

	*obuflen += len;
	return 0;
}

static int
sbk_read(struct sbk_ctx *ctx, void *ptr, size_t size)
{
	if (fread(ptr, size, 1, ctx->fp) != 1) {
		if (ferror(ctx->fp))
			sbk_error_set(ctx, NULL);
		else
			sbk_error_setx(ctx, "Unexpected end of file");
		return -1;
	}

	return 0;
}

static int
sbk_read_frame(struct sbk_ctx *ctx, size_t *frmlen)
{
	int32_t		len;
	unsigned char	lenbuf[4];

	if (sbk_read(ctx, lenbuf, sizeof lenbuf) == -1)
		return -1;

	len = (lenbuf[0] << 24) | (lenbuf[1] << 16) | (lenbuf[2] << 8) |
	    lenbuf[3];

	if (len <= 0) {
		sbk_error_setx(ctx, "Invalid frame size");
		return -1;
	}

	if (sbk_enlarge_buffers(ctx, len) == -1)
		return -1;

	if (sbk_read(ctx, ctx->ibuf, len) == -1)
		return -1;

	*frmlen = len;
	return 0;
}

static int
sbk_has_file_data(Signal__BackupFrame *frm)
{
	return frm->attachment != NULL || frm->avatar != NULL ||
	    frm->sticker != NULL;
}

static int
sbk_skip_file_data(struct sbk_ctx *ctx, Signal__BackupFrame *frm)
{
	uint32_t len;

	if (frm->attachment != NULL && frm->attachment->has_length)
		len = frm->attachment->length;
	else if (frm->avatar != NULL && frm->avatar->has_length)
		len = frm->avatar->length;
	else if (frm->sticker != NULL && frm->sticker->has_length)
		len = frm->sticker->length;
	else {
		sbk_error_setx(ctx, "Invalid frame");
		return -1;
	}

	if (fseek(ctx->fp, len + SBK_MAC_LEN, SEEK_CUR) == -1) {
		sbk_error_set(ctx, "Cannot seek");
		return -1;
	}

	ctx->counter++;
	return 0;
}

static Signal__BackupFrame *
sbk_unpack_frame(struct sbk_ctx *ctx, unsigned char *buf, size_t len)
{
	Signal__BackupFrame *frm;

	if ((frm = signal__backup_frame__unpack(NULL, len, buf)) == NULL)
		sbk_error_setx(ctx, "Cannot unpack frame");

	return frm;
}

static struct sbk_file *
sbk_get_file(struct sbk_ctx *ctx, Signal__BackupFrame *frm)
{
	struct sbk_file *file;

	if ((file = malloc(sizeof *file)) == NULL) {
		sbk_error_set(ctx, NULL);
		return NULL;
	}

	if ((file->pos = ftell(ctx->fp)) == -1) {
		sbk_error_set(ctx, NULL);
		goto error;
	}

	if (frm->attachment != NULL) {
		if (!frm->attachment->has_length) {
			sbk_error_setx(ctx, "Invalid attachment frame");
			goto error;
		}
		file->len = frm->attachment->length;
	} else if (frm->avatar != NULL) {
		if (!frm->avatar->has_length) {
			sbk_error_setx(ctx, "Invalid avatar frame");
			goto error;
		}
		file->len = frm->avatar->length;
	} else if (frm->sticker != NULL) {
		if (!frm->sticker->has_length) {
			sbk_error_setx(ctx, "Invalid sticker frame");
			goto error;
		}
		file->len = frm->sticker->length;
	}

	file->counter = ctx->counter;
	return file;

error:
	sbk_free_file(file);
	return NULL;
}

Signal__BackupFrame *
sbk_get_frame(struct sbk_ctx *ctx, struct sbk_file **file)
{
	Signal__BackupFrame	*frm;
	size_t			 ibuflen, obuflen;
	unsigned char		*mac;

	if (file != NULL)
		*file = NULL;

	if (ctx->eof)
		return NULL;

	if (sbk_read_frame(ctx, &ibuflen) == -1)
		return NULL;

	/* The first frame is not encrypted */
	if (ctx->firstframe) {
		ctx->firstframe = 0;
		return sbk_unpack_frame(ctx, ctx->ibuf, ibuflen);
	}

	if (ibuflen <= SBK_MAC_LEN) {
		sbk_error_setx(ctx, "Invalid frame size");
		return NULL;
	}

	ibuflen -= SBK_MAC_LEN;
	mac = ctx->ibuf + ibuflen;

	if (sbk_decrypt_init(ctx, ctx->counter) == -1)
		return NULL;

	if (sbk_decrypt_update(ctx, ibuflen, &obuflen) == -1)
		return NULL;

	if (sbk_decrypt_final(ctx, &obuflen, mac) == -1)
		return NULL;

	if ((frm = sbk_unpack_frame(ctx, ctx->obuf, obuflen)) == NULL)
		return NULL;

	if (frm->has_end)
		ctx->eof = 1;

	ctx->counter++;

	if (sbk_has_file_data(frm)) {
		if (file == NULL) {
			if (sbk_skip_file_data(ctx, frm) == -1) {
				sbk_free_frame(frm);
				return NULL;
			}
		} else {
			if ((*file = sbk_get_file(ctx, frm)) == NULL) {
				sbk_free_frame(frm);
				return NULL;
			}
			if (sbk_skip_file_data(ctx, frm) == -1) {
				sbk_free_frame(frm);
				sbk_free_file(*file);
				return NULL;
			}
		}
	}

	return frm;
}

void
sbk_free_frame(Signal__BackupFrame *frm)
{
	if (frm != NULL)
		signal__backup_frame__free_unpacked(frm, NULL);
}

void
sbk_free_file(struct sbk_file *file)
{
	free(file);
}

int
sbk_write_file(struct sbk_ctx *ctx, struct sbk_file *file, FILE *fp)
{
	size_t		ibuflen, len, obuflen;
	unsigned char	mac[SBK_MAC_LEN];

	if (sbk_enlarge_buffers(ctx, BUFSIZ) == -1)
		return -1;

	if (fseek(ctx->fp, file->pos, SEEK_SET) == -1) {
		sbk_error_set(ctx, "Cannot seek");
		return -1;
	}

	if (sbk_decrypt_init(ctx, file->counter) == -1)
		return -1;

#ifdef HAVE_EVP_MAC
	if (!EVP_MAC_update(ctx->mac_ctx, ctx->iv, SBK_IV_LEN)) {
		sbk_error_setx(ctx, "Cannot compute MAC");
		return -1;
	}
#else
	if (!HMAC_Update(ctx->hmac_ctx, ctx->iv, SBK_IV_LEN)) {
		sbk_error_setx(ctx, "Cannot compute MAC");
		return -1;
	}
#endif

	for (len = file->len; len > 0; len -= ibuflen) {
		ibuflen = (len < BUFSIZ) ? len : BUFSIZ;

		if (sbk_read(ctx, ctx->ibuf, ibuflen) == -1)
			return -1;

		if (sbk_decrypt_update(ctx, ibuflen, &obuflen) == -1)
			return -1;

		if (fp != NULL && fwrite(ctx->obuf, obuflen, 1, fp) != 1) {
			sbk_error_set(ctx, "Cannot write file");
			return -1;
		}
	}

	if (sbk_read(ctx, mac, sizeof mac) == -1)
		return -1;

	obuflen = 0;

	if (sbk_decrypt_final(ctx, &obuflen, mac) == -1)
		return -1;

	if (obuflen > 0 && fp != NULL && fwrite(ctx->obuf, obuflen, 1, fp) !=
	    1) {
		sbk_error_set(ctx, "Cannot write file");
		return -1;
	}

	return 0;
}

static char *
sbk_decrypt_file_data(struct sbk_ctx *ctx, struct sbk_file *file,
    size_t *buflen, int terminate)
{
	size_t		 ibuflen, len, obuflen, obufsize;
	unsigned char	 mac[SBK_MAC_LEN];
	char		*obuf, *ptr;

	if (buflen != NULL)
		*buflen = 0;

	if (sbk_enlarge_buffers(ctx, BUFSIZ) == -1)
		return NULL;

	if (fseek(ctx->fp, file->pos, SEEK_SET) == -1) {
		sbk_error_set(ctx, "Cannot seek");
		return NULL;
	}

	if (terminate)
		terminate = 1;

	if ((size_t)file->len > SIZE_MAX - EVP_MAX_BLOCK_LENGTH - terminate) {
		sbk_error_setx(ctx, "File too large");
		return NULL;
	}

	obufsize = file->len + EVP_MAX_BLOCK_LENGTH + terminate;

	if ((obuf = malloc(obufsize)) == NULL) {
		sbk_error_set(ctx, NULL);
		return NULL;
	}

	if (sbk_decrypt_init(ctx, file->counter) == -1)
		goto error;

#ifdef HAVE_EVP_MAC
	if (!EVP_MAC_update(ctx->mac_ctx, ctx->iv, SBK_IV_LEN)) {
		sbk_error_setx(ctx, "Cannot compute MAC");
		goto error;
	}
#else
	if (!HMAC_Update(ctx->hmac_ctx, ctx->iv, SBK_IV_LEN)) {
		sbk_error_setx(ctx, "Cannot compute MAC");
		goto error;
	}
#endif

	ptr = obuf;

	for (len = file->len; len > 0; len -= ibuflen) {
		ibuflen = (len < BUFSIZ) ? len : BUFSIZ;

		if (sbk_read(ctx, ctx->ibuf, ibuflen) == -1)
			goto error;

		if (sbk_decrypt_update(ctx, ibuflen, &obuflen) == -1)
			goto error;

		memcpy(ptr, ctx->obuf, obuflen);
		ptr += obuflen;
	}

	if (sbk_read(ctx, mac, sizeof mac) == -1)
		goto error;

	obuflen = 0;

	if (sbk_decrypt_final(ctx, &obuflen, mac) == -1)
		goto error;

	if (obuflen > 0) {
		memcpy(ptr, ctx->obuf, obuflen);
		ptr += obuflen;
	}

	if (terminate)
		*ptr = '\0';

	if (buflen != NULL)
		*buflen = ptr - obuf;

	return obuf;

error:
	free(obuf);
	return NULL;
}

char *
sbk_get_file_data(struct sbk_ctx *ctx, struct sbk_file *file, size_t *len)
{
	return sbk_decrypt_file_data(ctx, file, len, 0);
}

static char *
sbk_get_file_data_as_string(struct sbk_ctx *ctx, struct sbk_file *file)
{
	return sbk_decrypt_file_data(ctx, file, NULL, 1);
}

static int
sbk_sqlite_bind_blob(struct sbk_ctx *ctx, sqlite3_stmt *stm, int idx,
    const void *val, size_t len)
{
	if (sqlite3_bind_blob(stm, idx, val, len, SQLITE_STATIC) !=
	    SQLITE_OK) {
		sbk_error_sqlite_set(ctx, "Cannot bind SQL parameter");
		return -1;
	}

	return 0;
}

static int
sbk_sqlite_bind_double(struct sbk_ctx *ctx, sqlite3_stmt *stm, int idx,
    double val)
{
	if (sqlite3_bind_double(stm, idx, val) != SQLITE_OK) {
		sbk_error_sqlite_set(ctx, "Cannot bind SQL parameter");
		return -1;
	}

	return 0;
}

static int
sbk_sqlite_bind_int(struct sbk_ctx *ctx, sqlite3_stmt *stm, int idx, int val)
{
	if (sqlite3_bind_int(stm, idx, val) != SQLITE_OK) {
		sbk_error_sqlite_set(ctx, "Cannot bind SQL parameter");
		return -1;
	}

	return 0;
}

static int
sbk_sqlite_bind_int64(struct sbk_ctx *ctx, sqlite3_stmt *stm, int idx,
    sqlite3_int64 val)
{
	if (sqlite3_bind_int64(stm, idx, val) != SQLITE_OK) {
		sbk_error_sqlite_set(ctx, "Cannot bind SQL parameter");
		return -1;
	}

	return 0;
}

static int
sbk_sqlite_bind_null(struct sbk_ctx *ctx, sqlite3_stmt *stm, int idx)
{
	if (sqlite3_bind_null(stm, idx) != SQLITE_OK) {
		sbk_error_sqlite_set(ctx, "Cannot bind SQL parameter");
		return -1;
	}

	return 0;
}

static int
sbk_sqlite_bind_text(struct sbk_ctx *ctx, sqlite3_stmt *stm, int idx,
    const char *val)
{
	if (sqlite3_bind_text(stm, idx, val, -1, SQLITE_STATIC) != SQLITE_OK) {
		sbk_error_sqlite_set(ctx, "Cannot bind SQL parameter");
		return -1;
	}

	return 0;
}

static int
sbk_sqlite_column_text_copy(struct sbk_ctx *ctx, char **buf, sqlite3_stmt *stm,
    int idx)
{
#ifdef notyet
	const unsigned char	*txt;
	int			 len;

	*buf = NULL;

	if (sqlite3_column_type(stm, idx) == SQLITE_NULL)
		return 0;

	if ((txt = sqlite3_column_text(stm, idx)) == NULL) {
		sbk_error_sqlite_set(ctx, "Cannot get column text");
		return -1;
	}

	if ((len = sqlite3_column_bytes(stm, idx)) < 0) {
		sbk_error_sqlite_set(ctx, "Cannot get column size");
		return -1;
	}

	if ((*buf = malloc((size_t)len + 1)) == NULL) {
		sbk_error_set(ctx, NULL);
		return -1;
	}

	memcpy(*buf, txt, (size_t)len + 1);
	return len;
#else
	const unsigned char *txt;

	*buf = NULL;

	if (sqlite3_column_type(stm, idx) == SQLITE_NULL)
		return 0;

	if ((txt = sqlite3_column_text(stm, idx)) == NULL) {
		sbk_error_sqlite_set(ctx, "Cannot get column text");
		return -1;
	}

	if ((*buf = strdup((const char *)txt)) == NULL) {
		sbk_error_set(ctx, NULL);
		return -1;
	}

	return 0;
#endif
}

static int
sbk_sqlite_open(struct sbk_ctx *ctx, sqlite3 **db, const char *path)
{
	if (sqlite3_open(path, db) != SQLITE_OK) {
		sbk_error_sqlite_setd(ctx, *db, "Cannot open database");
		return -1;
	}

	return 0;
}

static int
sbk_sqlite_prepare(struct sbk_ctx *ctx, sqlite3_stmt **stm, const char *query)
{
	if (sqlite3_prepare_v2(ctx->db, query, -1, stm, NULL) != SQLITE_OK) {
		sbk_error_sqlite_set(ctx, "Cannot prepare SQL statement");
		return -1;
	}

	return 0;
}

static int
sbk_sqlite_step(struct sbk_ctx *ctx, sqlite3_stmt *stm)
{
	int ret;

	ret = sqlite3_step(stm);
	if (ret != SQLITE_ROW && ret != SQLITE_DONE)
		sbk_error_sqlite_set(ctx, "Cannot execute SQL statement");

	return ret;
}

static int
sbk_sqlite_exec(struct sbk_ctx *ctx, const char *sql)
{
	char *errmsg;

	if (sqlite3_exec(ctx->db, sql, NULL, NULL, &errmsg) != SQLITE_OK) {
		sbk_error_setx(ctx, "Cannot execute SQL statement: %s",
		    errmsg);
		sqlite3_free(errmsg);
		return -1;
	}

	return 0;
}

static int
sbk_cmp_attachment_entries(struct sbk_attachment_entry *a,
    struct sbk_attachment_entry *b)
{
	if (a->rowid < b->rowid)
		return -1;

	if (a->rowid > b->rowid)
		return 1;

	return (a->attachmentid < b->attachmentid) ? -1 :
	    (a->attachmentid > b->attachmentid);
}

static int
sbk_insert_attachment_entry(struct sbk_ctx *ctx, Signal__BackupFrame *frm,
    struct sbk_file *file)
{
	struct sbk_attachment_entry *entry;

	if (!frm->attachment->has_rowid ||
	    !frm->attachment->has_attachmentid) {
		sbk_error_setx(ctx, "Invalid attachment frame");
		sbk_free_file(file);
		return -1;
	}

	if ((entry = malloc(sizeof *entry)) == NULL) {
		sbk_error_set(ctx, NULL);
		sbk_free_file(file);
		return -1;
	}

	entry->rowid = frm->attachment->rowid;
	entry->attachmentid = frm->attachment->attachmentid;
	entry->file = file;
	RB_INSERT(sbk_attachment_tree, &ctx->attachments, entry);
	return 0;
}

static struct sbk_file *
sbk_get_attachment_file(struct sbk_ctx *ctx, int64_t rowid,
    int64_t attachmentid)
{
	struct sbk_attachment_entry find, *result;

	find.rowid = rowid;
	find.attachmentid = attachmentid;
	result = RB_FIND(sbk_attachment_tree, &ctx->attachments, &find);
	return (result != NULL) ? result->file : NULL;
}

static void
sbk_free_attachment_tree(struct sbk_ctx *ctx)
{
	struct sbk_attachment_entry *entry;

	while ((entry = RB_ROOT(&ctx->attachments)) != NULL) {
		RB_REMOVE(sbk_attachment_tree, &ctx->attachments, entry);
		sbk_free_file(entry->file);
		free(entry);
	}
}

static int
sbk_bind_param(struct sbk_ctx *ctx, sqlite3_stmt *stm, int idx,
    Signal__SqlStatement__SqlParameter *par)
{
	if (par->stringparamter != NULL)
		return sbk_sqlite_bind_text(ctx, stm, idx,
		    par->stringparamter);

	if (par->has_integerparameter)
		return sbk_sqlite_bind_int64(ctx, stm, idx,
		    par->integerparameter);

	if (par->has_doubleparameter)
		return sbk_sqlite_bind_double(ctx, stm, idx,
		    par->doubleparameter);

	if (par->has_blobparameter)
		return sbk_sqlite_bind_blob(ctx, stm, idx,
		    par->blobparameter.data, par->blobparameter.len);

	if (par->has_nullparameter)
		return sbk_sqlite_bind_null(ctx, stm, idx);

	sbk_error_setx(ctx, "Unknown SQL parameter type");
	return -1;
}

static int
sbk_exec_statement(struct sbk_ctx *ctx, Signal__SqlStatement *sql)
{
	sqlite3_stmt	*stm;
	size_t		 i;

	if (sql->statement == NULL) {
		sbk_error_setx(ctx, "Invalid SQL frame");
		return -1;
	}

	/* Don't try to create tables with reserved names */
	if (strncasecmp(sql->statement, "create table sqlite_", 20) == 0)
		return 0;

	if (sbk_sqlite_prepare(ctx, &stm, sql->statement) == -1)
		return -1;

	for (i = 0; i < sql->n_parameters; i++)
		if (sbk_bind_param(ctx, stm, i + 1, sql->parameters[i]) == -1)
			goto error;

	if (sbk_sqlite_step(ctx, stm) != SQLITE_DONE)
		goto error;

	sqlite3_finalize(stm);
	return 0;

error:
	sqlite3_finalize(stm);
	return -1;
}

static int
sbk_set_database_version(struct sbk_ctx *ctx, Signal__DatabaseVersion *ver)
{
	char	*sql;
	int	 ret;

	if (!ver->has_version) {
		sbk_error_setx(ctx, "Invalid version frame");
		return -1;
	}

	ctx->db_version = ver->version;

	if (asprintf(&sql, "PRAGMA user_version = %" PRIu32, ver->version) ==
	    -1) {
		sbk_error_setx(ctx, "asprintf() failed");
		return -1;
	}

	ret = sbk_sqlite_exec(ctx, sql);
	free(sql);
	return ret;
}

static int
sbk_create_database(struct sbk_ctx *ctx)
{
	Signal__BackupFrame	*frm;
	struct sbk_file		*file;
	int			 ret;

	if (ctx->db != NULL)
		return 0;

	if (sbk_sqlite_open(ctx, &ctx->db, ":memory:") == -1)
		goto error;

	if (sbk_rewind(ctx) == -1)
		goto error;

	if (sbk_sqlite_exec(ctx, "BEGIN TRANSACTION") == -1)
		goto error;

	ret = 0;

	while ((frm = sbk_get_frame(ctx, &file)) != NULL) {
		if (frm->version != NULL)
			ret = sbk_set_database_version(ctx, frm->version);
		else if (frm->statement != NULL)
			ret = sbk_exec_statement(ctx, frm->statement);
		else if (frm->attachment != NULL)
			ret = sbk_insert_attachment_entry(ctx, frm, file);
		else
			sbk_free_file(file);

		sbk_free_frame(frm);

		if (ret == -1)
			goto error;
	}

	if (sbk_sqlite_exec(ctx, "END TRANSACTION") == -1)
		goto error;

	if (!ctx->eof)
		goto error;

	return 0;

error:
	sbk_free_attachment_tree(ctx);
	sqlite3_close(ctx->db);
	ctx->db = NULL;
	return -1;
}

int
sbk_write_database(struct sbk_ctx *ctx, const char *path)
{
	sqlite3		*db;
	sqlite3_backup	*bak;

	if (sbk_create_database(ctx) == -1)
		return -1;

	if (sbk_sqlite_open(ctx, &db, path) == -1)
		goto error;

	if ((bak = sqlite3_backup_init(db, "main", ctx->db, "main")) == NULL) {
		sbk_error_sqlite_setd(ctx, db, "Cannot write database");
		goto error;
	}

	if (sqlite3_backup_step(bak, -1) != SQLITE_DONE) {
		sbk_error_sqlite_setd(ctx, db, "Cannot write database");
		sqlite3_backup_finish(bak);
		goto error;
	}

	sqlite3_backup_finish(bak);

	if (sqlite3_close(db) != SQLITE_OK) {
		sbk_error_sqlite_setd(ctx, db, "Cannot close database");
		return -1;
	}

	return 0;

error:
	sqlite3_close(db);
	return -1;
}

static int
sbk_cmp_recipient_entries(struct sbk_recipient_entry *e,
    struct sbk_recipient_entry *f)
{
	if (e->id.old != NULL)
		return strcmp(e->id.old, f->id.old);
	else
		return (e->id.new < f->id.new) ? -1 : (e->id.new > f->id.new);
}

static int
sbk_get_recipient_id_from_column(struct sbk_ctx *ctx,
    struct sbk_recipient_id *id, sqlite3_stmt *stm, int idx)
{
	if (ctx->db_version < SBK_DB_VERSION_RECIPIENT_IDS) {
		id->new = -1;
		if (sbk_sqlite_column_text_copy(ctx, &id->old, stm, idx) == -1)
			return -1;
		if (id->old == NULL) {
			sbk_error_setx(ctx, "Invalid recipient id");
			return -1;
		}
	} else {
		id->new = sqlite3_column_int(stm, idx);
		id->old = NULL;
	}

	return 0;
}

static void
sbk_free_recipient_entry(struct sbk_recipient_entry *ent)
{
	if (ent == NULL)
		return;

	switch (ent->recipient.type) {
	case SBK_CONTACT:
		if (ent->recipient.contact != NULL) {
			free(ent->recipient.contact->phone);
			free(ent->recipient.contact->email);
			free(ent->recipient.contact->system_display_name);
			free(ent->recipient.contact->system_phone_label);
			free(ent->recipient.contact->profile_given_name);
			free(ent->recipient.contact->profile_family_name);
			free(ent->recipient.contact->profile_joined_name);
			free(ent->recipient.contact);
		}
		break;
	case SBK_GROUP:
		if (ent->recipient.group != NULL) {
			free(ent->recipient.group->name);
			free(ent->recipient.group);
		}
		break;
	}

	free(ent->id.old);
	free(ent);
}

static void
sbk_free_recipient_tree(struct sbk_ctx *ctx)
{
	struct sbk_recipient_entry *ent;

	while ((ent = RB_ROOT(&ctx->recipients)) != NULL) {
		RB_REMOVE(sbk_recipient_tree, &ctx->recipients, ent);
		sbk_free_recipient_entry(ent);
	}
}

/* For database versions < SBK_DB_VERSION_RECIPIENT_IDS */
#define SBK_RECIPIENTS_QUERY_1						\
	"SELECT "							\
	"r.recipient_ids, "						\
	"NULL, "			/* phone */			\
	"NULL, "			/* email */			\
	"r.system_display_name, "					\
	"r.system_phone_label, "					\
	"r.signal_profile_name, "					\
	"NULL, "			/* profile_family_name */	\
	"NULL, "			/* profile_joined_name */	\
	"g.group_id, "							\
	"g.title "							\
	"FROM recipient_preferences AS r "				\
	"LEFT JOIN groups AS g "					\
	"ON r.recipient_ids = g.group_id"

/* For database versions < SBK_DB_VERSION_SPLIT_PROFILE_NAMES */
#define SBK_RECIPIENTS_QUERY_2						\
	"SELECT "							\
	"r._id, "							\
	"r.phone, "							\
	"r.email, "							\
	"r.system_display_name, "					\
	"r.system_phone_label, "					\
	"r.signal_profile_name, "					\
	"NULL, "			/* profile_family_name */	\
	"NULL, "			/* profile_joined_name */	\
	"g.group_id, "							\
	"g.title "							\
	"FROM recipient AS r "						\
	"LEFT JOIN groups AS g "					\
	"ON r._id = g.recipient_id"

/* For database versions >= SBK_DB_VERSION_SPLIT_PROFILE_NAMES */
#define SBK_RECIPIENTS_QUERY_3						\
	"SELECT "							\
	"r._id, "							\
	"r.phone, "							\
	"r.email, "							\
	"r.system_display_name, "					\
	"r.system_phone_label, "					\
	"r.signal_profile_name, "					\
	"r.profile_family_name, "					\
	"r.profile_joined_name, "					\
	"g.group_id, "							\
	"g.title "							\
	"FROM recipient AS r "						\
	"LEFT JOIN groups AS g "					\
	"ON r._id = g.recipient_id"

#define SBK_RECIPIENTS_COLUMN__ID			0
#define SBK_RECIPIENTS_COLUMN_PHONE			1
#define SBK_RECIPIENTS_COLUMN_EMAIL			2
#define SBK_RECIPIENTS_COLUMN_SYSTEM_DISPLAY_NAME	3
#define SBK_RECIPIENTS_COLUMN_SYSTEM_PHONE_LABEL	4
#define SBK_RECIPIENTS_COLUMN_SIGNAL_PROFILE_NAME	5
#define SBK_RECIPIENTS_COLUMN_PROFILE_FAMILY_NAME	6
#define SBK_RECIPIENTS_COLUMN_PROFILE_JOINED_NAME	7
#define SBK_RECIPIENTS_COLUMN_GROUP_ID			8
#define SBK_RECIPIENTS_COLUMN_TITLE			9

static struct sbk_recipient_entry *
sbk_get_recipient_entry(struct sbk_ctx *ctx, sqlite3_stmt *stm)
{
	struct sbk_recipient_entry	*ent;
	struct sbk_contact		*con;
	struct sbk_group		*grp;

	if ((ent = calloc(1, sizeof *ent)) == NULL) {
		sbk_error_set(ctx, NULL);
		return NULL;
	}

	if (sbk_get_recipient_id_from_column(ctx, &ent->id, stm,
	    SBK_RECIPIENTS_COLUMN__ID) == -1)
		goto error;

	if (sqlite3_column_type(stm, SBK_RECIPIENTS_COLUMN_GROUP_ID) ==
	    SQLITE_NULL)
		ent->recipient.type = SBK_CONTACT;
	else
		ent->recipient.type = SBK_GROUP;

	switch (ent->recipient.type) {
	case SBK_CONTACT:
		con = ent->recipient.contact = calloc(1, sizeof *con);
		if (con == NULL) {
			sbk_error_set(ctx, NULL);
			goto error;
		}

		if (ctx->db_version < SBK_DB_VERSION_RECIPIENT_IDS) {
			if (strchr(ent->id.old, '@') != NULL) {
				con->email = strdup(ent->id.old);
				if (con->email == NULL) {
					sbk_error_set(ctx, NULL);
					goto error;
				}
			} else {
				con->phone = strdup(ent->id.old);
				if (con->phone == NULL) {
					sbk_error_set(ctx, NULL);
					goto error;
				}
			}
		} else {
			if (sbk_sqlite_column_text_copy(ctx, &con->phone,
			    stm, SBK_RECIPIENTS_COLUMN_PHONE) == -1)
				goto error;

			if (sbk_sqlite_column_text_copy(ctx, &con->email,
			    stm, SBK_RECIPIENTS_COLUMN_EMAIL) == -1)
				goto error;
		}

		if (sbk_sqlite_column_text_copy(ctx, &con->system_display_name,
		    stm, SBK_RECIPIENTS_COLUMN_SYSTEM_DISPLAY_NAME) == -1)
			goto error;

		if (sbk_sqlite_column_text_copy(ctx, &con->system_phone_label,
		    stm, SBK_RECIPIENTS_COLUMN_SYSTEM_PHONE_LABEL) == -1)
			goto error;

		if (sbk_sqlite_column_text_copy(ctx, &con->profile_given_name,
		    stm, SBK_RECIPIENTS_COLUMN_SIGNAL_PROFILE_NAME) == -1)
			goto error;

		if (sbk_sqlite_column_text_copy(ctx, &con->profile_family_name,
		    stm, SBK_RECIPIENTS_COLUMN_PROFILE_FAMILY_NAME) == -1)
			goto error;

		if (sbk_sqlite_column_text_copy(ctx, &con->profile_joined_name,
		    stm, SBK_RECIPIENTS_COLUMN_PROFILE_JOINED_NAME) == -1)
			goto error;

		break;

	case SBK_GROUP:
		grp = ent->recipient.group = calloc(1, sizeof *grp);
		if (grp == NULL) {
			sbk_error_set(ctx, NULL);
			goto error;
		}

		if (sbk_sqlite_column_text_copy(ctx, &grp->name,
		    stm, SBK_RECIPIENTS_COLUMN_TITLE) == -1)
			goto error;

		break;
	}

	return ent;

error:
	sbk_free_recipient_entry(ent);
	return NULL;
}

static int
sbk_build_recipient_tree(struct sbk_ctx *ctx)
{
	struct sbk_recipient_entry	*ent;
	sqlite3_stmt			*stm;
	const char			*query;
	int				 ret;

	if (!RB_EMPTY(&ctx->recipients))
		return 0;

	if (sbk_create_database(ctx) == -1)
		return -1;

	if (ctx->db_version < SBK_DB_VERSION_RECIPIENT_IDS)
		query = SBK_RECIPIENTS_QUERY_1;
	else if (ctx->db_version < SBK_DB_VERSION_SPLIT_PROFILE_NAMES)
		query = SBK_RECIPIENTS_QUERY_2;
	else
		query = SBK_RECIPIENTS_QUERY_3;

	if (sbk_sqlite_prepare(ctx, &stm, query) == -1)
		return -1;

	while ((ret = sbk_sqlite_step(ctx, stm)) == SQLITE_ROW) {
		if ((ent = sbk_get_recipient_entry(ctx, stm)) == NULL)
			goto error;
		RB_INSERT(sbk_recipient_tree, &ctx->recipients, ent);
	}

	if (ret != SQLITE_DONE)
		goto error;

	sqlite3_finalize(stm);
	return 0;

error:
	sbk_free_recipient_tree(ctx);
	sqlite3_finalize(stm);
	return -1;
}

static struct sbk_recipient *
sbk_get_recipient(struct sbk_ctx *ctx, struct sbk_recipient_id *id)
{
	struct sbk_recipient_entry find, *result;

	if (sbk_build_recipient_tree(ctx) == -1)
		return NULL;

	find.id = *id;
	result = RB_FIND(sbk_recipient_tree, &ctx->recipients, &find);

	if (result == NULL) {
		sbk_error_setx(ctx, "Cannot find recipient");
		return NULL;
	}

	return &result->recipient;
}

static struct sbk_recipient *
sbk_get_recipient_from_column(struct sbk_ctx *ctx, sqlite3_stmt *stm,
    int idx)
{
	struct sbk_recipient	*rcp;
	struct sbk_recipient_id	 id;

	if (sbk_get_recipient_id_from_column(ctx, &id, stm, idx) == -1)
		return NULL;

	rcp = sbk_get_recipient(ctx, &id);
	free(id.old);
	return rcp;
}

const char *
sbk_get_recipient_display_name(const struct sbk_recipient *rcp)
{
	switch (rcp->type) {
	case SBK_CONTACT:
		if (rcp->contact->system_display_name != NULL)
			return rcp->contact->system_display_name;
		if (rcp->contact->profile_joined_name != NULL)
			return rcp->contact->profile_joined_name;
		if (rcp->contact->profile_given_name != NULL)
			return rcp->contact->profile_given_name;
		if (rcp->contact->phone != NULL)
			return rcp->contact->phone;
		if (rcp->contact->email != NULL)
			return rcp->contact->email;
		break;
	case SBK_GROUP:
		if (rcp->group->name != NULL)
			return rcp->group->name;
		break;
	}

	return "Unknown";
}

static void
sbk_free_attachment(struct sbk_attachment *att)
{
	if (att != NULL) {
		free(att->filename);
		free(att->content_type);
		free(att);
	}
}

void
sbk_free_attachment_list(struct sbk_attachment_list *lst)
{
	struct sbk_attachment *att;

	if (lst != NULL) {
		while ((att = TAILQ_FIRST(lst)) != NULL) {
			TAILQ_REMOVE(lst, att, entries);
			sbk_free_attachment(att);
		}
		free(lst);
	}
}

#define SBK_ATTACHMENTS_SELECT						\
	"SELECT "							\
	"file_name, "							\
	"ct, "								\
	"_id, "								\
	"unique_id, "							\
	"pending_push, "						\
	"data_size "							\
	"FROM part "

#define SBK_ATTACHMENTS_WHERE_THREAD					\
	"WHERE mid IN (SELECT _id FROM mms WHERE thread_id = ?) "

#define SBK_ATTACHMENTS_WHERE_MESSAGE					\
	"WHERE mid = ? "

#define SBK_ATTACHMENTS_ORDER						\
	"ORDER BY unique_id, _id"

#define SBK_ATTACHMENTS_QUERY_ALL					\
	SBK_ATTACHMENTS_SELECT						\
	SBK_ATTACHMENTS_ORDER

#define SBK_ATTACHMENTS_QUERY_THREAD					\
	SBK_ATTACHMENTS_SELECT						\
	SBK_ATTACHMENTS_WHERE_THREAD					\
	SBK_ATTACHMENTS_ORDER

#define SBK_ATTACHMENTS_QUERY_MESSAGE					\
	SBK_ATTACHMENTS_SELECT						\
	SBK_ATTACHMENTS_WHERE_MESSAGE					\
	SBK_ATTACHMENTS_ORDER

#define SBK_ATTACHMENTS_COLUMN_FILE_NAME	0
#define SBK_ATTACHMENTS_COLUMN_CT		1
#define SBK_ATTACHMENTS_COLUMN__ID		2
#define SBK_ATTACHMENTS_COLUMN_UNIQUE_ID	3
#define SBK_ATTACHMENTS_COLUMN_PENDING_PUSH	4
#define SBK_ATTACHMENTS_COLUMN_DATA_SIZE	5

static struct sbk_attachment *
sbk_get_attachment(struct sbk_ctx *ctx, sqlite3_stmt *stm)
{
	struct sbk_attachment *att;

	if ((att = malloc(sizeof *att)) == NULL) {
		sbk_error_set(ctx, NULL);
		return NULL;
	}

	att->filename = NULL;
	att->content_type = NULL;
	att->file = NULL;

	if (sbk_sqlite_column_text_copy(ctx, &att->filename, stm,
	    SBK_ATTACHMENTS_COLUMN_FILE_NAME) == -1)
		goto error;

	if (sbk_sqlite_column_text_copy(ctx, &att->content_type, stm,
	    SBK_ATTACHMENTS_COLUMN_CT) == -1)
		goto error;

	att->rowid = sqlite3_column_int64(stm, SBK_ATTACHMENTS_COLUMN__ID);
	att->attachmentid = sqlite3_column_int64(stm,
	    SBK_ATTACHMENTS_COLUMN_UNIQUE_ID);
	att->status = sqlite3_column_int(stm,
	    SBK_ATTACHMENTS_COLUMN_PENDING_PUSH);
	att->size = sqlite3_column_int64(stm,
	    SBK_ATTACHMENTS_COLUMN_DATA_SIZE);
	att->file = sbk_get_attachment_file(ctx, att->rowid,
	    att->attachmentid);

	if (att->file == NULL)
		sbk_warnx(ctx, "Attachment %" PRId64 "-%" PRId64 " not "
		    "available in backup", att->rowid, att->attachmentid);
	else if (att->size != att->file->len)
		sbk_warnx(ctx, "Attachment %" PRId64 "-%" PRId64 " has "
		    "inconsistent size", att->rowid, att->attachmentid);

	return att;

error:
	sbk_free_attachment(att);
	return NULL;
}

static struct sbk_attachment_list *
sbk_get_attachments(struct sbk_ctx *ctx, sqlite3_stmt *stm)
{
	struct sbk_attachment_list	*lst;
	struct sbk_attachment		*att;
	int				 ret;

	if ((lst = malloc(sizeof *lst)) == NULL) {
		sbk_error_set(ctx, NULL);
		goto error;
	}

	TAILQ_INIT(lst);

	while ((ret = sbk_sqlite_step(ctx, stm)) == SQLITE_ROW) {
		if ((att = sbk_get_attachment(ctx, stm)) == NULL)
			goto error;
		TAILQ_INSERT_TAIL(lst, att, entries);
	}

	if (ret != SQLITE_DONE)
		goto error;

	sqlite3_finalize(stm);
	return lst;

error:
	sbk_free_attachment_list(lst);
	sqlite3_finalize(stm);
	return NULL;
}

struct sbk_attachment_list *
sbk_get_all_attachments(struct sbk_ctx *ctx)
{
	sqlite3_stmt *stm;

	if (sbk_create_database(ctx) == -1)
		return NULL;

	if (sbk_sqlite_prepare(ctx, &stm, SBK_ATTACHMENTS_QUERY_ALL) == -1)
		return NULL;

	return sbk_get_attachments(ctx, stm);
}

struct sbk_attachment_list *
sbk_get_attachments_for_thread(struct sbk_ctx *ctx, int thread_id)
{
	sqlite3_stmt *stm;

	if (sbk_create_database(ctx) == -1)
		return NULL;

	if (sbk_sqlite_prepare(ctx, &stm, SBK_ATTACHMENTS_QUERY_THREAD) == -1)
		return NULL;

	if (sbk_sqlite_bind_int(ctx, stm, 1, thread_id) == -1) {
		sqlite3_finalize(stm);
		return NULL;
	}

	return sbk_get_attachments(ctx, stm);
}

static int
sbk_get_attachments_for_message(struct sbk_ctx *ctx, struct sbk_message *msg)
{
	sqlite3_stmt *stm;

	if (msg->id.type != SBK_MESSAGE_MMS)
		return 0;

	if (sbk_sqlite_prepare(ctx, &stm, SBK_ATTACHMENTS_QUERY_MESSAGE) == -1)
		return -1;

	if (sbk_sqlite_bind_int(ctx, stm, 1, msg->id.rowid) == -1) {
		sqlite3_finalize(stm);
		return -1;
	}

	if ((msg->attachments = sbk_get_attachments(ctx, stm)) == NULL)
		return -1;

	return 0;
}

static void
sbk_free_mention_list(struct sbk_mention_list *lst)
{
	struct sbk_mention *mnt;

	if (lst != NULL) {
		while ((mnt = SIMPLEQ_FIRST(lst)) != NULL) {
			SIMPLEQ_REMOVE_HEAD(lst, entries);
			free(mnt);
		}
		free(lst);
	}
}

#define SBK_MENTIONS_QUERY						\
	"SELECT "							\
	"recipient_id "							\
	"FROM mention "							\
	"WHERE message_id = ? "						\
	"ORDER BY range_start"

static struct sbk_mention *
sbk_get_mention(struct sbk_ctx *ctx, sqlite3_stmt *stm)
{
	struct sbk_mention *mnt;

	if ((mnt = malloc(sizeof *mnt)) == NULL) {
		sbk_error_set(ctx, NULL);
		return NULL;
	}

	mnt->recipient = sbk_get_recipient_from_column(ctx, stm, 0);
	if (mnt->recipient == NULL) {
		free(mnt);
		return NULL;
	}

	return mnt;
}

static int
sbk_get_mentions(struct sbk_ctx *ctx, struct sbk_message *msg)
{
	struct sbk_mention	*mnt;
	sqlite3_stmt		*stm;
	int			 ret;

	msg->mentions = NULL;

	if (msg->id.type != SBK_MESSAGE_MMS ||
	    ctx->db_version < SBK_DB_VERSION_MENTIONS)
		return 0;

	if (sbk_sqlite_prepare(ctx, &stm, SBK_MENTIONS_QUERY) == -1)
		return -1;

	if (sbk_sqlite_bind_int(ctx, stm, 1, msg->id.rowid) == -1)
		goto error;

	if ((msg->mentions = malloc(sizeof *msg->mentions)) == NULL) {
		sbk_error_set(ctx, NULL);
		goto error;
	}

	SIMPLEQ_INIT(msg->mentions);

	while ((ret = sbk_sqlite_step(ctx, stm)) == SQLITE_ROW) {
		if ((mnt = sbk_get_mention(ctx, stm)) == NULL)
			goto error;
		SIMPLEQ_INSERT_TAIL(msg->mentions, mnt, entries);
	}

	if (ret != SQLITE_DONE)
		goto error;

	sqlite3_finalize(stm);
	return 0;

error:
	sbk_free_mention_list(msg->mentions);
	msg->mentions = NULL;
	sqlite3_finalize(stm);
	return -1;
}

static int
sbk_insert_mentions(struct sbk_ctx *ctx, struct sbk_message *msg)
{
	struct sbk_mention *mnt;
	char		*newtext, *newtextpos, *placeholderpos, *textpos;
	const char	*name;
	size_t		 copylen, newtextlen, placeholderlen, prefixlen;

	if (sbk_get_mentions(ctx, msg) == -1)
		return -1;

	if (msg->mentions == NULL || SIMPLEQ_EMPTY(msg->mentions))
		return 0;

	newtext = NULL;
	placeholderlen = strlen(SBK_MENTION_PLACEHOLDER);
	prefixlen = strlen(SBK_MENTION_PREFIX);

	/* Calculate length of new text */
	newtextlen = strlen(msg->text);
	SIMPLEQ_FOREACH(mnt, msg->mentions, entries) {
		if (newtextlen < placeholderlen)
			goto error;
		name = sbk_get_recipient_display_name(mnt->recipient);
		/* Subtract placeholder, add mention */
		newtextlen = newtextlen - placeholderlen + prefixlen +
		    strlen(name);
	}

	if ((newtext = malloc(newtextlen + 1)) == NULL) {
		sbk_error_set(ctx, NULL);
		return -1;
	}

	textpos = msg->text;
	newtextpos = newtext;

	/* Write new text, replacing placeholders with mentions */
	SIMPLEQ_FOREACH(mnt, msg->mentions, entries) {
		placeholderpos = strstr(textpos, SBK_MENTION_PLACEHOLDER);
		if (placeholderpos == NULL)
			goto error;

		copylen = placeholderpos - textpos;
		memcpy(newtextpos, textpos, copylen);
		textpos += copylen + placeholderlen;
		newtextpos += copylen;

		memcpy(newtextpos, SBK_MENTION_PREFIX, prefixlen);
		newtextpos += prefixlen;

		name = sbk_get_recipient_display_name(mnt->recipient);
		copylen = strlen(name);
		memcpy(newtextpos, name, copylen);
		newtextpos += copylen;
	}

	/* Sanity check: there should be no placeholders left */
	if (strstr(textpos, SBK_MENTION_PLACEHOLDER) != NULL)
		goto error;

	copylen = strlen(textpos);
	memcpy(newtextpos, textpos, copylen);
	newtextpos += copylen;
	*newtextpos = '\0';

	free(msg->text);
	msg->text = newtext;

	return 0;

error:
	sbk_warnx(ctx, "Invalid mention in message %d-%d", msg->id.type,
	    msg->id.rowid);
	free(newtext);
	return 0;
}

int
sbk_is_outgoing_message(const struct sbk_message *msg)
{
	switch (msg->type & SBK_BASE_TYPE_MASK) {
	case SBK_OUTGOING_AUDIO_CALL_TYPE:
	case SBK_BASE_OUTBOX_TYPE:
	case SBK_BASE_SENDING_TYPE:
	case SBK_BASE_SENT_TYPE:
	case SBK_BASE_SENT_FAILED_TYPE:
	case SBK_BASE_PENDING_SECURE_SMS_FALLBACK:
	case SBK_BASE_PENDING_INSECURE_SMS_FALLBACK:
	case SBK_OUTGOING_VIDEO_CALL_TYPE:
		return 1;
	default:
		return 0;
	}
}

static void
sbk_free_reaction(struct sbk_reaction *rct)
{
	if (rct != NULL) {
		free(rct->emoji);
		free(rct);
	}
}

static void
sbk_free_reaction_list(struct sbk_reaction_list *lst)
{
	struct sbk_reaction *rct;

	if (lst != NULL) {
		while ((rct = SIMPLEQ_FIRST(lst)) != NULL) {
			SIMPLEQ_REMOVE_HEAD(lst, entries);
			sbk_free_reaction(rct);
		}
		free(lst);
	}
}

/*
 * For database versions < SBK_DB_VERSION_REACTION_REFACTOR
 */

static Signal__ReactionList *
sbk_unpack_reaction_list_message(struct sbk_ctx *ctx, const void *buf,
    size_t len)
{
	Signal__ReactionList *msg;

	if ((msg = signal__reaction_list__unpack(NULL, len, buf)) == NULL)
		sbk_error_setx(ctx, "Cannot unpack reaction list");

	return msg;
}

static void
sbk_free_reaction_list_message(Signal__ReactionList *msg)
{
	if (msg != NULL)
		signal__reaction_list__free_unpacked(msg, NULL);
}

static int
sbk_get_reactions_from_column(struct sbk_ctx *ctx,
    struct sbk_reaction_list **lst, sqlite3_stmt *stm, int idx)
{
	struct sbk_reaction	*rct;
	struct sbk_recipient_id	 id;
	Signal__ReactionList	*msg;
	const void		*blob;
	size_t			 i;
	int			 len;

	*lst = NULL;

	if (sqlite3_column_type(stm, idx) != SQLITE_BLOB) {
		/* No reactions */
		return 0;
	}

	if ((blob = sqlite3_column_blob(stm, idx)) == NULL) {
		sbk_error_sqlite_set(ctx, "Cannot get reactions column");
		return -1;
	}

	if ((len = sqlite3_column_bytes(stm, idx)) < 0) {
		sbk_error_sqlite_set(ctx, "Cannot get reactions size");
		return -1;
	}

	if ((msg = sbk_unpack_reaction_list_message(ctx, blob, len)) == NULL)
		return -1;

	if ((*lst = malloc(sizeof **lst)) == NULL) {
		sbk_error_set(ctx, NULL);
		goto error1;
	}

	SIMPLEQ_INIT(*lst);

	for (i = 0; i < msg->n_reactions; i++) {
		if ((rct = malloc(sizeof *rct)) == NULL) {
			sbk_error_set(ctx, NULL);
			goto error1;
		}

		id.new = msg->reactions[i]->author;
		id.old = NULL;

		if ((rct->recipient = sbk_get_recipient(ctx, &id)) == NULL)
			goto error2;

		if ((rct->emoji = strdup(msg->reactions[i]->emoji)) == NULL) {
			sbk_error_set(ctx, NULL);
			goto error2;
		}

		rct->time_sent = msg->reactions[i]->senttime;
		rct->time_recv = msg->reactions[i]->receivedtime;
		SIMPLEQ_INSERT_TAIL(*lst, rct, entries);
	}

	sbk_free_reaction_list_message(msg);
	return 0;

error2:
	free(rct);

error1:
	sbk_free_reaction_list(*lst);
	sbk_free_reaction_list_message(msg);
	*lst = NULL;
	return -1;
}

/*
 * For database versions >= SBK_DB_VERSION_REACTION_REFACTOR
 */

#define SBK_REACTIONS_QUERY						\
	"SELECT "							\
	"author_id, "							\
	"date_sent, "							\
	"date_received, "						\
	"emoji "							\
	"FROM reaction "						\
	"WHERE message_id = ? AND is_mms = ? "				\
	"ORDER BY date_sent"

#define SBK_REACTIONS_COLUMN_AUTHOR_ID		0
#define SBK_REACTIONS_COLUMN_DATE_SENT		1
#define SBK_REACTIONS_COLUMN_DATE_RECEIVED	2
#define SBK_REACTIONS_COLUMN_EMOJI		3

static struct sbk_reaction *
sbk_get_reaction(struct sbk_ctx *ctx, sqlite3_stmt *stm)
{
	struct sbk_reaction *rct;

	if ((rct = calloc(1, sizeof *rct)) == NULL) {
		sbk_error_set(ctx, NULL);
		return NULL;
	}

	rct->recipient = sbk_get_recipient_from_column(ctx, stm,
	    SBK_REACTIONS_COLUMN_AUTHOR_ID);
	if (rct->recipient == NULL)
		goto error;

	if (sbk_sqlite_column_text_copy(ctx, &rct->emoji, stm,
	    SBK_REACTIONS_COLUMN_EMOJI) == -1)
		goto error;

	rct->time_sent = sqlite3_column_int64(stm,
	    SBK_REACTIONS_COLUMN_DATE_SENT);
	rct->time_recv = sqlite3_column_int64(stm,
	    SBK_REACTIONS_COLUMN_DATE_RECEIVED);

	return rct;

error:
	sbk_free_reaction(rct);
	return NULL;
}

static struct sbk_reaction_list *
sbk_get_reactions(struct sbk_ctx *ctx, sqlite3_stmt *stm)
{
	struct sbk_reaction_list	*lst;
	struct sbk_reaction		*rct;
	int				 ret;

	if ((lst = malloc(sizeof *lst)) == NULL) {
		sbk_error_set(ctx, NULL);
		goto error;
	}

	SIMPLEQ_INIT(lst);

	while ((ret = sbk_sqlite_step(ctx, stm)) == SQLITE_ROW) {
		if ((rct = sbk_get_reaction(ctx, stm)) == NULL)
			goto error;
		SIMPLEQ_INSERT_TAIL(lst, rct, entries);
	}

	if (ret != SQLITE_DONE)
		goto error;

	sqlite3_finalize(stm);
	return lst;

error:
	sbk_free_reaction_list(lst);
	sqlite3_finalize(stm);
	return NULL;
}

static int
sbk_get_reactions_from_table(struct sbk_ctx *ctx, struct sbk_message *msg)
{
	sqlite3_stmt *stm;

	if (sbk_sqlite_prepare(ctx, &stm, SBK_REACTIONS_QUERY) == -1)
		return -1;

	if (sbk_sqlite_bind_int(ctx, stm, 1, msg->id.rowid) == -1) {
		sqlite3_finalize(stm);
		return -1;
	}

	if (sbk_sqlite_bind_int(ctx, stm, 2, msg->id.type == SBK_MESSAGE_MMS)
	    == -1) {
		sqlite3_finalize(stm);
		return -1;
	}

	if ((msg->reactions = sbk_get_reactions(ctx, stm)) == NULL)
		return -1;

	return 0;
}

static int
sbk_get_body(struct sbk_message *msg)
{
	const char *fmt;

	fmt = NULL;

	if (msg->type & SBK_ENCRYPTION_REMOTE_FAILED_BIT)
		fmt = "Bad encrypted message";
	else if (msg->type & SBK_ENCRYPTION_REMOTE_NO_SESSION_BIT)
		fmt = "Message encrypted for non-existing session";
	else if (msg->type & SBK_ENCRYPTION_REMOTE_DUPLICATE_BIT)
		fmt = "Duplicate message";
	else if ((msg->type & SBK_ENCRYPTION_REMOTE_LEGACY_BIT) ||
	    (msg->type & SBK_ENCRYPTION_REMOTE_BIT))
		fmt = "Encrypted message sent from an older version of Signal "
		    "that is no longer supported";
	else if (msg->type & SBK_GROUP_UPDATE_BIT) {
		if (sbk_is_outgoing_message(msg))
			fmt = "You updated the group";
		else
			fmt = "%s updated the group";
	} else if (msg->type & SBK_GROUP_QUIT_BIT) {
		if (sbk_is_outgoing_message(msg))
			fmt = "You have left the group";
		else
			fmt = "%s has left the group";
	} else if (msg->type & SBK_END_SESSION_BIT) {
		if (sbk_is_outgoing_message(msg))
			fmt = "You reset the secure session";
		else
			fmt = "%s reset the secure session";
	} else if (msg->type & SBK_KEY_EXCHANGE_IDENTITY_VERIFIED_BIT) {
		if (sbk_is_outgoing_message(msg))
			fmt = "You marked your safety number with %s verified";
		else
			fmt = "You marked your safety number with %s verified "
			    "from another device";
	} else if (msg->type & SBK_KEY_EXCHANGE_IDENTITY_DEFAULT_BIT) {
		if (sbk_is_outgoing_message(msg))
			fmt = "You marked your safety number with %s "
			    "unverified";
		else
			fmt = "You marked your safety number with %s "
			    "unverified from another device";
	} else if (msg->type & SBK_KEY_EXCHANGE_CORRUPTED_BIT)
		fmt = "Corrupt key exchange message";
	else if (msg->type & SBK_KEY_EXCHANGE_INVALID_VERSION_BIT)
		fmt = "Key exchange message for invalid protocol version";
	else if (msg->type & SBK_KEY_EXCHANGE_BUNDLE_BIT)
		fmt = "Message with new safety number";
	else if (msg->type & SBK_KEY_EXCHANGE_IDENTITY_UPDATE_BIT)
		fmt = "Your safety number with %s has changed";
	else if (msg->type & SBK_KEY_EXCHANGE_BIT)
		fmt = "Key exchange message";
	else
		switch (msg->type & SBK_BASE_TYPE_MASK) {
		case SBK_INCOMING_AUDIO_CALL_TYPE:
		case SBK_INCOMING_VIDEO_CALL_TYPE:
			fmt = "%s called you";
			break;
		case SBK_OUTGOING_AUDIO_CALL_TYPE:
		case SBK_OUTGOING_VIDEO_CALL_TYPE:
			fmt = "Called %s";
			break;
		case SBK_MISSED_AUDIO_CALL_TYPE:
			fmt = "Missed audio call from %s";
			break;
		case SBK_JOINED_TYPE:
			fmt = "%s is on Signal";
			break;
		case SBK_UNSUPPORTED_MESSAGE_TYPE:
			fmt = "Unsupported message sent from a newer version "
			    "of Signal";
			break;
		case SBK_INVALID_MESSAGE_TYPE:
			fmt = "Invalid message";
			break;
		case SBK_PROFILE_CHANGE_TYPE:
			fmt = "%s changed their profile";
			break;
		case SBK_MISSED_VIDEO_CALL_TYPE:
			fmt = "Missed video call from %s";
			break;
		case SBK_GV1_MIGRATION_TYPE:
			fmt = "This group was updated to a new group";
			break;
		}

	if (fmt == NULL)
		return 0;

	free(msg->text);

	if (asprintf(&msg->text, fmt,
	    sbk_get_recipient_display_name(msg->recipient)) == -1) {
		msg->text = NULL;
		return -1;
	}

	return 0;
}

static void
sbk_remove_attachment(struct sbk_message *msg, struct sbk_attachment *att)
{
	TAILQ_REMOVE(msg->attachments, att, entries);
	sbk_free_attachment(att);
	if (TAILQ_EMPTY(msg->attachments)) {
		sbk_free_attachment_list(msg->attachments);
		msg->attachments = NULL;
	}
}

static int
sbk_get_long_message(struct sbk_ctx *ctx, struct sbk_message *msg)
{
	struct sbk_attachment	*att;
	char			*longmsg;
	int			 found;

	/* Look for a long-message attachment */
	found = 0;
	TAILQ_FOREACH(att, msg->attachments, entries)
		if (att->content_type != NULL &&
		    strcmp(att->content_type, SBK_LONG_TEXT_TYPE) == 0) {
			found = 1;
			break;
		}

	if (!found)
		return 0;

	if (att->file == NULL) {
		sbk_warnx(ctx, "Long-message attachment for message %d-%d not "
		    "available in backup", msg->id.type, msg->id.rowid);
		return 0;
	}

	if ((longmsg = sbk_get_file_data_as_string(ctx, att->file)) == NULL)
		return -1;

	free(msg->text);
	msg->text = longmsg;

	/* Do not expose the long-message attachment */
	sbk_remove_attachment(msg, att);

	return 0;
}

static void
sbk_free_message(struct sbk_message *msg)
{
	if (msg != NULL) {
		free(msg->text);
		sbk_free_attachment_list(msg->attachments);
		sbk_free_mention_list(msg->mentions);
		sbk_free_reaction_list(msg->reactions);
		free(msg);
	}
}

void
sbk_free_message_list(struct sbk_message_list *lst)
{
	struct sbk_message *msg;

	if (lst != NULL) {
		while ((msg = SIMPLEQ_FIRST(lst)) != NULL) {
			SIMPLEQ_REMOVE_HEAD(lst, entries);
			sbk_free_message(msg);
		}
		free(lst);
	}
}

/* For database versions < SBK_DB_VERSION_REACTIONS */
#define SBK_MESSAGES_SELECT_SMS_1					\
	"SELECT "							\
	"0, "								\
	"_id, "								\
	"address, "							\
	"body, "							\
	"date_sent, "							\
	"date AS date_received, "					\
	"type, "							\
	"thread_id, "							\
	"NULL, "				/* reactions */			\
	"NULL "				/* quote_id */			\
	"FROM sms "

/* For database versions >= SBK_DB_VERSION_REACTIONS */
#define SBK_MESSAGES_SELECT_SMS_2					\
	"SELECT "							\
	"0, "								\
	"_id, "								\
	"address, "							\
	"body, "							\
	"date_sent, "							\
	"date AS date_received, "					\
	"type, "							\
	"thread_id, "							\
	"reactions, "							\
	"NULL "				/* quote_id */			\
	"FROM sms "

/* For database versions < SBK_DB_VERSION_REACTIONS */
#define SBK_MESSAGES_SELECT_MMS_1					\
	"SELECT "							\
	"1, "								\
	"_id, "								\
	"address, "							\
	"body, "							\
	"date, "			/* sms.date_sent */		\
	"date_received, "						\
	"msg_box, "			/* sms.type */			\
	"thread_id, "							\
	"NULL, "				/* reactions */			\
	"NULL "				/* quote_id */			\
	"FROM mms "

/* For database versions >= SBK_DB_VERSION_REACTIONS */
#define SBK_MESSAGES_SELECT_MMS_2					\
	"SELECT "							\
	"1, "								\
	"_id, "								\
	"address, "							\
	"body, "							\
	"date, "			/* sms.date_sent */		\
	"date_received, "						\
	"msg_box, "			/* type */			\
	"thread_id, "							\
	"reactions, "							\
	"quote_id "							\
	"FROM mms "

#define SBK_MESSAGES_WHERE_THREAD					\
	"WHERE thread_id = ? "

#define SBK_MESSAGES_ORDER						\
	"ORDER BY date_received"

/* For database versions < SBK_DB_VERSION_REACTIONS */
#define SBK_MESSAGES_QUERY_ALL_1					\
	SBK_MESSAGES_SELECT_SMS_1					\
	"UNION ALL "							\
	SBK_MESSAGES_SELECT_MMS_1					\
	SBK_MESSAGES_ORDER

/* For database versions >= SBK_DB_VERSION_REACTIONS */
#define SBK_MESSAGES_QUERY_ALL_2					\
	SBK_MESSAGES_SELECT_SMS_2					\
	"UNION ALL "							\
	SBK_MESSAGES_SELECT_MMS_2					\
	SBK_MESSAGES_ORDER

/* For database versions < SBK_DB_VERSION_REACTIONS */
#define SBK_MESSAGES_QUERY_THREAD_1					\
	SBK_MESSAGES_SELECT_SMS_1					\
	SBK_MESSAGES_WHERE_THREAD					\
	"UNION ALL "							\
	SBK_MESSAGES_SELECT_MMS_1					\
	SBK_MESSAGES_WHERE_THREAD					\
	SBK_MESSAGES_ORDER

/* For database versions >= SBK_DB_VERSION_REACTIONS */
#define SBK_MESSAGES_QUERY_THREAD_2					\
	SBK_MESSAGES_SELECT_SMS_2					\
	SBK_MESSAGES_WHERE_THREAD					\
	"UNION ALL "							\
	SBK_MESSAGES_SELECT_MMS_2					\
	SBK_MESSAGES_WHERE_THREAD					\
	SBK_MESSAGES_ORDER

#define SBK_MESSAGES_COLUMN_TABLE		0
#define SBK_MESSAGES_COLUMN__ID			1
#define SBK_MESSAGES_COLUMN_ADDRESS		2
#define SBK_MESSAGES_COLUMN_BODY		3
#define SBK_MESSAGES_COLUMN_DATE_SENT		4
#define SBK_MESSAGES_COLUMN_DATE_RECEIVED	5
#define SBK_MESSAGES_COLUMN_TYPE		6
#define SBK_MESSAGES_COLUMN_THREAD_ID		7
#define SBK_MESSAGES_COLUMN_REACTIONS		8
#define SBK_MESSAGES_COLUMN_QUOTE_ID		9

static struct sbk_message *
sbk_get_message(struct sbk_ctx *ctx, sqlite3_stmt *stm)
{
	struct sbk_message *msg;

	if ((msg = malloc(sizeof *msg)) == NULL) {
		sbk_error_set(ctx, NULL);
		return NULL;
	}

	msg->recipient = NULL;
	msg->text = NULL;
	msg->attachments = NULL;
	msg->mentions = NULL;
	msg->reactions = NULL;

	msg->id.type =
	    (sqlite3_column_int(stm, SBK_MESSAGES_COLUMN_TABLE) == 0) ?
	    SBK_MESSAGE_SMS : SBK_MESSAGE_MMS;
	msg->id.rowid = sqlite3_column_int(stm, SBK_MESSAGES_COLUMN__ID);

	msg->recipient = sbk_get_recipient_from_column(ctx, stm,
	    SBK_MESSAGES_COLUMN_ADDRESS);
	if (msg->recipient == NULL)
		goto error;

	if (sbk_sqlite_column_text_copy(ctx, &msg->text, stm,
	    SBK_MESSAGES_COLUMN_BODY) == -1)
		goto error;

	msg->time_sent = sqlite3_column_int64(stm,
	    SBK_MESSAGES_COLUMN_DATE_SENT);
	msg->time_recv = sqlite3_column_int64(stm,
	    SBK_MESSAGES_COLUMN_DATE_RECEIVED);
	msg->type = sqlite3_column_int(stm, SBK_MESSAGES_COLUMN_TYPE);
	msg->thread = sqlite3_column_int(stm, SBK_MESSAGES_COLUMN_THREAD_ID);

	msg->quote_id = sqlite3_column_int64(stm,
	    SBK_MESSAGES_COLUMN_QUOTE_ID);

	if (sbk_get_body(msg) == -1)
		goto error;

	if (msg->id.type == SBK_MESSAGE_MMS) {
		if (sbk_get_attachments_for_message(ctx, msg) == -1)
			goto error;

		if (sbk_get_long_message(ctx, msg) == -1)
			goto error;

		if (sbk_insert_mentions(ctx, msg) == -1)
			goto error;
	}

	if (ctx->db_version < SBK_DB_VERSION_REACTION_REFACTOR) {
		if (sbk_get_reactions_from_column(ctx, &msg->reactions, stm,
		    SBK_MESSAGES_COLUMN_REACTIONS) == -1)
			goto error;
	} else {
		if (sbk_get_reactions_from_table(ctx, msg) == -1)
			goto error;
	}

	return msg;

error:
	sbk_free_message(msg);
	return NULL;
}

static struct sbk_message_list *
sbk_get_messages(struct sbk_ctx *ctx, sqlite3_stmt *stm)
{
	struct sbk_message_list	*lst;
	struct sbk_message	*msg;
	int			 ret;

	if ((lst = malloc(sizeof *lst)) == NULL) {
		sbk_error_set(ctx, NULL);
		goto error;
	}

	SIMPLEQ_INIT(lst);

	while ((ret = sbk_sqlite_step(ctx, stm)) == SQLITE_ROW) {
		if ((msg = sbk_get_message(ctx, stm)) == NULL)
			goto error;
		SIMPLEQ_INSERT_TAIL(lst, msg, entries);
	}

	if (ret != SQLITE_DONE)
		goto error;

	sqlite3_finalize(stm);
	return lst;

error:
	sbk_free_message_list(lst);
	sqlite3_finalize(stm);
	return NULL;
}

struct sbk_message_list *
sbk_get_all_messages(struct sbk_ctx *ctx)
{
	sqlite3_stmt	*stm;
	const char	*query;

	if (sbk_create_database(ctx) == -1)
		return NULL;

	if (ctx->db_version < SBK_DB_VERSION_REACTIONS)
		query = SBK_MESSAGES_QUERY_ALL_1;
	else
		query = SBK_MESSAGES_QUERY_ALL_2;

	if (sbk_sqlite_prepare(ctx, &stm, query) == -1)
		return NULL;

	return sbk_get_messages(ctx, stm);
}

struct sbk_message_list *
sbk_get_messages_for_thread(struct sbk_ctx *ctx, int thread_id)
{
	sqlite3_stmt	*stm;
	const char	*query;

	if (sbk_create_database(ctx) == -1)
		return NULL;

	if (ctx->db_version < SBK_DB_VERSION_REACTIONS)
		query = SBK_MESSAGES_QUERY_THREAD_1;
	else
		query = SBK_MESSAGES_QUERY_THREAD_2;

	if (sbk_sqlite_prepare(ctx, &stm, query) == -1)
		return NULL;

	if (sbk_sqlite_bind_int(ctx, stm, 1, thread_id) == -1) {
		sqlite3_finalize(stm);
		return NULL;
	}

	if (sbk_sqlite_bind_int(ctx, stm, 2, thread_id) == -1) {
		sqlite3_finalize(stm);
		return NULL;
	}

	return sbk_get_messages(ctx, stm);
}

void
sbk_free_thread_list(struct sbk_thread_list *lst)
{
	struct sbk_thread *thd;

	if (lst != NULL) {
		while ((thd = SIMPLEQ_FIRST(lst)) != NULL) {
			SIMPLEQ_REMOVE_HEAD(lst, entries);
			free(thd);
		}
		free(lst);
	}
}

/* For database versions < SBK_DB_VERSION_THREAD_AUTOINCREMENT */
#define SBK_THREADS_QUERY_1						\
	"SELECT "							\
	"recipient_ids, "						\
	"_id, "								\
	"date, "							\
	"message_count "						\
	"FROM thread "							\
	"ORDER BY _id"

/* For database versions >= SBK_DB_VERSION_THREAD_AUTOINCREMENT */
#define SBK_THREADS_QUERY_2						\
	"SELECT "							\
	"thread_recipient_id, "						\
	"_id, "								\
	"date, "							\
	"message_count "						\
	"FROM thread "							\
	"ORDER BY _id"

#define SBK_THREADS_COLUMN_THREAD_RECIPIENT_ID	0
#define SBK_THREADS_COLUMN__ID			1
#define SBK_THREADS_COLUMN_DATE			2
#define SBK_THREADS_COLUMN_MESSAGE_COUNT	3

struct sbk_thread_list *
sbk_get_threads(struct sbk_ctx *ctx)
{
	struct sbk_thread_list	*lst;
	struct sbk_thread	*thd;
	sqlite3_stmt		*stm;
	const char		*query;
	int			 ret;

	if (sbk_create_database(ctx) == -1)
		return NULL;

	if ((lst = malloc(sizeof *lst)) == NULL) {
		sbk_error_set(ctx, NULL);
		return NULL;
	}

	SIMPLEQ_INIT(lst);

	if (ctx->db_version < SBK_DB_VERSION_THREAD_AUTOINCREMENT)
		query = SBK_THREADS_QUERY_1;
	else
		query = SBK_THREADS_QUERY_2;

	if (sbk_sqlite_prepare(ctx, &stm, query) == -1)
		goto error;

	while ((ret = sbk_sqlite_step(ctx, stm)) == SQLITE_ROW) {
		if ((thd = malloc(sizeof *thd)) == NULL) {
			sbk_error_set(ctx, NULL);
			goto error;
		}

		thd->recipient = sbk_get_recipient_from_column(ctx, stm,
		    SBK_THREADS_COLUMN_THREAD_RECIPIENT_ID);
		if (thd->recipient == NULL) {
			free(thd);
			goto error;
		}

		thd->id = sqlite3_column_int64(stm, SBK_THREADS_COLUMN__ID);
		thd->date = sqlite3_column_int64(stm, SBK_THREADS_COLUMN_DATE);
		thd->nmessages = sqlite3_column_int64(stm,
		    SBK_THREADS_COLUMN_MESSAGE_COUNT);
		SIMPLEQ_INSERT_TAIL(lst, thd, entries);
	}

	if (ret != SQLITE_DONE)
		goto error;

	sqlite3_finalize(stm);
	return lst;

error:
	sbk_free_thread_list(lst);
	sqlite3_finalize(stm);
	return NULL;
}

static int
sbk_compute_keys(struct sbk_ctx *ctx, const char *passphr,
    const unsigned char *salt, size_t saltlen)
{
	EVP_MD_CTX	*md_ctx;
	unsigned char	 key[SHA512_DIGEST_LENGTH];
	unsigned char	 deriv_key[SBK_DERIV_KEY_LEN];
	size_t		 passphrlen;
	int		 i, ret;

	if ((md_ctx = EVP_MD_CTX_new()) == NULL)
		goto error;

	passphrlen = strlen(passphr);

	/* The first round */
	if (!EVP_DigestInit_ex(md_ctx, EVP_sha512(), NULL))
		goto error;
	if (salt != NULL)
		if (!EVP_DigestUpdate(md_ctx, salt, saltlen))
			goto error;
	if (!EVP_DigestUpdate(md_ctx, passphr, passphrlen))
		goto error;
	if (!EVP_DigestUpdate(md_ctx, passphr, passphrlen))
		goto error;
	if (!EVP_DigestFinal_ex(md_ctx, key, NULL))
		goto error;

	/* The remaining rounds */
	for (i = 0; i < SBK_ROUNDS - 1; i++) {
		if (!EVP_DigestInit_ex(md_ctx, EVP_sha512(), NULL))
			goto error;
		if (!EVP_DigestUpdate(md_ctx, key, sizeof key))
			goto error;
		if (!EVP_DigestUpdate(md_ctx, passphr, passphrlen))
			goto error;
		if (!EVP_DigestFinal(md_ctx, key, NULL))
			goto error;
	}

	if (!HKDF(deriv_key, sizeof deriv_key, EVP_sha256(), key, SBK_KEY_LEN,
	    (const unsigned char *)"", 0, (const unsigned char *)SBK_HKDF_INFO,
	    strlen(SBK_HKDF_INFO)))
		goto error;

	memcpy(ctx->cipher_key, deriv_key, SBK_CIPHER_KEY_LEN);
	memcpy(ctx->mac_key, deriv_key + SBK_CIPHER_KEY_LEN, SBK_MAC_KEY_LEN);

	ret = 0;
	goto out;

error:
	sbk_error_setx(ctx, "Cannot compute keys");
	ret = -1;

out:
	explicit_bzero(key, sizeof key);
	explicit_bzero(deriv_key, sizeof deriv_key);
	if (md_ctx != NULL)
		EVP_MD_CTX_free(md_ctx);
	return ret;
}

struct sbk_ctx *
sbk_ctx_new(void)
{
	struct sbk_ctx *ctx;
#ifdef HAVE_EVP_MAC
	EVP_MAC *mac;
#endif

	if ((ctx = malloc(sizeof *ctx)) == NULL)
		return NULL;

#ifdef HAVE_EVP_MAC
	ctx->mac_ctx = NULL;
#else
	ctx->hmac_ctx = NULL;
#endif
	ctx->ibuf = NULL;
	ctx->obuf = NULL;
	ctx->ibufsize = 0;
	ctx->obufsize = 0;
	ctx->error = NULL;

	if ((ctx->cipher_ctx = EVP_CIPHER_CTX_new()) == NULL)
		goto error;

#ifdef HAVE_EVP_MAC
	if ((mac = EVP_MAC_fetch(NULL, "HMAC", NULL)) == NULL)
		goto error;

	if ((ctx->mac_ctx = EVP_MAC_CTX_new(mac)) == NULL) {
		EVP_MAC_free(mac);
		goto error;
	}

	EVP_MAC_free(mac);
#else
	if ((ctx->hmac_ctx = HMAC_CTX_new()) == NULL)
		goto error;
#endif

	if (sbk_enlarge_buffers(ctx, 1024) == -1)
		goto error;

	return ctx;

error:
	sbk_ctx_free(ctx);
	return NULL;
}

void
sbk_ctx_free(struct sbk_ctx *ctx)
{
	if (ctx != NULL) {
		sbk_error_clear(ctx);
		EVP_CIPHER_CTX_free(ctx->cipher_ctx);
#ifdef HAVE_EVP_MAC
		EVP_MAC_CTX_free(ctx->mac_ctx);
#else
		HMAC_CTX_free(ctx->hmac_ctx);
#endif
		free(ctx->ibuf);
		free(ctx->obuf);
		free(ctx);
	}
}

int
sbk_open(struct sbk_ctx *ctx, const char *path, const char *passphr)
{
	Signal__BackupFrame	*frm;
	uint8_t			*salt;
	size_t			 saltlen;

	if ((ctx->fp = fopen(path, "rb")) == NULL) {
		sbk_error_set(ctx, NULL);
		return -1;
	}

	ctx->firstframe = 1;
	ctx->eof = 0;

	if ((frm = sbk_get_frame(ctx, NULL)) == NULL)
		goto error;

	if (frm->header == NULL) {
		sbk_error_setx(ctx, "Missing header frame");
		goto error;
	}

	if (!frm->header->has_iv) {
		sbk_error_setx(ctx, "Missing IV");
		goto error;
	}

	if (frm->header->iv.len != SBK_IV_LEN) {
		sbk_error_setx(ctx, "Invalid IV size");
		goto error;
	}

	memcpy(ctx->iv, frm->header->iv.data, SBK_IV_LEN);
	ctx->counter =
	    ((uint32_t)ctx->iv[0] << 24) | ((uint32_t)ctx->iv[1] << 16) |
	    ((uint32_t)ctx->iv[2] <<  8) | ctx->iv[3];

	if (frm->header->has_salt) {
		salt = frm->header->salt.data;
		saltlen = frm->header->salt.len;
	} else {
		salt = NULL;
		saltlen = 0;
	}

	if (sbk_compute_keys(ctx, passphr, salt, saltlen) == -1)
		goto error;

	if (!EVP_DecryptInit_ex(ctx->cipher_ctx, EVP_aes_256_ctr(), NULL, NULL,
	    NULL)) {
		sbk_error_setx(ctx, "Cannot initialise cipher");
		goto error;
	}

#ifdef HAVE_EVP_MAC
	ctx->params[0] = OSSL_PARAM_construct_octet_string("key", ctx->mac_key,
	    SBK_MAC_KEY_LEN);
	ctx->params[1] = OSSL_PARAM_construct_utf8_string("digest", "SHA256",
	    0);
	ctx->params[2] = OSSL_PARAM_construct_end();
#else
	if (!HMAC_Init_ex(ctx->hmac_ctx, ctx->mac_key, SBK_MAC_KEY_LEN,
	    EVP_sha256(), NULL)) {
		sbk_error_setx(ctx, "Cannot initialise MAC");
		goto error;
	}
#endif

	if (sbk_rewind(ctx) == -1)
		goto error;

	sbk_free_frame(frm);
	ctx->db = NULL;
	ctx->db_version = 0;
	RB_INIT(&ctx->attachments);
	RB_INIT(&ctx->recipients);
	return 0;

error:
	explicit_bzero(ctx->cipher_key, SBK_CIPHER_KEY_LEN);
	explicit_bzero(ctx->mac_key, SBK_MAC_KEY_LEN);
	sbk_free_frame(frm);
	fclose(ctx->fp);
	return -1;
}

void
sbk_close(struct sbk_ctx *ctx)
{
	sbk_free_recipient_tree(ctx);
	sbk_free_attachment_tree(ctx);
	explicit_bzero(ctx->cipher_key, SBK_CIPHER_KEY_LEN);
	explicit_bzero(ctx->mac_key, SBK_MAC_KEY_LEN);
	sqlite3_close(ctx->db);
	fclose(ctx->fp);
}

int
sbk_rewind(struct sbk_ctx *ctx)
{
	if (fseek(ctx->fp, 0, SEEK_SET) == -1) {
		sbk_error_set(ctx, "Cannot seek");
		return -1;
	}

	clearerr(ctx->fp);
	ctx->eof = 0;
	ctx->firstframe = 1;
	return 0;
}

int
sbk_eof(struct sbk_ctx *ctx)
{
	return ctx->eof;
}

const char *
sbk_error(struct sbk_ctx *ctx)
{
	return (ctx->error != NULL) ? ctx->error : "Unknown error";
}
