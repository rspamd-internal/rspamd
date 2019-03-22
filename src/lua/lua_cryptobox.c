/*-
 * Copyright 2016 Vsevolod Stakhov
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/***
 * @module rspamd_cryptobox
 * Rspamd cryptobox is a module that operates with digital signatures and
 * hashes.
 * @example
 * local hash = require "rspamd_cryptobox_hash"
 *
 * local h = hash.create()
 * h:update('hello world')
 * print(h:hex())
 */


#include "lua_common.h"
#include "libcryptobox/cryptobox.h"
#include "libcryptobox/keypair.h"
#include "libcryptobox/keypair_private.h"
#include "unix-std.h"
#include "contrib/libottery/ottery.h"

struct rspamd_lua_cryptobox_hash {
	rspamd_cryptobox_hash_state_t *h;
	EVP_MD_CTX *c;
	gboolean is_ssl;
	gboolean is_finished;
};

LUA_FUNCTION_DEF (cryptobox_pubkey,	 load);
LUA_FUNCTION_DEF (cryptobox_pubkey,	 create);
LUA_FUNCTION_DEF (cryptobox_pubkey,	 gc);
LUA_FUNCTION_DEF (cryptobox_keypair,	 load);
LUA_FUNCTION_DEF (cryptobox_keypair,	 create);
LUA_FUNCTION_DEF (cryptobox_keypair,	 gc);
LUA_FUNCTION_DEF (cryptobox_keypair,	 totable);
LUA_FUNCTION_DEF (cryptobox_keypair,	 get_type);
LUA_FUNCTION_DEF (cryptobox_keypair,	 get_alg);
LUA_FUNCTION_DEF (cryptobox_keypair,	 get_pk);
LUA_FUNCTION_DEF (cryptobox_signature, create);
LUA_FUNCTION_DEF (cryptobox_signature, load);
LUA_FUNCTION_DEF (cryptobox_signature, save);
LUA_FUNCTION_DEF (cryptobox_signature, gc);
LUA_FUNCTION_DEF (cryptobox_signature, hex);
LUA_FUNCTION_DEF (cryptobox_signature, base32);
LUA_FUNCTION_DEF (cryptobox_signature, base64);
LUA_FUNCTION_DEF (cryptobox_signature, bin);
LUA_FUNCTION_DEF (cryptobox_hash, create);
LUA_FUNCTION_DEF (cryptobox_hash, create_specific);
LUA_FUNCTION_DEF (cryptobox_hash, create_keyed);
LUA_FUNCTION_DEF (cryptobox_hash, update);
LUA_FUNCTION_DEF (cryptobox_hash, reset);
LUA_FUNCTION_DEF (cryptobox_hash, hex);
LUA_FUNCTION_DEF (cryptobox_hash, base32);
LUA_FUNCTION_DEF (cryptobox_hash, base64);
LUA_FUNCTION_DEF (cryptobox_hash, bin);
LUA_FUNCTION_DEF (cryptobox_hash, gc);
LUA_FUNCTION_DEF (cryptobox, verify_memory);
LUA_FUNCTION_DEF (cryptobox, verify_file);
LUA_FUNCTION_DEF (cryptobox, sign_file);
LUA_FUNCTION_DEF (cryptobox, sign_memory);
LUA_FUNCTION_DEF (cryptobox, encrypt_memory);
LUA_FUNCTION_DEF (cryptobox, encrypt_file);
LUA_FUNCTION_DEF (cryptobox, decrypt_memory);
LUA_FUNCTION_DEF (cryptobox, decrypt_file);
LUA_FUNCTION_DEF (cryptobox, encrypt_cookie);
LUA_FUNCTION_DEF (cryptobox, decrypt_cookie);

static const struct luaL_reg cryptoboxlib_f[] = {
	LUA_INTERFACE_DEF (cryptobox, verify_memory),
	LUA_INTERFACE_DEF (cryptobox, verify_file),
	LUA_INTERFACE_DEF (cryptobox, sign_memory),
	LUA_INTERFACE_DEF (cryptobox, sign_file),
	LUA_INTERFACE_DEF (cryptobox, encrypt_memory),
	LUA_INTERFACE_DEF (cryptobox, encrypt_file),
	LUA_INTERFACE_DEF (cryptobox, decrypt_memory),
	LUA_INTERFACE_DEF (cryptobox, decrypt_file),
	LUA_INTERFACE_DEF (cryptobox, encrypt_cookie),
	LUA_INTERFACE_DEF (cryptobox, decrypt_cookie),
	{NULL, NULL}
};

static const struct luaL_reg cryptoboxpubkeylib_f[] = {
	LUA_INTERFACE_DEF (cryptobox_pubkey, load),
	LUA_INTERFACE_DEF (cryptobox_pubkey, create),
	{NULL, NULL}
};

static const struct luaL_reg cryptoboxpubkeylib_m[] = {
	{"__tostring", rspamd_lua_class_tostring},
	{"__gc", lua_cryptobox_pubkey_gc},
	{NULL, NULL}
};

static const struct luaL_reg cryptoboxkeypairlib_f[] = {
	LUA_INTERFACE_DEF (cryptobox_keypair, load),
	LUA_INTERFACE_DEF (cryptobox_keypair, create),
	{NULL, NULL}
};

static const struct luaL_reg cryptoboxkeypairlib_m[] = {
	{"__tostring", rspamd_lua_class_tostring},
	{"totable", lua_cryptobox_keypair_totable},
	{"get_type", lua_cryptobox_keypair_get_type},
	{"get_alg", lua_cryptobox_keypair_get_alg},
	{"type", lua_cryptobox_keypair_get_type},
	{"alg", lua_cryptobox_keypair_get_alg},
	{"pk", lua_cryptobox_keypair_get_pk},
	{"pubkey", lua_cryptobox_keypair_get_pk},
	{"__gc", lua_cryptobox_keypair_gc},
	{NULL, NULL}
};

static const struct luaL_reg cryptoboxsignlib_f[] = {
	LUA_INTERFACE_DEF (cryptobox_signature, load),
	LUA_INTERFACE_DEF (cryptobox_signature, create),
	{NULL, NULL}
};

static const struct luaL_reg cryptoboxsignlib_m[] = {
	LUA_INTERFACE_DEF (cryptobox_signature, save),
	LUA_INTERFACE_DEF (cryptobox_signature, hex),
	LUA_INTERFACE_DEF (cryptobox_signature, base32),
	LUA_INTERFACE_DEF (cryptobox_signature, base64),
	LUA_INTERFACE_DEF (cryptobox_signature, bin),
	{"__tostring", rspamd_lua_class_tostring},
	{"__gc", lua_cryptobox_signature_gc},
	{NULL, NULL}
};

static const struct luaL_reg cryptoboxhashlib_f[] = {
	LUA_INTERFACE_DEF (cryptobox_hash, create),
	LUA_INTERFACE_DEF (cryptobox_hash, create_keyed),
	LUA_INTERFACE_DEF (cryptobox_hash, create_specific),
	{NULL, NULL}
};

static const struct luaL_reg cryptoboxhashlib_m[] = {
	LUA_INTERFACE_DEF (cryptobox_hash, update),
	LUA_INTERFACE_DEF (cryptobox_hash, reset),
	LUA_INTERFACE_DEF (cryptobox_hash, hex),
	LUA_INTERFACE_DEF (cryptobox_hash, base32),
	LUA_INTERFACE_DEF (cryptobox_hash, base64),
	LUA_INTERFACE_DEF (cryptobox_hash, bin),
	{"__tostring", rspamd_lua_class_tostring},
	{"__gc", lua_cryptobox_hash_gc},
	{NULL, NULL}
};


static struct rspamd_cryptobox_pubkey *
lua_check_cryptobox_pubkey (lua_State * L, int pos)
{
	void *ud = rspamd_lua_check_udata (L, pos, "rspamd{cryptobox_pubkey}");

	luaL_argcheck (L, ud != NULL, 1, "'cryptobox_pubkey' expected");
	return ud ? *((struct rspamd_cryptobox_pubkey **)ud) : NULL;
}

static struct rspamd_cryptobox_keypair *
lua_check_cryptobox_keypair (lua_State * L, int pos)
{
	void *ud = rspamd_lua_check_udata (L, pos, "rspamd{cryptobox_keypair}");

	luaL_argcheck (L, ud != NULL, 1, "'cryptobox_keypair' expected");
	return ud ? *((struct rspamd_cryptobox_keypair **)ud) : NULL;
}

