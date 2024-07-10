/*
 * Copyright 2023 Vsevolod Stakhov
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef RSPAMD_FUZZY_STORAGE_H
#define RSPAMD_FUZZY_STORAGE_H

#include "config.h"
#include "rspamd.h"
#include "shingles.h"
#include "cryptobox.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RSPAMD_FUZZY_VERSION 4
#define RSPAMD_FUZZY_KEYLEN 8

#define RSPAMD_FUZZY_FLAG_WEAK (1u << 7u)
/* Use lower 4 bits for the version */
#define RSPAMD_FUZZY_VERSION_MASK 0x0fu
/* Commands for fuzzy storage */
#define FUZZY_CHECK 0
#define FUZZY_WRITE 1
#define FUZZY_DEL 2
#define FUZZY_STAT 3
#define FUZZY_PING 4
#define FUZZY_CLIENT_MAX 4
/* Internal commands */
#define FUZZY_REFRESH 100 /* Update expire */
#define FUZZY_DUP 101     /* Skip duplicate in update queue */

/**
 * The epoch of the fuzzy client
 */
enum rspamd_fuzzy_epoch {
	RSPAMD_FUZZY_EPOCH10, /**< 1.0+ encryption */
	RSPAMD_FUZZY_EPOCH11, /**< 1.7+ extended reply */
	RSPAMD_FUZZY_EPOCH_MAX
};

RSPAMD_PACKED(rspamd_fuzzy_cmd)
{
	guint8 version;
	guint8 cmd;
	guint8 shingles_count;
	guint8 flag;
	gint32 value;
	guint32 tag;
	gchar digest[rspamd_cryptobox_HASHBYTES];
};

RSPAMD_PACKED(rspamd_fuzzy_shingle_cmd)
{
	struct rspamd_fuzzy_cmd basic;
	struct rspamd_shingle sgl;
};

RSPAMD_PACKED(rspamd_fuzzy_reply_v1)
{
	gint32 value;
	guint32 flag;
	guint32 tag;
	float prob;
};

RSPAMD_PACKED(rspamd_fuzzy_reply)
{
	struct rspamd_fuzzy_reply_v1 v1;
	gchar digest[rspamd_cryptobox_HASHBYTES];
	guint32 ts;
	guchar reserved[12];
};

RSPAMD_PACKED(rspamd_fuzzy_encrypted_req_hdr)
{
	guchar magic[4];
	guchar key_id[RSPAMD_FUZZY_KEYLEN];
	guchar pubkey[32];
	guchar nonce[rspamd_cryptobox_MAX_NONCEBYTES];
	guchar mac[rspamd_cryptobox_MAX_MACBYTES];
};

RSPAMD_PACKED(rspamd_fuzzy_encrypted_cmd)
{
	struct rspamd_fuzzy_encrypted_req_hdr hdr;
	struct rspamd_fuzzy_cmd cmd;
};

RSPAMD_PACKED(rspamd_fuzzy_encrypted_shingle_cmd)
{
	struct rspamd_fuzzy_encrypted_req_hdr hdr;
	struct rspamd_fuzzy_shingle_cmd cmd;
};

RSPAMD_PACKED(rspamd_fuzzy_encrypted_rep_hdr)
{
	guchar nonce[rspamd_cryptobox_MAX_NONCEBYTES];
	guchar mac[rspamd_cryptobox_MAX_MACBYTES];
};

RSPAMD_PACKED(rspamd_fuzzy_encrypted_reply)
{
	struct rspamd_fuzzy_encrypted_rep_hdr hdr;
	struct rspamd_fuzzy_reply rep;
};

static const guchar fuzzy_encrypted_magic[4] = {'r', 's', 'f', 'e'};

enum rspamd_fuzzy_extension_type {
	RSPAMD_FUZZY_EXT_SOURCE_DOMAIN = 'd',
	RSPAMD_FUZZY_EXT_SOURCE_IP4 = '4',
	RSPAMD_FUZZY_EXT_SOURCE_IP6 = '6',
};

struct rspamd_fuzzy_cmd_extension {
	enum rspamd_fuzzy_extension_type ext;
	guint length;
	struct rspamd_fuzzy_cmd_extension *next;
	guchar *payload;
};

struct rspamd_fuzzy_stat_entry {
	const gchar *name;
	guint64 fuzzy_cnt;
};

RSPAMD_PACKED(fuzzy_peer_cmd)
{
	gint32 is_shingle;
	union {
		struct rspamd_fuzzy_cmd normal;
		struct rspamd_fuzzy_shingle_cmd shingle;
	} cmd;
};

#ifdef __cplusplus
}
#endif

#endif
