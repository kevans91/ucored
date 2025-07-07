/*-
 * Copyright (c) 2025 Kyle Evans <kevans@FreeBSD.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <sys/types.h>
#include <sys/stat.h>
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <regex.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <assert.h>
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>

#include "ucored.h"

/*
 * Preserve the nouchg and nodump flag; I can't see much reason anyone
 * would want to set the others off-hand.
 */
#define	UCORED_FFLAGS_PRESERVED \
    (UF_IMMUTABLE | UF_NODUMP)

/* Just a light wrapper for our ucore. */
struct luaucore {
	struct ucored_client	*cl;
};

enum ucore_uvalues {
	UCV_ATTRS = 1,

	UCV_NVALS,
};

#define	UCORED_UCOREHANDLE	"ucored_ucore_t"
#define	UCORED_REGEXHANDLE	"ucored_regex_t"
static lua_State *ucored_state;

/* Core module */
static int
ucored_lua_logit(lua_State *L, int priority)
{
	const char *logstr;

	logstr = luaL_checkstring(L, 1);
	ucored_log(priority, "%s", logstr);

	lua_pushboolean(L, 1);
	return (1);
}

static int
ucored_lua_debug(lua_State *L)
{
	return (ucored_lua_logit(L, LOG_DEBUG));
}

static int
ucored_lua_error(lua_State *L)
{
	return (ucored_lua_logit(L, LOG_ERR));
}

static int
ucored_lua_info(lua_State *L)
{
	return (ucored_lua_logit(L, LOG_INFO));
}

static int
ucored_lua_isdir(lua_State *L)
{
	struct stat sb;
	const char *path;

	path = luaL_checkstring(L, 1);
	if (stat(path, &sb) == -1) {
		lua_pushboolean(L, 0);
		return (1);
	}

	lua_pushboolean(L, S_ISDIR(sb.st_mode));
	return (1);
}

static int
ucored_lua_mkpath(lua_State *L)
{
	struct stat sb;
	const char *patharg;
	char *path, *walker;
	int serrno;
	bool failed = false;

	patharg = luaL_checkstring(L, 1);
	if (stat(patharg, &sb) == 0 && S_ISDIR(sb.st_mode)) {
		lua_pushboolean(L, 1);
		return (1);
	}

	path = strdup(patharg);
	if (path == NULL) {
		luaL_pushfail(L);
		lua_pushstring(L, strerror(ENOMEM));
		return (2);
	}

	/* Skip any leading path separators. */
	walker = path;
	while (*walker == '/' && *walker != '\0')
		walker++;

	while (*walker != '\0') {
		char orig;

		walker = strchrnul(walker, '/');
		orig = *walker;
		*walker = '\0';

		if (mkdir(path, 0755) == -1) {
			int serrno = errno;

			/*
			 * If we already know it was something else, just drop
			 * it.
			 */
			if (errno != EEXIST) {
				failed = true;
				break;
			}

			/*
			 * If it did exist, we failed if it wasn't a directory
			 * or if can't tell that it wasn't a directory.
			 */
			if (stat(path, &sb) == -1) {
				failed = true;
				errno = serrno;
				break;
			}

			if (!S_ISDIR(sb.st_mode)) {
				failed = true;
				errno = ENOTDIR;
				break;
			}
		}

		if (orig == '\0')
			break;

		*walker = orig;
		walker++;
	}

	serrno = errno;
	free(path);

	if (failed) {
		luaL_pushfail(L);
		lua_pushstring(L, strerror(errno));
		return (2);
	}

	lua_pushboolean(L, 1);
	return (1);
}

static int
ucored_lua_regcomp(lua_State *L)
{
	const char *pattern;
	regex_t *regex;
	int error;

	pattern = luaL_checkstring(L, 1);

	regex = lua_newuserdata(L, sizeof(*regex));
	luaL_setmetatable(L, UCORED_REGEXHANDLE);

	if ((error = regcomp(regex, pattern, REG_EXTENDED)) != 0) {
		char errbuf[64];

		(void)regerror(error, regex, errbuf, sizeof(errbuf));

		/* Pop the regex_t */
		lua_pop(L, 1);

		luaL_pushfail(L);
		lua_pushstring(L, errbuf);
		return (2);
	}

	return (1);
}