static rspamd_fstring_t *
lua_check_cryptobox_sign (lua_State * L, int pos)
{
	void *ud = rspamd_lua_check_udata (L, pos, "rspamd{cryptobox_signature}");

	luaL_argcheck (L, ud != NULL, 1, "'cryptobox_signature' expected");
	return ud ? *((rspamd_fstring_t **)ud) : NULL;
}

struct rspamd_lua_cryptobox_hash *
lua_check_cryptobox_hash (lua_State * L, int pos)
{
	void *ud = rspamd_lua_check_udata (L, pos, "rspamd{cryptobox_hash}");

	luaL_argcheck (L, ud != NULL, 1, "'cryptobox_hash' expected");
	return ud ? *((struct rspamd_lua_cryptobox_hash **)ud) : NULL;
}

/***
 * @function rspamd_cryptobox_pubkey.load(file[, type[, alg]])
 * Loads public key from base32 encoded file
 * @param {string} file filename to load
 * @param {string} type optional 'sign' or 'kex' for signing and encryption
 * @param {string} alg optional 'default' or 'nist' for curve25519/nistp256 keys
 * @return {cryptobox_pubkey} new public key
 */
static gint
lua_cryptobox_pubkey_load (lua_State *L)
{
	LUA_TRACE_POINT;
	struct rspamd_cryptobox_pubkey *pkey = NULL, **ppkey;
	const gchar *filename, *arg;
	gint type = RSPAMD_KEYPAIR_SIGN;
	gint alg = RSPAMD_CRYPTOBOX_MODE_25519;
	guchar *map;
	gsize len;

	filename = luaL_checkstring (L, 1);
	if (filename != NULL) {
		map = rspamd_file_xmap (filename, PROT_READ, &len, TRUE);

		if (map == NULL) {
			msg_err ("cannot open pubkey from file: %s, %s",
				filename,
				strerror (errno));
			lua_pushnil (L);
		}
		else {
			if (lua_type (L, 2) == LUA_TSTRING) {
				/* keypair type */
				arg = lua_tostring (L, 2);

				if (strcmp (arg, "sign") == 0) {
					type = RSPAMD_KEYPAIR_SIGN;
				}
				else if (strcmp (arg, "kex") == 0) {
					type = RSPAMD_KEYPAIR_KEX;
				}
			}
			if (lua_type (L, 3) == LUA_TSTRING) {
				/* algorithm */
				arg = lua_tostring (L, 3);

				if (strcmp (arg, "default") == 0 || strcmp (arg, "curve25519") == 0) {
					type = RSPAMD_CRYPTOBOX_MODE_25519;
				}
				else if (strcmp (arg, "nist") == 0) {
					type = RSPAMD_CRYPTOBOX_MODE_NIST;
				}
			}

			pkey = rspamd_pubkey_from_base32 (map, len, type, alg);

			if (pkey == NULL) {
				msg_err ("cannot open pubkey from file: %s", filename);
				munmap (map, len);
				lua_pushnil (L);
			}
			else {
				munmap (map, len);
				ppkey = lua_newuserdata (L, sizeof (void *));
				rspamd_lua_setclass (L, "rspamd{cryptobox_pubkey}", -1);
				*ppkey = pkey;
			}
		}
	}
	else {
		return luaL_error (L, "bad input arguments");
	}

	return 1;
}


/***
 * @function rspamd_cryptobox_pubkey.create(data[, type[, alg]])
 * Loads public key from base32 encoded file
 * @param {base32 string} base32 string with the key
 * @param {string} type optional 'sign' or 'kex' for signing and encryption
 * @param {string} alg optional 'default' or 'nist' for curve25519/nistp256 keys
 * @return {cryptobox_pubkey} new public key
 */
static gint
lua_cryptobox_pubkey_create (lua_State *L)
{
	LUA_TRACE_POINT;
	struct rspamd_cryptobox_pubkey *pkey = NULL, **ppkey;
	const gchar *buf, *arg;
	gsize len;
	gint type = RSPAMD_KEYPAIR_SIGN;
	gint alg = RSPAMD_CRYPTOBOX_MODE_25519;

	buf = luaL_checklstring (L, 1, &len);
	if (buf != NULL) {
		if (lua_type (L, 2) == LUA_TSTRING) {
			/* keypair type */
			arg = lua_tostring (L, 2);

			if (strcmp (arg, "sign") == 0) {
				type = RSPAMD_KEYPAIR_SIGN;
			}
			else if (strcmp (arg, "kex") == 0) {
				type = RSPAMD_KEYPAIR_KEX;
			}
		}
		if (lua_type (L, 3) == LUA_TSTRING) {
			/* algorithm */
			arg = lua_tostring (L, 3);

			if (strcmp (arg, "default") == 0 || strcmp (arg, "curve25519") == 0) {
				type = RSPAMD_CRYPTOBOX_MODE_25519;
			}
			else if (strcmp (arg, "nist") == 0) {
				type = RSPAMD_CRYPTOBOX_MODE_NIST;
			}
		}

		pkey = rspamd_pubkey_from_base32 (buf, len, type, alg);

		if (pkey == NULL) {
			msg_err ("cannot load pubkey from string");
			lua_pushnil (L);
		}
		else {
			ppkey = lua_newuserdata (L, sizeof (void *));
			rspamd_lua_setclass (L, "rspamd{cryptobox_pubkey}", -1);
			*ppkey = pkey;
		}

	}
	else {
		return luaL_error (L, "bad input arguments");
	}

	return 1;
}

static gint
lua_cryptobox_pubkey_gc (lua_State *L)
{
	LUA_TRACE_POINT;
	struct rspamd_cryptobox_pubkey *pkey = lua_check_cryptobox_pubkey (L, 1);

	if (pkey != NULL) {
		rspamd_pubkey_unref (pkey);
	}

	return 0;
}

/***
 * @function rspamd_cryptobox_keypair.load(file|table)
 * Loads public key from UCL file or directly from Lua
 * @param {string} file filename to load
 * @return {cryptobox_keypair} new keypair
 */
static gint
lua_cryptobox_keypair_load (lua_State *L)
{
	LUA_TRACE_POINT;
	struct rspamd_cryptobox_keypair *kp, **pkp;
	const gchar *buf;
	gsize len;
	struct ucl_parser *parser;
	ucl_object_t *obj;

	if (lua_type (L, 1) == LUA_TSTRING) {
		buf = luaL_checklstring (L, 1, &len);
		if (buf != NULL) {
			parser = ucl_parser_new (0);

			if (!ucl_parser_add_chunk (parser, buf, len)) {
				msg_err ("cannot open keypair from data: %s",
						ucl_parser_get_error (parser));
				ucl_parser_free (parser);
				lua_pushnil (L);
			}
			else {
				obj = ucl_parser_get_object (parser);
				kp = rspamd_keypair_from_ucl (obj);
				ucl_parser_free (parser);

				if (kp == NULL) {
					msg_err ("cannot load keypair from data");
					ucl_object_unref (obj);
					lua_pushnil (L);
				}
				else {
					pkp = lua_newuserdata (L, sizeof (gpointer));
					*pkp = kp;
					rspamd_lua_setclass (L, "rspamd{cryptobox_keypair}", -1);
					ucl_object_unref (obj);
				}
			}
		}
		else {
			luaL_error (L, "bad input arguments");
		}
	}
	else {
		/* Directly import from lua */
		obj = ucl_object_lua_import (L, 1);
		kp = rspamd_keypair_from_ucl (obj);

		if (kp == NULL) {
			msg_err ("cannot load keypair from data");
			ucl_object_unref (obj);
			lua_pushnil (L);
		}
		else {
			pkp = lua_newuserdata (L, sizeof (gpointer));
			*pkp = kp;
			rspamd_lua_setclass (L, "rspamd{cryptobox_keypair}", -1);
			ucl_object_unref (obj);
		}
	}

	return 1;
}

/***
 * @function rspamd_cryptobox_keypair.create([type='encryption'[, alg='curve25519']])
 * Generates new keypair
 * @param {string} type type of keypair: 'encryption' (default) or 'sign'
 * @param {string} alg algorithm of keypair: 'curve25519' (default) or 'nist'
 * @return {cryptobox_keypair} new keypair
 */
static gint
lua_cryptobox_keypair_create (lua_State *L)
{
	LUA_TRACE_POINT;
	struct rspamd_cryptobox_keypair *kp, **pkp;
	enum rspamd_cryptobox_keypair_type type = RSPAMD_KEYPAIR_KEX;
	enum rspamd_cryptobox_mode alg = RSPAMD_CRYPTOBOX_MODE_25519;

	if (lua_isstring (L, 1)) {
		const gchar *str = lua_tostring (L, 1);

		if (strcmp (str, "sign") == 0) {
			type = RSPAMD_KEYPAIR_SIGN;
		}
		else if (strcmp (str, "encryption") == 0) {
			type = RSPAMD_KEYPAIR_KEX;
		}
		else {
			return luaL_error (L, "invalid keypair type: %s", str);
		}
	}

	if (lua_isstring (L, 2)) {
		const gchar *str = lua_tostring (L, 2);

		if (strcmp (str, "nist") == 0 || strcmp (str, "openssl") == 0) {
			alg = RSPAMD_CRYPTOBOX_MODE_NIST;
		}
		else if (strcmp (str, "curve25519") == 0 || strcmp (str, "default") == 0) {
			alg = RSPAMD_CRYPTOBOX_MODE_25519;
		}
		else {
			return luaL_error (L, "invalid keypair algorithm: %s", str);
		}
	}

	kp = rspamd_keypair_new (type, alg);

	pkp = lua_newuserdata (L, sizeof (gpointer));
	*pkp = kp;
	rspamd_lua_setclass (L, "rspamd{cryptobox_keypair}", -1);

	return 1;
}

static gint
lua_cryptobox_keypair_gc (lua_State *L)
{
	LUA_TRACE_POINT;
	struct rspamd_cryptobox_keypair *kp = lua_check_cryptobox_keypair (L, 1);

	if (kp != NULL) {
		rspamd_keypair_unref (kp);
	}

	return 0;
}

/***
 * @method keypair:totable([hex=false]])
 * Converts keypair to table (not very safe due to memory leftovers)
 */
static gint
lua_cryptobox_keypair_totable (lua_State *L)
{
	LUA_TRACE_POINT;
	struct rspamd_cryptobox_keypair *kp = lua_check_cryptobox_keypair (L, 1);
	ucl_object_t *obj;
	gboolean hex = FALSE;
	gint ret = 1;

	if (kp != NULL) {

		if (lua_isboolean (L, 2)) {
			hex = lua_toboolean (L, 2);
		}

		obj = rspamd_keypair_to_ucl (kp, hex);

		ret = ucl_object_push_lua (L, obj, true);
		ucl_object_unref (obj);
	}
	else {
		return luaL_error (L, "invalid arguments");
	}

	return ret;
}
/***
 * @method keypair:type()
 * Returns type of keypair as a string: 'encryption' or 'sign'
 * @return {string} type of keypair as a string
 */
static gint
lua_cryptobox_keypair_get_type (lua_State *L)
{
	LUA_TRACE_POINT;
	struct rspamd_cryptobox_keypair *kp = lua_check_cryptobox_keypair (L, 1);

	if (kp) {
		if (kp->type == RSPAMD_KEYPAIR_KEX) {
			lua_pushstring (L, "encryption");
		}
		else {
			lua_pushstring (L, "sign");
		}
	}
	else {
		return luaL_error (L, "invalid arguments");
	}

	return 1;
}

/***
 * @method keypair:alg()
 * Returns algorithm of keypair as a string: 'encryption' or 'sign'
 * @return {string} type of keypair as a string
 */
static gint
lua_cryptobox_keypair_get_alg (lua_State *L)
{
	LUA_TRACE_POINT;
	struct rspamd_cryptobox_keypair *kp = lua_check_cryptobox_keypair (L, 1);

	if (kp) {
		if (kp->alg == RSPAMD_CRYPTOBOX_MODE_25519) {
			lua_pushstring (L, "curve25519");
		}
		else {
			lua_pushstring (L, "nist");
		}
	}
	else {
		return luaL_error (L, "invalid arguments");
	}

	return 1;
}

/***
 * @method keypair:pk()
 * Returns pubkey for a specific keypair
 * @return {rspamd_pubkey} pubkey for a keypair
 */
static gint
lua_cryptobox_keypair_get_pk (lua_State *L)
{
	LUA_TRACE_POINT;
	struct rspamd_cryptobox_keypair *kp = lua_check_cryptobox_keypair (L, 1);
	struct rspamd_cryptobox_pubkey *pk, **ppk;
	const guchar *data;
	guint dlen;

	if (kp) {
		data = rspamd_keypair_component (kp, RSPAMD_KEYPAIR_COMPONENT_PK, &dlen);
		pk = rspamd_pubkey_from_bin (data, dlen, kp->type, kp->alg);

		if (pk == NULL) {
			return luaL_error (L, "invalid keypair");
		}

		ppk = lua_newuserdata (L, sizeof (*ppk));
		*ppk = pk;
		rspamd_lua_setclass (L, "rspamd{cryptobox_pubkey}", -1);
	}
	else {
		return luaL_error (L, "invalid arguments");
	}

	return 1;
}

/***
 * @function rspamd_cryptobox_signature.load(file, [alg = 'curve25519'])
 * Loads signature from raw file
 * @param {string} file filename to load
 * @return {cryptobox_signature} new signature
 */
static gint
lua_cryptobox_signature_load (lua_State *L)
{
	LUA_TRACE_POINT;
	rspamd_fstring_t *sig, **psig;
	const gchar *filename;
	gpointer data;
	int fd;
	struct stat st;
	enum rspamd_cryptobox_mode alg = RSPAMD_CRYPTOBOX_MODE_25519;

	filename = luaL_checkstring (L, 1);
	if (filename != NULL) {
		fd = open (filename, O_RDONLY);
		if (fd == -1) {
			msg_err ("cannot open signature file: %s, %s", filename,
				strerror (errno));
			lua_pushnil (L);
		}
		else {
			if (fstat (fd, &st) == -1 ||
				(data =
				mmap (NULL, st.st_size, PROT_READ, MAP_SHARED, fd, 0))
						== MAP_FAILED) {
				msg_err ("cannot mmap file %s: %s", filename, strerror (errno));
				lua_pushnil (L);
			}
			else {
				if (lua_isstring (L, 2)) {
					const gchar *str = lua_tostring (L, 2);

					if (strcmp (str, "nist") == 0 || strcmp (str, "openssl") == 0) {
						alg = RSPAMD_CRYPTOBOX_MODE_NIST;
					}
					else if (strcmp (str, "curve25519") == 0 || strcmp (str, "default") == 0) {
						alg = RSPAMD_CRYPTOBOX_MODE_25519;
					}
					else {
						return luaL_error (L, "invalid keypair algorithm: %s", str);
					}
				}
				if (st.st_size > 0) {
					sig = rspamd_fstring_new_init (data, st.st_size);
					psig = lua_newuserdata (L, sizeof (rspamd_fstring_t *));
					rspamd_lua_setclass (L, "rspamd{cryptobox_signature}", -1);
					*psig = sig;
				}
				else {
					msg_err ("size of %s mismatches: %d while %d is expected",
							filename, (int)st.st_size,
							rspamd_cryptobox_signature_bytes (alg));
					lua_pushnil (L);
				}

				munmap (data, st.st_size);
			}
			close (fd);
		}
	}
	else {
		luaL_error (L, "bad input arguments");
	}

	return 1;
}

/***
 * @method rspamd_cryptobox_signature:save(file)
 * Stores signature in raw file
 * @param {string} file filename to use
 * @return {boolean} true if signature has been saved
 */
static gint
lua_cryptobox_signature_save (lua_State *L)
{
	LUA_TRACE_POINT;
	rspamd_fstring_t *sig;
	gint fd, flags;
	const gchar *filename;
	gboolean forced = FALSE, res = TRUE;

	sig = lua_check_cryptobox_sign (L, 1);
	filename = luaL_checkstring (L, 2);

	if (!sig || !filename) {
		luaL_error (L, "bad input arguments");
		return 1;
	}

	if (lua_gettop (L) > 2) {
		forced = lua_toboolean (L, 3);
	}

	if (sig != NULL && filename != NULL) {
		flags = O_WRONLY | O_CREAT;
		if (forced) {
			flags |= O_TRUNC;
		}
		else {
			flags |= O_EXCL;
		}
		fd = open (filename, flags, 00644);
		if (fd == -1) {
			msg_err ("cannot create a signature file: %s, %s",
				filename,
				strerror (errno));
			lua_pushboolean (L, FALSE);
		}
		else {
			while (write (fd, sig->str, sig->len) == -1) {
				if (errno == EINTR) {
					continue;
				}
				msg_err ("cannot write to a signature file: %s, %s",
					filename,
					strerror (errno));
				res = FALSE;
				break;
			}
			lua_pushboolean (L, res);
			close (fd);
		}
	}
	else {
		return luaL_error (L, "invalid arguments");
	}

	return 1;
}

/***
 * @function rspamd_cryptobox_signature.create(data)
 * Creates signature object from raw data
 * @param {data} raw signature data
 * @return {cryptobox_signature} signature object
 */