#define	REG_SIMPLE(n)	{ #n, ucored_lua_ ## n }
static const struct luaL_Reg corelib[] = {
	REG_SIMPLE(debug),
	REG_SIMPLE(error),
	REG_SIMPLE(info),
	REG_SIMPLE(isdir),
	REG_SIMPLE(mkpath),
	REG_SIMPLE(regcomp),
	{ NULL, NULL },
};

static int
ucored_regex_error(lua_State *L, regex_t *self, int error)
{
	char errbuf[64];

	(void)regerror(error, self, errbuf, sizeof(errbuf));

	luaL_pushfail(L);
	lua_pushstring(L, errbuf);
	return (2);
}

static int
ucored_regex_find(lua_State *L)
{
	const char *subject;
	regex_t *self;
	regmatch_t match;
	int error;

	self = luaL_checkudata(L, 1, UCORED_REGEXHANDLE);
	subject = luaL_checkstring(L, 2);

	error = regexec(self, subject, 1, &match, 0);
	if (error != 0) {
		if (error == REG_NOMATCH) {
			lua_pushnil(L);
			return (1);
		}

		return (ucored_regex_error(L, self, error));
	}

	/*
	 * Lua's strings are one-indexed, so we bump rm_so by 1.  rm_eo is
	 * actually the the character just *after* the match, so we'll just take
	 * that as-is rather than - 1 + 1.
	 */
	lua_pushnumber(L, match.rm_so + 1);
	lua_pushnumber(L, match.rm_eo);
	return (2);
}

static int
ucored_regex_close(lua_State *L)
{
	regex_t *self;

	self = luaL_checkudata(L, 1, UCORED_REGEXHANDLE);
	regfree(self);
	return (0);
}

#define	REGEX_SIMPLE(n)	{ #n, ucored_regex_ ## n }
static const luaL_Reg ucored_regex[] = {
	REGEX_SIMPLE(find),
	{ NULL, NULL },
};

static const luaL_Reg ucored_regex_meta[] = {
	{ "__index", NULL },	/* Set during registration */
	{ "__gc", ucored_regex_close },
	{ "__close", ucored_regex_close },
	{ NULL, NULL },
};

/* ucore methods */
static const uint8_t *
ucored_ucore_strfetch_value(lua_State *L, struct luaucore *self,
    enum ucore_data_type type)
{
	struct ucored_client_data *sd;

	SLIST_FOREACH(sd, &self->cl->cl_datasegs, cl_entry) {
		if (sd->cl_data.ud_hdr.uhdr_type == type)
			return (&sd->cl_data.ud_data[0]);
	}

	return (NULL);
}

static int
ucored_ucore_strfetch(lua_State *L, struct luaucore *self,
    enum ucore_data_type type, const char *dflt)
{
	const char *str;

	str = (const char *)ucored_ucore_strfetch_value(L, self, type);
	if (str == NULL && dflt != NULL)
		str = dflt;
	if (str == NULL)
		lua_pushnil(L);
	else
		lua_pushstring(L, (char *)str);
	return (1);
}

static int
ucored_ucore_attributes(lua_State *L)
{
	struct luaucore *self;
	int type;

	self = luaL_checkudata(L, 1, UCORED_UCOREHANDLE);
	type = lua_getiuservalue(L, 1, UCV_ATTRS);
	assert(type == LUA_TTABLE);
	return (1);
}

static int
ucored_ucore_comm(lua_State *L)
{
	struct luaucore *self;

	self = luaL_checkudata(L, 1, UCORED_UCOREHANDLE);
	return (ucored_ucore_strfetch(L, self, UDT_COMM, NULL));
}

static int
ucored_ucore_jail(lua_State *L)
{
	struct luaucore *self;

	self = luaL_checkudata(L, 1, UCORED_UCOREHANDLE);
	return (ucored_ucore_strfetch(L, self, UDT_JAIL, ""));
}

static bool
ucored_copy_file(int fromfd, int tofd, off_t fsize)
{
	off_t copied = 0;
	off_t fromoff = 0, tooff = 0;

	while (copied < fsize) {
		ssize_t ret;

		ret = copy_file_range(fromfd, &fromoff, tofd, &tooff,
		    fsize, 0);
		if (ret == -1) {
			if (errno == EINTR)
				continue;

			return (false);
		} else if (ret == 0) {
			/*
			 * Truncation is maybe sketchy, but we'll give it a
			 * pass anyways.
			 */
			return (true);
		}

		/* Excellent, making progress. */
		copied += ret;
	}

	return (true);
}