static gint
lua_cryptobox_signature_create (lua_State *L)
{
	LUA_TRACE_POINT;
	rspamd_fstring_t *sig, **psig;
	struct rspamd_lua_text *t;
	const gchar *data;
	gsize dlen;

	if (lua_isuserdata (L, 1)) {
		t = lua_check_text (L, 1);

		if (!t) {
			return luaL_error (L, "invalid arguments");
		}

		data = t->start;
		dlen = t->len;
	}
	else {
		data = luaL_checklstring (L, 1, &dlen);
	}

	if (data != NULL) {
		if (dlen == rspamd_cryptobox_signature_bytes (RSPAMD_CRYPTOBOX_MODE_25519)) {
			sig = rspamd_fstring_new_init (data, dlen);
			psig = lua_newuserdata (L, sizeof (rspamd_fstring_t *));
			rspamd_lua_setclass (L, "rspamd{cryptobox_signature}", -1);
			*psig = sig;
		}
	}
	else {
		return luaL_error (L, "bad input arguments");
	}

	return 1;
}

/***
 * @method cryptobox_signature:hex()
 * Return hex encoded signature string
 * @return {string} raw value of signature
 */
static gint
lua_cryptobox_signature_hex (lua_State *L)
{
	LUA_TRACE_POINT;
	rspamd_fstring_t *sig = lua_check_cryptobox_sign (L, 1);
	gchar *encoded;

	if (sig) {
		encoded = rspamd_encode_hex (sig->str, sig->len);
		lua_pushstring (L, encoded);
		g_free (encoded);
	}
	else {
		return luaL_error (L, "invalid arguments");
	}

	return 1;
}

/***
 * @method cryptobox_signature:base32()
 * Return base32 encoded signature string
 * @return {string} raw value of signature
 */
static gint
lua_cryptobox_signature_base32 (lua_State *L)
{
	LUA_TRACE_POINT;
	rspamd_fstring_t *sig = lua_check_cryptobox_sign (L, 1);
	gchar *encoded;

	if (sig) {
		encoded = rspamd_encode_base32 (sig->str, sig->len);
		lua_pushstring (L, encoded);
		g_free (encoded);
	}
	else {
		return luaL_error (L, "invalid arguments");
	}

	return 1;
}

/***
 * @method cryptobox_signature:base64()
 * Return base64 encoded signature string
 * @return {string} raw value of signature
 */
static gint
lua_cryptobox_signature_base64 (lua_State *L)
{
	LUA_TRACE_POINT;
	rspamd_fstring_t *sig = lua_check_cryptobox_sign (L, 1);
	gsize dlen;
	gchar *encoded;

	if (sig) {
		encoded = rspamd_encode_base64 (sig->str, sig->len, 0, &dlen);
		lua_pushlstring (L, encoded, dlen);
		g_free (encoded);
	}
	else {
		return luaL_error (L, "invalid arguments");
	}

	return 1;
}

/***
 * @method cryptobox_signature:bin()
 * Return raw signature string
 * @return {string} raw value of signature
 */
static gint
lua_cryptobox_signature_bin (lua_State *L)
{
	LUA_TRACE_POINT;
	rspamd_fstring_t *sig = lua_check_cryptobox_sign (L, 1);

	if (sig) {
		lua_pushlstring (L, sig->str, sig->len);
	}
	else {
		return luaL_error (L, "invalid arguments");
	}

	return 1;
}

static gint
lua_cryptobox_signature_gc (lua_State *L)
{
	LUA_TRACE_POINT;
	rspamd_fstring_t *sig = lua_check_cryptobox_sign (L, 1);

	rspamd_fstring_free (sig);

	return 0;
}

static void
rspamd_lua_hash_update (struct rspamd_lua_cryptobox_hash *h,
		const void *p, gsize len)
{
	if (h) {
		if (h->is_ssl) {
			EVP_DigestUpdate (h->c, p, len);
		}
		else {
			rspamd_cryptobox_hash_update (h->h, p, len);
		}
	}
}

static struct rspamd_lua_cryptobox_hash *
rspamd_lua_hash_create (const gchar *type)
{
	struct rspamd_lua_cryptobox_hash *h;

	h = g_malloc0 (sizeof (*h));

	if (type) {
		if (g_ascii_strcasecmp (type, "md5") == 0) {
			h->is_ssl = TRUE;
			h->c = EVP_MD_CTX_create ();
			EVP_DigestInit (h->c, EVP_md5 ());

			goto ret;
		}
		else if (g_ascii_strcasecmp (type, "sha1") == 0 ||
					g_ascii_strcasecmp (type, "sha") == 0) {
			h->is_ssl = TRUE;
			h->c = EVP_MD_CTX_create ();
			EVP_DigestInit (h->c, EVP_sha1 ());

			goto ret;
		}
		else if (g_ascii_strcasecmp (type, "sha256") == 0) {
			h->is_ssl = TRUE;
			h->c = EVP_MD_CTX_create ();
			EVP_DigestInit (h->c, EVP_sha256 ());

			goto ret;
		}
		else if (g_ascii_strcasecmp (type, "sha512") == 0) {
			h->is_ssl = TRUE;
			h->c = EVP_MD_CTX_create ();
			EVP_DigestInit (h->c, EVP_sha512 ());

			goto ret;
		}
		else if (g_ascii_strcasecmp (type, "sha384") == 0) {
			h->is_ssl = TRUE;
			h->c = EVP_MD_CTX_create ();
			EVP_DigestInit (h->c, EVP_sha384 ());

			goto ret;
		}
	}

	h->h = g_malloc0 (sizeof (*h->h));
	rspamd_cryptobox_hash_init (h->h, NULL, 0);

ret:
	return h;
}

/***
 * @function rspamd_cryptobox_hash.create([string])
 * Creates new hash context
 * @param {string} data optional string to hash
 * @return {cryptobox_hash} hash object
 */
static gint
lua_cryptobox_hash_create (lua_State *L)
{
	LUA_TRACE_POINT;
	struct rspamd_lua_cryptobox_hash *h, **ph;
	const gchar *s = NULL;
	struct rspamd_lua_text *t;
	gsize len = 0;

	h = rspamd_lua_hash_create (NULL);

	if (lua_type (L, 1) == LUA_TSTRING) {
		s = lua_tolstring (L, 1, &len);
	}
	else if (lua_type (L, 1) == LUA_TUSERDATA) {
		t = lua_check_text (L, 1);

		if (!t) {
			return luaL_error (L, "invalid arguments");
		}

		s = t->start;
		len = t->len;
	}

	if (s) {
		rspamd_lua_hash_update (h, s, len);
	}

	ph = lua_newuserdata (L, sizeof (void *));
	*ph = h;
	rspamd_lua_setclass (L, "rspamd{cryptobox_hash}", -1);

	return 1;
}

/***
 * @function rspamd_cryptobox_hash.create_specific(type, [string])
 * Creates new hash context
 * @param {string} type type of signature
 * @param {string} data raw signature data
 * @return {cryptobox_hash} hash object
 */
static gint
lua_cryptobox_hash_create_specific (lua_State *L)
{
	LUA_TRACE_POINT;
	struct rspamd_lua_cryptobox_hash *h, **ph;
	const gchar *s = NULL, *type = luaL_checkstring (L, 1);
	gsize len = 0;
	struct rspamd_lua_text *t;

	if (!type) {
		return luaL_error (L, "invalid arguments");
	}

	h = rspamd_lua_hash_create (type);

	if (lua_type (L, 2) == LUA_TSTRING) {
		s = lua_tolstring (L, 2, &len);
	}
	else if (lua_type (L, 2) == LUA_TUSERDATA) {
		t = lua_check_text (L, 2);

		if (!t) {
			return luaL_error (L, "invalid arguments");
		}

		s = t->start;
		len = t->len;
	}

	if (s) {
		rspamd_lua_hash_update (h, s, len);
	}

	ph = lua_newuserdata (L, sizeof (void *));
	*ph = h;
	rspamd_lua_setclass (L, "rspamd{cryptobox_hash}", -1);

	return 1;
}

/***
 * @function rspamd_cryptobox_hash.create_keyed(key, [string])
 * Creates new hash context with specified key
 * @param {string} key key
 * @return {cryptobox_hash} hash object
 */
static gint
lua_cryptobox_hash_create_keyed (lua_State *L)
{
	LUA_TRACE_POINT;
	struct rspamd_lua_cryptobox_hash *h, **ph;
	const gchar *key, *s = NULL;
	struct rspamd_lua_text *t;
	gsize len = 0;
	gsize keylen;

	key = luaL_checklstring (L, 1, &keylen);

	if (key != NULL) {
		h = rspamd_lua_hash_create (NULL);
		rspamd_cryptobox_hash_init (h->h, key, keylen);

		if (lua_type (L, 2) == LUA_TSTRING) {
			s = lua_tolstring (L, 2, &len);
		}
		else if (lua_type (L, 2) == LUA_TUSERDATA) {
			t = lua_check_text (L, 2);

			if (!t) {
				return luaL_error (L, "invalid arguments");
			}

			s = t->start;
			len = t->len;
		}

		if (s) {
			rspamd_cryptobox_hash_update (h, s, len);
		}

		ph = lua_newuserdata (L, sizeof (void *));
		*ph = h;
		rspamd_lua_setclass (L, "rspamd{cryptobox_hash}", -1);
	}
	else {
		return luaL_error (L, "invalid arguments");
	}

	return 1;
}

/***
 * @method cryptobox_hash:update(data)
 * Updates hash with the specified data (hash should not be finalized using `hex` or `bin` methods)
 * @param {string} data data to hash
 */
static gint
lua_cryptobox_hash_update (lua_State *L)
{
	LUA_TRACE_POINT;
	struct rspamd_lua_cryptobox_hash *h = lua_check_cryptobox_hash (L, 1);
	const gchar *data;
	struct rspamd_lua_text *t;
	gsize len;

	if (lua_isuserdata (L, 2)) {
		t = lua_check_text (L, 2);

		if (!t) {
			return luaL_error (L, "invalid arguments");
		}

		data = t->start;
		len = t->len;
	}
	else {
		data = luaL_checklstring (L, 2, &len);
	}

	if (lua_isnumber (L, 3)) {
		gsize nlen = lua_tonumber (L, 3);

		if (nlen > len) {
			return luaL_error (L, "invalid length: %d while %d is available",
					(int)nlen, (int)len);
		}

		len = nlen;
	}

	if (h && !h->is_finished && data) {
		rspamd_lua_hash_update (h, data, len);
	}
	else {
		return luaL_error (L, "invalid arguments");
	}

	return 0;
}

/***
 * @method cryptobox_hash:reset()
 * Resets hash to the initial state
 */
static gint
lua_cryptobox_hash_reset (lua_State *L)
{
	LUA_TRACE_POINT;
	struct rspamd_lua_cryptobox_hash *h = lua_check_cryptobox_hash (L, 1);

	if (h) {
		if (h->is_ssl) {
			EVP_DigestInit (h->c, EVP_MD_CTX_md (h->c));
		}
		else {
			memset (h->h, 0, sizeof (*h->h));
			rspamd_cryptobox_hash_init (h->h, NULL, 0);
		}
		h->is_finished = FALSE;
	}
	else {
		return luaL_error (L, "invalid arguments");
	}

	return 0;
}

/***
 * @method cryptobox_hash:hex()
 * Finalizes hash and return it as hex string
 * @return {string} hex value of hash
 */
static gint
lua_cryptobox_hash_hex (lua_State *L)
{
	LUA_TRACE_POINT;
	struct rspamd_lua_cryptobox_hash *h = lua_check_cryptobox_hash (L, 1);
	guchar out[rspamd_cryptobox_HASHBYTES],
		out_hex[rspamd_cryptobox_HASHBYTES * 2 + 1];
	guint dlen;

	if (h && !h->is_finished) {
		memset (out_hex, 0, sizeof (out_hex));

		if (h->is_ssl) {
			dlen = sizeof (out);
			EVP_DigestFinal_ex (h->c, out, &dlen);
		}
		else {
			dlen = sizeof (out);
			rspamd_cryptobox_hash_final (h->h, out);
		}

		rspamd_encode_hex_buf (out, dlen, out_hex, sizeof (out_hex));
		lua_pushstring (L, out_hex);
		h->is_finished = TRUE;
	}
	else {
		return luaL_error (L, "invalid arguments");
	}

	return 1;
}

/***
 * @method cryptobox_hash:base32()
 * Finalizes hash and return it as zbase32 string
 * @return {string} base32 value of hash
 */
static gint
lua_cryptobox_hash_base32 (lua_State *L)
{
	LUA_TRACE_POINT;
	struct rspamd_lua_cryptobox_hash *h = lua_check_cryptobox_hash (L, 1);
	guchar out[rspamd_cryptobox_HASHBYTES],
		out_b32[rspamd_cryptobox_HASHBYTES * 2];
	guint dlen;

	if (h && !h->is_finished) {
		memset (out_b32, 0, sizeof (out_b32));
		if (h->is_ssl) {
			dlen = sizeof (out);
			EVP_DigestFinal_ex (h->c, out, &dlen);
		}
		else {
			dlen = sizeof (out);
			rspamd_cryptobox_hash_final (h->h, out);
		}

		rspamd_encode_base32_buf (out, dlen, out_b32, sizeof (out_b32));
		lua_pushstring (L, out_b32);
		h->is_finished = TRUE;
	}
	else {
		return luaL_error (L, "invalid arguments");
	}

	return 1;
}

/***
 * @method cryptobox_hash:base64()
 * Finalizes hash and return it as base64 string
 * @return {string} base64 value of hash
 */
static gint
lua_cryptobox_hash_base64 (lua_State *L)
{
	LUA_TRACE_POINT;
	struct rspamd_lua_cryptobox_hash *h = lua_check_cryptobox_hash (L, 1);
	guchar out[rspamd_cryptobox_HASHBYTES], *b64;
	gsize len;
	guint dlen;

	if (h && !h->is_finished) {
		if (h->is_ssl) {
			dlen = sizeof (out);
			EVP_DigestFinal_ex (h->c, out, &dlen);
		}
		else {
			dlen = sizeof (out);
			rspamd_cryptobox_hash_final (h->h, out);
		}

		b64 = rspamd_encode_base64 (out, dlen, 0, &len);
		lua_pushlstring (L, b64, len);
		g_free (b64);
		h->is_finished = TRUE;
	}
	else {
		return luaL_error (L, "invalid arguments");
	}

	return 1;
}

/***
 * @method cryptobox_hash:bin()
 * Finalizes hash and return it as raw string
 * @return {string} raw value of hash
 */
static gint
lua_cryptobox_hash_bin (lua_State *L)
{
	LUA_TRACE_POINT;
	struct rspamd_lua_cryptobox_hash *h = lua_check_cryptobox_hash (L, 1);
	guchar out[rspamd_cryptobox_HASHBYTES];
	guint dlen;

	if (h && !h->is_finished) {
		if (h->is_ssl) {
			dlen = sizeof (out);
			EVP_DigestFinal_ex (h->c, out, &dlen);
		}
		else {
			dlen = sizeof (out);
			rspamd_cryptobox_hash_final (h->h, out);
		}

		lua_pushlstring (L, out, dlen);
		h->is_finished = TRUE;
	}
	else {
		return luaL_error (L, "invalid arguments");
	}

	return 1;
}

static gint
lua_cryptobox_hash_gc (lua_State *L)
{
	LUA_TRACE_POINT;
	struct rspamd_lua_cryptobox_hash *h = lua_check_cryptobox_hash (L, 1);

	if (h->is_ssl) {
#if OPENSSL_VERSION_NUMBER < 0x10100000L || defined(LIBRESSL_VERSION_NUMBER)
		EVP_MD_CTX_cleanup (h->c);
#else
		EVP_MD_CTX_reset (h->c);
#endif
		EVP_MD_CTX_destroy (h->c);
	}
	else {
		rspamd_explicit_memzero (h->h, sizeof (*h->h));
		g_free (h->h);
	}

	g_free (h);

	return 0;
}

/***
 * @function rspamd_cryptobox.verify_memory(pk, sig, data, [alg = 'curve25519'])
 * Check memory using specified cryptobox key and signature
 * @param {pubkey} pk public key to verify
 * @param {sig} signature to check
 * @param {string} data data to check signature against
 * @return {boolean} `true` - if string matches cryptobox signature
 */