static int
ucored_ucore_filename(lua_State *L)
{
	struct luaucore *self;
	const char *corefile, *delim;

	self = luaL_checkudata(L, 1, UCORED_UCOREHANDLE);
	corefile = (const char *)ucored_ucore_strfetch_value(L, self, UDT_PATH);

	delim = strrchr(corefile, '/');
	if (delim != NULL)
		corefile = delim + 1;

	lua_pushstring(L, corefile);
	return (1);
}

static int
ucored_ucore_move(lua_State *L)
{
	struct stat sb;
	struct timespec ts[2];
	struct luaucore *self;
	const char *corepath, *path;
	off_t coresize;
	int fromfd, serrno, tofd;
	fflags_t fflags;
	uid_t uid;
	gid_t gid;

	fromfd = tofd = -1;
	self = luaL_checkudata(L, 1, UCORED_UCOREHANDLE);
	corepath = (const char *)ucored_ucore_strfetch_value(L, self, UDT_PATH);
	path = luaL_checkstring(L, 2);

	/*
	 * We will only attempt a rename if the destination just does not exist.
	 * If it does, we need to do further evaluation to decide if this is a
	 * potential security issue or not.
	 */
	if (lstat(path, &sb) == -1 && errno == ENOENT) {
		/*
		 * Attempt to rename first; maybe we'll get lucky.
		 */
		if (rename(corepath, path) == 0) {
			lua_pushboolean(L, 1);
			return (1);
		} else if (errno != EXDEV) {
			goto err;
		}
	}

	/*
	 * If the problem is just that it's a cross-fs copy that is ineligible
	 * for rename(2), we'll set it up for copy_file_range(2) + unlink(2)
	 * instead.
	 */
	fromfd = open(corepath, O_RDONLY | O_NOFOLLOW);
	if (fromfd == -1) {
		/*
		 * Symlinks are all kinds of security issues, so we'll always
		 * raise a red flag if we feel like we're getting tricked into
		 * something.  We access paths all across the system, so one
		 * must consider the possibility of an attacker with some
		 * knowledge of the system ucored(8) configuration finding some
		 * trick to pull.
		 */
		if (errno == EMLINK) {
			luaL_pushfail(L);
			lua_pushstring(L,
			    "security: refusing to follow source symlink");
			return (2);
		}

		goto err;
	}

	if (fstat(fromfd, &sb) == -1)
		goto err;

	fflags = sb.st_flags & UCORED_FFLAGS_PRESERVED;
	uid = sb.st_uid;
	gid = sb.st_gid;
	coresize = sb.st_size;
	ts[0].tv_nsec = UTIME_OMIT;
	ts[1] = sb.st_mtim;

	tofd = open(path, O_WRONLY | O_NOFOLLOW | O_CREAT | O_EXCL, sb.st_mode);
	if (tofd == -1 && errno == EEXIST) {
		/*
		 * If the file already exists, we may be OK to just unlink it
		 * and write a new one in its place.  We just want to take care
		 * to be a good citizen and avoid clobbering someone else's
		 * core -- otherwise, we'd just blindly unlink it before.
		 */
		if (lstat(path, &sb) == -1)
			goto err;

		if (S_ISLNK(sb.st_mode)) {
			luaL_pushfail(L);
			lua_pushstring(L,
			    "security: refusing to follow destination symlink");
			return (2);
		} else if (sb.st_uid != uid) {
			luaL_pushfail(L);
			lua_pushstring(L,
			    "security: refusing to clobber another user's core");
			return (2);
		}

		(void)unlink(path);

		/*
		 * We'll only allow the above once though.  If something does
		 * recreate the destination in between that unlink and our
		 * creation, we're not going to try again in case someone's
		 * trying to pull something.
		 */
		tofd = open(path, O_WRONLY | O_NOFOLLOW | O_CREAT | O_EXCL,
		    sb.st_mode);
	}

	if (tofd == -1)
		goto err;

	/*
	 * Fix permissions.  We'll make a best-effort attempt to preserve *some*
	 * flags from the source file (see UCORED_FFLAGS_PRESERVED above).
	 */
	if (fchown(tofd, uid, gid) == -1)
		goto err;
	if (futimens(tofd, ts) == -1)
		goto err;
	if (fflags != 0 && fchflags(tofd, fflags) == -1)
		goto err;

	if (!ucored_copy_file(fromfd, tofd, coresize))
		goto err;

	lua_pushboolean(L, 1);
	return (1);
err:
	serrno = errno;

	if (fromfd >= 0)
		close(fromfd);
	if (tofd >= 0)
		close(tofd);

	luaL_pushfail(L);
	lua_pushstring(L, strerror(serrno));
	return (2);
}