static gint
lua_cryptobox_verify_memory (lua_State *L)
{
	LUA_TRACE_POINT;
	struct rspamd_cryptobox_pubkey *pk;
	rspamd_fstring_t *signature;
	struct rspamd_lua_text *t;
	const gchar *data;
	enum rspamd_cryptobox_mode alg = RSPAMD_CRYPTOBOX_MODE_25519;
	gsize len;
	gint ret;

	pk = lua_check_cryptobox_pubkey (L, 1);
	signature = lua_check_cryptobox_sign (L, 2);

	if (lua_isuserdata (L, 3)) {
		t = lua_check_text (L, 3);

		if (!t) {
			return luaL_error (L, "invalid arguments");
		}

		data = t->start;
		len = t->len;
	}
	else {
		data = luaL_checklstring (L, 3, &len);
	}

	if (lua_isstring (L, 4)) {
		const gchar *str = lua_tostring (L, 4);

		if (strcmp (str, "nist") == 0 || strcmp (str, "openssl") == 0) {
			alg = RSPAMD_CRYPTOBOX_MODE_NIST;
		}
		else if (strcmp (str, "curve25519") == 0 || strcmp (str, "default") == 0) {
			alg = RSPAMD_CRYPTOBOX_MODE_25519;
		}
		else {
			return luaL_error (L, "invalid algorithm: %s", str);
		}
	}

	if (pk != NULL && signature != NULL && data != NULL) {
		ret = rspamd_cryptobox_verify (signature->str, signature->len, data, len,
				rspamd_pubkey_get_pk (pk, NULL), alg);

		if (ret) {
			lua_pushboolean (L, 1);
		}
		else {
			lua_pushboolean (L, 0);
		}
	}
	else {
		return luaL_error (L, "invalid arguments");
	}

	return 1;
}

/***
 * @function rspamd_cryptobox.verify_file(pk, sig, file, [alg = 'curve25519'])
 * Check file using specified cryptobox key and signature
 * @param {pubkey} pk public key to verify
 * @param {sig} signature to check
 * @param {string} file to load data from
 * @return {boolean} `true` - if string matches cryptobox signature
 */
static gint
lua_cryptobox_verify_file (lua_State *L)
{
	LUA_TRACE_POINT;
	const gchar *fname;
	struct rspamd_cryptobox_pubkey *pk;
	rspamd_fstring_t *signature;
	guchar *map = NULL;
	enum rspamd_cryptobox_mode alg = RSPAMD_CRYPTOBOX_MODE_25519;
	gsize len;
	gint ret;

	pk = lua_check_cryptobox_pubkey (L, 1);
	signature = lua_check_cryptobox_sign (L, 2);
	fname = luaL_checkstring (L, 3);

	if (lua_isstring (L, 4)) {
		const gchar *str = lua_tostring (L, 4);

		if (strcmp (str, "nist") == 0 || strcmp (str, "openssl") == 0) {
			alg = RSPAMD_CRYPTOBOX_MODE_NIST;
		}
		else if (strcmp (str, "curve25519") == 0 || strcmp (str, "default") == 0) {
			alg = RSPAMD_CRYPTOBOX_MODE_25519;
		}
		else {
			return luaL_error (L, "invalid algorithm: %s", str);
		}
	}

	map = rspamd_file_xmap (fname, PROT_READ, &len, TRUE);

	if (map != NULL && pk != NULL && signature != NULL) {
		ret = rspamd_cryptobox_verify (signature->str, signature->len,
				map, len,
				rspamd_pubkey_get_pk (pk, NULL), alg);

		if (ret) {
			lua_pushboolean (L, 1);
		}
		else {
			lua_pushboolean (L, 0);
		}
	}
	else {
		if (map != NULL) {
			munmap (map, len);
		}

		return luaL_error (L, "invalid arguments");
	}

	if (map != NULL) {
		munmap (map, len);
	}

	return 1;
}

/***
 * @function rspamd_cryptobox.sign_memory(kp, data)
 * Sign data using specified keypair
 * @param {keypair} kp keypair to sign
 * @param {string} data
 * @return {cryptobox_signature} signature object
 */
static gint
lua_cryptobox_sign_memory (lua_State *L)
{
	LUA_TRACE_POINT;
	struct rspamd_cryptobox_keypair *kp;
	const gchar *data;
	struct rspamd_lua_text *t;
	gsize len = 0;
	rspamd_fstring_t *sig, **psig;

	kp = lua_check_cryptobox_keypair (L, 1);

	if (lua_isuserdata (L, 2)) {
		t = lua_check_text (L, 2);

		if (!t) {
			return luaL_error (L, "invalid arguments");
		}

		data = t->start;
		len = t->len;
	}
	else {
		data = luaL_checklstring (L, 2, &len);
	}


	if (!kp || !data || kp->type == RSPAMD_KEYPAIR_KEX) {
		return luaL_error (L, "invalid arguments");
	}

	sig = rspamd_fstring_sized_new (rspamd_cryptobox_signature_bytes (
			rspamd_keypair_alg (kp)));
	rspamd_cryptobox_sign (sig->str, &sig->len, data,
			len, rspamd_keypair_component (kp, RSPAMD_KEYPAIR_COMPONENT_SK,
					NULL), rspamd_keypair_alg (kp));

	psig = lua_newuserdata (L, sizeof (void *));
	*psig = sig;
	rspamd_lua_setclass (L, "rspamd{cryptobox_signature}", -1);

	return 1;
}

/***
 * @function rspamd_cryptobox.sign_file(kp, file)
 * Sign file using specified keypair
 * @param {keypair} kp keypair to sign
 * @param {string} filename
 * @return {cryptobox_signature} signature object
 */
static gint
lua_cryptobox_sign_file (lua_State *L)
{
	LUA_TRACE_POINT;
	struct rspamd_cryptobox_keypair *kp;
	const gchar *filename;
	gchar *data;
	gsize len = 0;
	rspamd_fstring_t *sig, **psig;

	kp = lua_check_cryptobox_keypair (L, 1);
	filename = luaL_checkstring (L, 2);

	if (!kp || !filename) {
		return luaL_error (L, "invalid arguments");
	}

	data = rspamd_file_xmap (filename, PROT_READ, &len, TRUE);

	if (data == NULL) {
		msg_err ("cannot mmap file %s: %s", filename, strerror (errno));
		lua_pushnil (L);
	}
	else {
		sig = rspamd_fstring_sized_new (rspamd_cryptobox_signature_bytes (
				rspamd_keypair_alg (kp)));
		rspamd_cryptobox_sign (sig->str, &sig->len, data,
				len, rspamd_keypair_component (kp, RSPAMD_KEYPAIR_COMPONENT_SK,
						NULL), rspamd_keypair_alg (kp));

		psig = lua_newuserdata (L, sizeof (void *));
		*psig = sig;
		rspamd_lua_setclass (L, "rspamd{cryptobox_signature}", -1);
		munmap (data, len);
	}

	return 1;
}

/***
 * @function rspamd_cryptobox.encrypt_memory(kp, data[, nist=false])
 * Encrypt data using specified keypair/pubkey
 * @param {keypair|string} kp keypair or pubkey in base32 to use
 * @param {string|text} data
 * @return {rspamd_text} encrypted text
 */
static gint
lua_cryptobox_encrypt_memory (lua_State *L)
{
	LUA_TRACE_POINT;
	struct rspamd_cryptobox_keypair *kp = NULL;
	struct rspamd_cryptobox_pubkey *pk = NULL;
	const gchar *data;
	guchar *out = NULL;
	struct rspamd_lua_text *t, *res;
	gsize len = 0, outlen = 0;
	GError *err = NULL;

	if (lua_type (L, 1) == LUA_TUSERDATA) {
		if (rspamd_lua_check_udata_maybe (L, 1, "rspamd{cryptobox_keypair}")) {
			kp = lua_check_cryptobox_keypair (L, 1);
		}
		else if (rspamd_lua_check_udata_maybe (L, 1, "rspamd{cryptobox_pubkey}")) {
			pk = lua_check_cryptobox_pubkey (L, 1);
		}
	}
	else if (lua_type (L, 1) == LUA_TSTRING) {
		const gchar *b32;
		gsize blen;

		b32 = lua_tolstring (L, 1, &blen);
		pk = rspamd_pubkey_from_base32 (b32, blen, RSPAMD_KEYPAIR_KEX,
				lua_toboolean (L, 3) ?
				RSPAMD_CRYPTOBOX_MODE_NIST : RSPAMD_CRYPTOBOX_MODE_25519);
	}

	if (lua_isuserdata (L, 2)) {
		t = lua_check_text (L, 2);

		if (!t) {
			return luaL_error (L, "invalid arguments");
		}

		data = t->start;
		len = t->len;
	}
	else {
		data = luaL_checklstring (L, 2, &len);
	}


	if (!(kp || pk) || !data) {
		return luaL_error (L, "invalid arguments");
	}

	if (kp) {
		if (!rspamd_keypair_encrypt (kp, data, len, &out, &outlen, &err)) {
			gint ret = luaL_error (L, "cannot encrypt data: %s", err->message);
			g_error_free (err);

			return ret;
		}
	}
	else if (pk) {
		if (!rspamd_pubkey_encrypt (pk, data, len, &out, &outlen, &err)) {
			gint ret = luaL_error (L, "cannot encrypt data: %s", err->message);
			g_error_free (err);

			return ret;
		}
	}

	res = lua_newuserdata (L, sizeof (*res));
	res->flags = RSPAMD_TEXT_FLAG_OWN;
	res->start = out;
	res->len = outlen;
	rspamd_lua_setclass (L, "rspamd{text}", -1);

	return 1;
}