static int
ucored_ucore_path(lua_State *L)
{
	struct luaucore *self;

	self = luaL_checkudata(L, 1, UCORED_UCOREHANDLE);
	return (ucored_ucore_strfetch(L, self, UDT_PATH, NULL));
}

#define	UCORE_SIMPLE(n)	{ #n, ucored_ucore_ ## n }
static const luaL_Reg ucored_ucore[] = {
	UCORE_SIMPLE(attributes),
	UCORE_SIMPLE(comm),
	UCORE_SIMPLE(jail),
	UCORE_SIMPLE(filename),
	UCORE_SIMPLE(move),
	UCORE_SIMPLE(path),
	{ NULL, NULL },
};

static const luaL_Reg ucored_ucore_meta[] = {
	{ "__index", NULL },	/* Set during registration */
	{ NULL, NULL },
};

static int
luaopen_core(lua_State *L)
{

	luaL_newlib(L, corelib);

	luaL_newmetatable(L, UCORED_REGEXHANDLE);
	luaL_setfuncs(L, ucored_regex_meta, 0);

	luaL_newlibtable(L, ucored_regex);
	luaL_setfuncs(L, ucored_regex, 0);
	lua_setfield(L, -2, "__index");
	lua_pop(L, 1);

	luaL_newmetatable(L, UCORED_UCOREHANDLE);
	luaL_setfuncs(L, ucored_ucore_meta, 0);

	luaL_newlibtable(L, ucored_ucore);
	luaL_setfuncs(L, ucored_ucore, 0);
	lua_setfield(L, -2, "__index");
	lua_pop(L, 1);

	return (1);
}

bool
ucored_lua_init(void)
{

	setenv("LUA_PATH", UCORED_LUAPATH, 1);

	ucored_state = luaL_newstate();
	if (ucored_state == NULL) {
		warnx("luaL_newstate: out of memory");
		return (false);
	}

	luaL_openlibs(ucored_state);
	luaL_requiref(ucored_state, "core", luaopen_core, 0);
	lua_pop(ucored_state, 1);

	if (luaL_dofile(ucored_state, UCORED_LUAFILE) != LUA_OK) {
		const char *err;

		err = lua_tostring(ucored_state, -1);
		if (err == NULL)
			err = "unknown";

		warnx("%s\n", err);
		return (false);
	}

	/*
	 * The handler should be left on the stack from ucored.lua's return
	 * value.
	 */
	return (true);
}

static void
ucored_lua_push_ucore(struct ucored_client *cl)
{
	struct luaucore *lucore;
	struct ucore *ucore;

	/* Setup our ucore arg. */
	lucore = lua_newuserdatauv(ucored_state, sizeof(*lucore), UCV_NVALS - 1);
	luaL_setmetatable(ucored_state, UCORED_UCOREHANDLE);
	lucore->cl = cl;

	/* UCV_ATTRS */
	ucore = &cl->cl_hdr;
	lua_newtable(ucored_state);
	lua_pushinteger(ucored_state, ucore->ucore_signo);
	lua_setfield(ucored_state, -2, "signal");
	lua_pushinteger(ucored_state, ucore->ucore_jid);
	lua_setfield(ucored_state, -2, "jid");
	lua_pushinteger(ucored_state, ucore->ucore_ppid);
	lua_setfield(ucored_state, -2, "ppid");
	lua_pushinteger(ucored_state, ucore->ucore_pid);
	lua_setfield(ucored_state, -2, "pid");

	lua_setiuservalue(ucored_state, -2, UCV_ATTRS);
}

bool
ucored_lua_handle(struct ucored_client *cl)
{
	bool ok;

	/* Copy the handler that ucored.lua left on the stack. */
	lua_pushvalue(ucored_state, -1);

	ucored_lua_push_ucore(cl);
	if (lua_pcall(ucored_state, 1, 1, 0)) {
		const char *err;

		err = lua_tostring(ucored_state, -1);
		if (err == NULL)
			err = "unknown";

		ucored_log(LOG_ERR, "%s\n", err);
		lua_pop(ucored_state, 1);
		return (false);
	}

	ok = lua_toboolean(ucored_state, -1);
	lua_pop(ucored_state, 1);
	return (ok);
}