/***
 * @function rspamd_cryptobox.encrypt_file(kp|pk_string, filename[, nist=false])
 * Encrypt data using specified keypair/pubkey
 * @param {keypair|string} kp keypair or pubkey in base32 to use
 * @param {string} filename
 * @return {rspamd_text} encrypted text
 */
static gint
lua_cryptobox_encrypt_file (lua_State *L)
{
	LUA_TRACE_POINT;
	struct rspamd_cryptobox_keypair *kp = NULL;
	struct rspamd_cryptobox_pubkey *pk = NULL;
	const gchar *filename;
	gchar *data;
	guchar *out = NULL;
	struct rspamd_lua_text *res;
	gsize len = 0, outlen = 0;
	GError *err = NULL;

	if (lua_type (L, 1) == LUA_TUSERDATA) {
		if (rspamd_lua_check_udata_maybe (L, 1, "rspamd{cryptobox_keypair}")) {
			kp = lua_check_cryptobox_keypair (L, 1);
		}
		else if (rspamd_lua_check_udata_maybe (L, 1, "rspamd{cryptobox_pubkey}")) {
			pk = lua_check_cryptobox_pubkey (L, 1);
		}
	}
	else if (lua_type (L, 1) == LUA_TSTRING) {
		const gchar *b32;
		gsize blen;

		b32 = lua_tolstring (L, 1, &blen);
		pk = rspamd_pubkey_from_base32 (b32, blen, RSPAMD_KEYPAIR_KEX,
				lua_toboolean (L, 3) ?
				RSPAMD_CRYPTOBOX_MODE_NIST : RSPAMD_CRYPTOBOX_MODE_25519);
	}

	filename = luaL_checkstring (L, 2);
	data = rspamd_file_xmap (filename, PROT_READ, &len, TRUE);

	if (!(kp || pk) || !data) {
		return luaL_error (L, "invalid arguments");
	}

	if (kp) {
		if (!rspamd_keypair_encrypt (kp, data, len, &out, &outlen, &err)) {
			gint ret = luaL_error (L, "cannot encrypt file %s: %s", filename,
					err->message);
			g_error_free (err);
			munmap (data, len);

			return ret;
		}
	}
	else if (pk) {
		if (!rspamd_pubkey_encrypt (pk, data, len, &out, &outlen, &err)) {
			gint ret = luaL_error (L, "cannot encrypt file %s: %s", filename,
					err->message);
			g_error_free (err);
			munmap (data, len);

			return ret;
		}
	}

	res = lua_newuserdata (L, sizeof (*res));
	res->flags = RSPAMD_TEXT_FLAG_OWN;
	res->start = out;
	res->len = outlen;
	rspamd_lua_setclass (L, "rspamd{text}", -1);
	munmap (data, len);

	return 1;
}

/***
 * @function rspamd_cryptobox.decrypt_memory(kp, data[, nist = false])
 * Encrypt data using specified keypair
 * @param {keypair} kp keypair to use
 * @param {string} data
 * @return status,{rspamd_text}|error status is boolean variable followed by either unencrypted data or an error message
 */
static gint
lua_cryptobox_decrypt_memory (lua_State *L)
{
	LUA_TRACE_POINT;
	struct rspamd_cryptobox_keypair *kp;
	const gchar *data;
	guchar *out;
	struct rspamd_lua_text *t, *res;
	gsize len = 0, outlen;
	GError *err = NULL;

	kp = lua_check_cryptobox_keypair (L, 1);

	if (lua_isuserdata (L, 2)) {
		t = lua_check_text (L, 2);

		if (!t) {
			return luaL_error (L, "invalid arguments");
		}

		data = t->start;
		len = t->len;
	}
	else {
		data = luaL_checklstring (L, 2, &len);
	}


	if (!kp || !data) {
		return luaL_error (L, "invalid arguments");
	}

	if (!rspamd_keypair_decrypt (kp, data, len, &out, &outlen, &err)) {
		lua_pushboolean (L, false);
		lua_pushstring (L, err->message);
		g_error_free (err);
	}
	else {
		lua_pushboolean (L, true);
		res = lua_newuserdata (L, sizeof (*res));
		res->flags = RSPAMD_TEXT_FLAG_OWN;
		res->start = out;
		res->len = outlen;
		rspamd_lua_setclass (L, "rspamd{text}", -1);
	}

	return 2;
}

/***
 * @function rspamd_cryptobox.decrypt_file(kp, filename)
 * Encrypt data using specified keypair
 * @param {keypair} kp keypair to use
 * @param {string} filename
 * @return status,{rspamd_text}|error status is boolean variable followed by either unencrypted data or an error message
 */
static gint
lua_cryptobox_decrypt_file (lua_State *L)
{
	LUA_TRACE_POINT;
	struct rspamd_cryptobox_keypair *kp;
	const gchar *filename;
	gchar *data;
	guchar *out;
	struct rspamd_lua_text *res;
	gsize len = 0, outlen;
	GError *err = NULL;

	kp = lua_check_cryptobox_keypair (L, 1);
	filename = luaL_checkstring (L, 2);
	data = rspamd_file_xmap (filename, PROT_READ, &len, TRUE);


	if (!kp || !data) {
		return luaL_error (L, "invalid arguments");
	}

	if (!rspamd_keypair_decrypt (kp, data, len, &out, &outlen, &err)) {
		lua_pushboolean (L, false);
		lua_pushstring (L, err->message);
		g_error_free (err);
	}
	else {
		lua_pushboolean (L, true);
		res = lua_newuserdata (L, sizeof (*res));
		res->flags = RSPAMD_TEXT_FLAG_OWN;
		res->start = out;
		res->len = outlen;
		rspamd_lua_setclass (L, "rspamd{text}", -1);
	}

	munmap (data, len);

	return 2;
}

#define RSPAMD_CRYPTOBOX_AES_BLOCKSIZE 16
#define RSPAMD_CRYPTOBOX_AES_KEYSIZE 16

/***
 * @function rspamd_cryptobox.encrypt_cookie(secret_key, secret_cookie)
 * Specialised function that performs AES-CTR encryption of the provided cookie
 * ```
 * e := base64(nonce||aesencrypt(nonce, secret_cookie))
 * nonce := uint32_le(unix_timestamp)||random_64bit
 * aesencrypt := aes_ctr(nonce, secret_key) ^ pad(secret_cookie)
 * pad := secret_cookie || 0^(32-len(secret_cookie))
 * ```
 * @param {string} secret_key secret key as a hex string (must be 16 bytes in raw or 32 in hex)
 * @param {string} secret_cookie secret cookie as a string for up to 31 character
 * @return {string} e function value for this sk and cookie
 */
static gint
lua_cryptobox_encrypt_cookie (lua_State *L)
{
	guchar aes_block[RSPAMD_CRYPTOBOX_AES_BLOCKSIZE], *blk;
	guchar padded_cookie[RSPAMD_CRYPTOBOX_AES_BLOCKSIZE];
	guchar nonce[RSPAMD_CRYPTOBOX_AES_BLOCKSIZE];
	guchar aes_key[RSPAMD_CRYPTOBOX_AES_KEYSIZE];
	guchar result[RSPAMD_CRYPTOBOX_AES_BLOCKSIZE * 2];
	guint32 ts;

	const gchar *sk, *cookie;
	gsize sklen, cookie_len;
	gint bklen;

	sk = lua_tolstring (L, 1, &sklen);
	cookie = lua_tolstring (L, 2, &cookie_len);

	if (sk && cookie) {
		if (sklen == 32) {
			/* Hex */
			rspamd_decode_hex_buf (sk, sklen, aes_key, sizeof (aes_key));
		}
		else if (sklen == RSPAMD_CRYPTOBOX_AES_KEYSIZE) {
			/* Raw */
			memcpy (aes_key, sk, sizeof (aes_key));
		}
		else {
			return luaL_error (L, "invalid keysize %d", (gint)sklen);
		}

		if (cookie_len > sizeof (padded_cookie) - 1) {
			return luaL_error (L, "cookie is too long %d", (gint)cookie_len);
		}

		/* Fill nonce */
		ottery_rand_bytes (nonce, sizeof (guint64) + sizeof (guint32));
		ts = (guint32)rspamd_get_calendar_ticks ();
		ts = GUINT32_TO_LE (ts);
		memcpy (nonce + sizeof (guint64) + sizeof (guint32), &ts, sizeof (ts));

		/* Prepare padded cookie */
		memset (padded_cookie, 0, sizeof (padded_cookie));
		memcpy (padded_cookie, cookie, cookie_len);

		/* Perform AES CTR via AES ECB on nonce */
		EVP_CIPHER_CTX *ctx;
		ctx = EVP_CIPHER_CTX_new ();
		EVP_EncryptInit_ex (ctx, EVP_aes_128_ecb (), NULL, aes_key, NULL);
		EVP_CIPHER_CTX_set_padding (ctx, 0);

		bklen = sizeof (aes_block);
		blk = aes_block;
		g_assert (EVP_EncryptUpdate (ctx, blk, &bklen, nonce, sizeof (nonce)));
		blk += bklen;
		g_assert (EVP_EncryptFinal_ex(ctx, blk, &bklen));
		EVP_CIPHER_CTX_free (ctx);

		/* Encode result */
		memcpy (result, nonce, sizeof (nonce));
		for (guint i = 0; i < sizeof (aes_block); i ++) {
			result[i + sizeof (nonce)] = padded_cookie[i] ^ aes_block[i];
		}

		gsize rlen;
		gchar *res = rspamd_encode_base64 (result, sizeof (result),
				0, &rlen);

		lua_pushlstring (L, res, rlen);
		g_free (res);
		rspamd_explicit_memzero (aes_key, sizeof (aes_key));
		rspamd_explicit_memzero (aes_block, sizeof (aes_block));
	}
	else {
		return luaL_error (L, "invalid arguments");
	}

	return 1;
}

/***
 * @function rspamd_cryptobox.decrypt_cookie(secret_key, encrypted_cookie)
 * Specialised function that performs AES-CTR decryption of the provided cookie in form
 * ```
 * e := base64(nonce||aesencrypt(nonce, secret_cookie))
 * nonce := int32_le(unix_timestamp)||random_96bit
 * aesencrypt := aes_ctr(nonce, secret_key) ^ pad(secret_cookie)
 * pad := secret_cookie || 0^(32-len(secret_cookie))
 * ```
 * @param {string} secret_key secret key as a hex string (must be 16 bytes in raw or 32 in hex)
 * @param {string} encrypted_cookie encrypted cookie as a base64 encoded string
 * @return {string+number} decrypted value of the cookie and the cookie timestamp
 */
static gint
lua_cryptobox_decrypt_cookie (lua_State *L)
{
	guchar *blk;
	guchar nonce[RSPAMD_CRYPTOBOX_AES_BLOCKSIZE];
	guchar aes_key[RSPAMD_CRYPTOBOX_AES_KEYSIZE];
	guchar *src;
	guint32 ts;

	const gchar *sk, *cookie;
	gsize sklen, cookie_len;
	gint bklen;

	sk = lua_tolstring (L, 1, &sklen);
	cookie = lua_tolstring (L, 2, &cookie_len);

	if (sk && cookie) {
		if (sklen == 32) {
			/* Hex */
			rspamd_decode_hex_buf (sk, sklen, aes_key, sizeof (aes_key));
		}
		else if (sklen == RSPAMD_CRYPTOBOX_AES_KEYSIZE) {
			/* Raw */
			memcpy (aes_key, sk, sizeof (aes_key));
		}
		else {
			return luaL_error (L, "invalid keysize %d", (gint)sklen);
		}

		src = g_malloc (cookie_len);

		rspamd_cryptobox_base64_decode (cookie, cookie_len, src, &cookie_len);

		if (cookie_len != RSPAMD_CRYPTOBOX_AES_BLOCKSIZE * 2) {
			g_free (src);
			lua_pushnil (L);

			return 1;
		}

		/* Perform AES CTR via AES ECB on nonce */
		EVP_CIPHER_CTX *ctx;
		ctx = EVP_CIPHER_CTX_new ();
		/* As per CTR definition, we use encrypt for both encrypt and decrypt */
		EVP_EncryptInit_ex (ctx, EVP_aes_128_ecb (), NULL, aes_key, NULL);
		EVP_CIPHER_CTX_set_padding (ctx, 0);

		/* Copy time */
		memcpy (&ts, src + sizeof (guint64) + sizeof (guint32), sizeof (ts));
		ts = GUINT32_FROM_LE (ts);
		bklen = sizeof (nonce);
		blk = nonce;
		g_assert (EVP_EncryptUpdate (ctx, blk, &bklen, src,
				RSPAMD_CRYPTOBOX_AES_BLOCKSIZE));
		blk += bklen;
		g_assert (EVP_EncryptFinal_ex (ctx, blk, &bklen));
		EVP_CIPHER_CTX_free (ctx);

		/* Decode result */
		for (guint i = 0; i < RSPAMD_CRYPTOBOX_AES_BLOCKSIZE; i ++) {
			src[i + sizeof (nonce)] ^= nonce[i];
		}

		if (src[RSPAMD_CRYPTOBOX_AES_BLOCKSIZE * 2 - 1] != '\0') {
			/* Bad cookie */
			lua_pushnil (L);
			lua_pushnil (L);
		}
		else {
			lua_pushstring (L, src + sizeof (nonce));
			lua_pushnumber (L, ts);
		}

		rspamd_explicit_memzero (src, RSPAMD_CRYPTOBOX_AES_BLOCKSIZE * 2);
		g_free (src);
		rspamd_explicit_memzero (aes_key, sizeof (aes_key));
	}
	else {
		return luaL_error (L, "invalid arguments");
	}

	return 2;
}

static gint
lua_load_pubkey (lua_State * L)
{
	lua_newtable (L);
	luaL_register (L, NULL, cryptoboxpubkeylib_f);

	return 1;
}

static gint
lua_load_keypair (lua_State * L)
{
	lua_newtable (L);
	luaL_register (L, NULL, cryptoboxkeypairlib_f);

	return 1;
}

static gint
lua_load_signature (lua_State * L)
{
	lua_newtable (L);
	luaL_register (L, NULL, cryptoboxsignlib_f);

	return 1;
}

static gint
lua_load_hash (lua_State * L)
{
	lua_newtable (L);
	luaL_register (L, NULL, cryptoboxhashlib_f);

	return 1;
}

static gint
lua_load_cryptobox (lua_State * L)
{
	lua_newtable (L);
	luaL_register (L, NULL, cryptoboxlib_f);

	return 1;
}

void
luaopen_cryptobox (lua_State * L)
{
	luaL_newmetatable (L, "rspamd{cryptobox_pubkey}");
	lua_pushstring (L, "__index");
	lua_pushvalue (L, -2);
	lua_settable (L, -3);

	lua_pushstring (L, "class");
	lua_pushstring (L, "rspamd{cryptobox_pubkey}");
	lua_rawset (L, -3);

	luaL_register (L, NULL, cryptoboxpubkeylib_m);
	rspamd_lua_add_preload (L, "rspamd_cryptobox_pubkey", lua_load_pubkey);

	luaL_newmetatable (L, "rspamd{cryptobox_keypair}");
	lua_pushstring (L, "__index");
	lua_pushvalue (L, -2);
	lua_settable (L, -3);

	lua_pushstring (L, "class");
	lua_pushstring (L, "rspamd{cryptobox_keypair}");
	lua_rawset (L, -3);

	luaL_register (L, NULL, cryptoboxkeypairlib_m);
	rspamd_lua_add_preload (L, "rspamd_cryptobox_keypair", lua_load_keypair);

	luaL_newmetatable (L, "rspamd{cryptobox_signature}");

	lua_pushstring (L, "__index");
	lua_pushvalue (L, -2);
	lua_settable (L, -3);

	lua_pushstring (L, "class");
	lua_pushstring (L, "rspamd{cryptobox_signature}");
	lua_rawset (L, -3);

	luaL_register (L, NULL, cryptoboxsignlib_m);
	rspamd_lua_add_preload (L, "rspamd_cryptobox_signature", lua_load_signature);

	luaL_newmetatable (L, "rspamd{cryptobox_hash}");

	lua_pushstring (L, "__index");
	lua_pushvalue (L, -2);
	lua_settable (L, -3);

	lua_pushstring (L, "class");
	lua_pushstring (L, "rspamd{cryptobox_hash}");
	lua_rawset (L, -3);

	luaL_register (L, NULL, cryptoboxhashlib_m);
	rspamd_lua_add_preload (L, "rspamd_cryptobox_hash", lua_load_hash);

	rspamd_lua_add_preload (L, "rspamd_cryptobox", lua_load_cryptobox);

	lua_settop (L, 0);
}
