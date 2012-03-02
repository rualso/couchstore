#include <iostream>
#include <cassert>
#include <sysexits.h>

#include <libcouchstore/couch_db.h>

#include <lua.hpp>

#include "endian.h"

typedef union {
    struct {
        uint64_t cas;
        uint32_t exp;
        uint32_t flags;
    } fields;
    char bytes[];
} revbuf_t;

extern "C" {

    static void push_db(lua_State *ls, Db *db) {
        Db **d = static_cast<Db **>(lua_newuserdata(ls, sizeof(Db*)));
        assert(d);
        *d = db;

        luaL_getmetatable(ls, "couch");
        lua_setmetatable(ls, -2);
    }

    static void push_docinfo(lua_State *ls, DocInfo *docinfo) {
        DocInfo **di = static_cast<DocInfo **>(lua_newuserdata(ls, sizeof(DocInfo*)));
        assert(di);
        *di = docinfo;
        assert(*di);

        luaL_getmetatable(ls, "docinfo");
        lua_setmetatable(ls, -2);
    }

    static DocInfo *getDocInfo(lua_State *ls) {
        DocInfo **d = static_cast<DocInfo**>(luaL_checkudata(ls, 1, "docinfo"));
        assert(d);
        assert(*d);
        return *d;
    }

    static int couch_open(lua_State *ls) {
        if (lua_gettop(ls) < 1) {
            lua_pushstring(ls, "couch.open takes at least one argument: "
                           "\"pathname\" [shouldCreate]");
            lua_error(ls);
            return 1;
        }

        const char *pathname = luaL_checkstring(ls, 1);
        uint64_t flags(0);

        if (lua_gettop(ls) > 1) {
            if (!lua_isboolean(ls, 2)) {
                lua_pushstring(ls, "Second arg must be a boolean, "
                               "true if allowed to create databases.");
                lua_error(ls);
                return 1;
            }
            flags = lua_toboolean(ls, 2) ? COUCH_CREATE_FILES : 0;
        }

        Db *db(NULL);

        int rc = open_db(pathname, flags, NULL, &db);
        if (rc < 0) {
            char buf[256];
            snprintf(buf, sizeof(buf), "error opening DB: %s", describe_error(rc));
            lua_pushstring(ls, buf);
            lua_error(ls);
            return 1;
        }

        push_db(ls, db);

        return 1;
    }

    static Db *getDb(lua_State *ls) {
        Db **d = static_cast<Db**>(luaL_checkudata(ls, 1, "couch"));
        assert(d);
        assert(*d);
        return *d;
    }

    static int couch_close(lua_State *ls) {
        Db *db = getDb(ls);

        if (close_db(db) < 0) {
            lua_pushstring(ls, "error closing database");
            lua_error(ls);
            return 1;
        }
        return 0;
    }

    static int couch_commit(lua_State *ls) {
        Db *db = getDb(ls);

        if (commit_all(db, 0) < 0) {
            lua_pushstring(ls, "error committing");
            lua_error(ls);
            return 1;
        }
        return 0;
    }

    static int couch_get_from_docinfo(lua_State *ls) {
        if (lua_gettop(ls) < 2) {
            lua_pushstring(ls, "couch:get_from_docinfo takes one argument: \"docinfo\"");
            lua_error(ls);
            return 1;
        }

        Db *db = getDb(ls);
        assert(db);
        Doc *doc(NULL);
        lua_remove(ls, 1);
        DocInfo *docinfo = getDocInfo(ls);
        assert(docinfo);

        int rc = open_doc_with_docinfo(db, docinfo, &doc, 0);
        if (rc < 0) {
            char buf[256];
            free_docinfo(docinfo);
            snprintf(buf, sizeof(buf), "error getting doc by docinfo: %s", describe_error(rc));
            lua_pushstring(ls, buf);
            lua_error(ls);
            return 1;
        }

        lua_pushlstring(ls, doc->data.buf, doc->data.size);

        free_doc(doc);

        return 1;
    }

    // couch:get(key) -> string, docinfo
    static int couch_get(lua_State *ls) {
        if (lua_gettop(ls) < 1) {
            lua_pushstring(ls, "couch:get takes one argument: \"key\"");
            lua_error(ls);
            return 1;
        }

        Doc *doc;
        DocInfo *docinfo;
        Db *db = getDb(ls);

        size_t klen;
        // Should be const :/
        char *key = const_cast<char*>(luaL_checklstring(ls, 2, &klen));

        int rc = docinfo_by_id(db, reinterpret_cast<uint8_t*>(key), klen, &docinfo);
        if (rc < 0) {
            char buf[256];
            snprintf(buf, sizeof(buf), "error get docinfo: %s", describe_error(rc));
            lua_pushstring(ls, buf);
            lua_error(ls);
            return 1;
        }

        rc = open_doc_with_docinfo(db, docinfo, &doc, 0);
        if (rc < 0) {
            char buf[256];
            free_docinfo(docinfo);
            snprintf(buf, sizeof(buf), "error get doc by docinfo: %s", describe_error(rc));
            lua_pushstring(ls, buf);
            lua_error(ls);
            return 1;
        }

        lua_pushlstring(ls, doc->data.buf, doc->data.size);
        push_docinfo(ls, docinfo);

        free_doc(doc);

        return 2;
    }

    // couch:delete(key, [rev])
    static int couch_delete(lua_State *ls) {
        if (lua_gettop(ls) < 1) {
            lua_pushstring(ls, "couch:delete takes at least one argument: "
                           "\"key\" [rev_seq]");
            lua_error(ls);
            return 1;
        }

        Doc doc;
        DocInfo docinfo = DOC_INFO_INITIALIZER;

        doc.id.buf = const_cast<char*>(luaL_checklstring(ls, 2, &doc.id.size));
        doc.data.size = 0;
        docinfo.id = doc.id;
        docinfo.deleted = 1;

        if (lua_gettop(ls) > 2) {
            docinfo.rev_seq = luaL_checknumber(ls, 3);
        }

        Db *db = getDb(ls);

        int rc = save_doc(db, &doc, &docinfo, 0);
        if (rc < 0) {
            char buf[256];
            snprintf(buf, sizeof(buf), "error deleting document: %s", describe_error(rc));
            lua_pushstring(ls, buf);
            lua_error(ls);
            return 1;
        }

        return 0;
    }

    // couch:save(key, value, content_meta, [rev_seq], [cas], [exp], [flags]
    static int couch_save(lua_State *ls) {

        if (lua_gettop(ls) < 4) {
            lua_pushstring(ls, "couch:save takes at least three arguments: "
                           "\"key\" \"value\" meta_flags [rev_seq] [cas] [exp] [flags]");
            lua_error(ls);
            return 1;
        }

        Doc doc;
        DocInfo docinfo = DOC_INFO_INITIALIZER;

        revbuf_t revbuf;

        // These really should be const char*
        doc.id.buf = const_cast<char*>(luaL_checklstring(ls, 2, &doc.id.size));
        doc.data.buf = const_cast<char*>(luaL_checklstring(ls, 3, &doc.data.size));
        docinfo.id = doc.id;

        docinfo.content_meta = static_cast<uint8_t>(luaL_checkint(ls, 4));

        if (lua_gettop(ls) > 4) {
            docinfo.rev_seq = luaL_checknumber(ls, 5);
        }

        if (lua_gettop(ls) > 5) {
            revbuf.fields.cas =luaL_checknumber(ls, 6);
            revbuf.fields.cas = endianSwap(revbuf.fields.cas);
        }

        if (lua_gettop(ls) > 6) {
            revbuf.fields.exp = luaL_checklong(ls, 7);
            revbuf.fields.exp = endianSwap(revbuf.fields.exp);
        }

        if (lua_gettop(ls) > 7) {
            revbuf.fields.flags = luaL_checklong(ls, 8);
            revbuf.fields.flags = endianSwap(revbuf.fields.flags);
        }

        docinfo.rev_meta.size = sizeof(revbuf);
        docinfo.rev_meta.buf = revbuf.bytes;

        Db *db = getDb(ls);

        int rc = save_doc(db, &doc, &docinfo, COMPRESS_DOC_BODIES);
        if (rc < 0) {
            char buf[256];
            snprintf(buf, sizeof(buf), "error storing document: %s", describe_error(rc));
            lua_pushstring(ls, buf);
            lua_error(ls);
            return 1;
        }

        return 0;
    }

    static int couch_save_local(lua_State *ls) {
        if (lua_gettop(ls) < 3) {
            lua_pushstring(ls, "couch:save_local takes two arguments: "
                           "\"key\" \"value\"");
            lua_error(ls);
            return 1;
        }

        LocalDoc doc;
        doc.id.buf = const_cast<char*>(luaL_checklstring(ls, 2, &doc.id.size));
        doc.json.buf = const_cast<char*>(luaL_checklstring(ls, 3, &doc.json.size));
        doc.deleted = 0;

        Db *db = getDb(ls);

        int rc = save_local_doc(db, &doc);
        if (rc < 0) {
            char buf[256];
            snprintf(buf, sizeof(buf), "error storing local document: %s",
                     describe_error(rc));
            lua_pushstring(ls, buf);
            lua_error(ls);
            return 1;
        }
        return 0;
    }

    static int couch_delete_local(lua_State *ls) {
        if (lua_gettop(ls) < 2) {
            lua_pushstring(ls, "couch:delete_local takes one argument: \"key\"");
            lua_error(ls);
            return 1;
        }

        LocalDoc doc;
        doc.id.buf = const_cast<char*>(luaL_checklstring(ls, 2, &doc.id.size));
        doc.json.size = 0;
        doc.deleted = 1;

        Db *db = getDb(ls);

        int rc = save_local_doc(db, &doc);
        if (rc < 0) {
            char buf[256];
            snprintf(buf, sizeof(buf), "error deleting local document: %s",
                     describe_error(rc));
            lua_pushstring(ls, buf);
            lua_error(ls);
            return 1;
        }
        return 0;
    }

    // couch:get_local(key) -> val
    static int couch_get_local(lua_State *ls) {
        if (lua_gettop(ls) < 1) {
            lua_pushstring(ls, "couch:get_local takes one argument: \"key\"");
            lua_error(ls);
            return 1;
        }

        LocalDoc *doc;
        Db *db = getDb(ls);

        size_t klen;
        // Should be const :/
        char *key = const_cast<char*>(luaL_checklstring(ls, 2, &klen));


        int rc = open_local_doc(db, reinterpret_cast<uint8_t*>(key), klen, &doc);
        if (rc < 0) {
            char buf[256];
            snprintf(buf, sizeof(buf), "error getting local doc: %s",
                     describe_error(rc));
            lua_pushstring(ls, buf);
            lua_error(ls);
            return 1;
        }

        lua_pushlstring(ls, doc->json.buf, doc->json.size);

        free_local_doc(doc);

        return 1;
    }

    static int luaStringWriter(lua_State *,
                               const void* p,
                               size_t sz,
                               void* ud) {
        std::string *s = static_cast<std::string*>(ud);
        s->append(static_cast<const char*>(p), sz);
        return 0;
    }

    typedef struct {
        lua_State *ls;
        std::string fun;
    } changes_state_t;

    static int couch_changes_each(Db *db, DocInfo *di, void *ctx) {
        changes_state_t *changes_state(static_cast<changes_state_t*>(ctx));
        luaL_loadbuffer(changes_state->ls,
                        changes_state->fun.data(),
                        changes_state->fun.size(),
                        "changes_lambda");

        push_db(changes_state->ls, db);
        push_docinfo(changes_state->ls, di);
        if (lua_pcall(changes_state->ls, 2, 0, 0) != 0) {
            // Not *exactly* sure what to do here.
            std::cerr << "Error running function: "
                      << lua_tostring(changes_state->ls, -1) << std::endl;
        }
        return NO_FREE_DOCINFO;
    }

    // db:changes(function(docinfo) something end, [since])
    static int couch_changes(lua_State *ls) {
                if (lua_gettop(ls) < 3) {
            lua_pushstring(ls, "couch:changes takes two arguments: "
                           "rev_seq, function(docinfo)...");
            lua_error(ls);
            return 1;
        }

        Db *db = getDb(ls);

        changes_state_t changes_state;
        changes_state.ls = ls;

        uint64_t since(luaL_checknumber(ls, 2));

        if (!lua_isfunction(ls, 3)) {
            lua_pushstring(ls, "I need a function to iterate over.");
            lua_error(ls);
            return 1;
        }

        if (lua_dump(ls, luaStringWriter, &changes_state.fun)) {
            size_t rlen;
            const char *m = lua_tolstring(ls, 1, &rlen);
            throw std::string(m, rlen);
        }

        int rc = changes_since(db, since, 0, couch_changes_each, &changes_state);
        if (rc != 0) {
            char buf[128];
            snprintf(buf, sizeof(buf), "error iterating: %s", describe_error(rc));
            lua_pushstring(ls, buf);
            lua_error(ls);
            return 1;
        }

        return 0;
    }

    static const luaL_Reg couch_funcs[] = {
        {"open", couch_open},
        {NULL, NULL}
    };

    static const luaL_Reg couch_methods[] = {
        {"save", couch_save},
        {"delete", couch_delete},
        {"get", couch_get},
        {"get_from_docinfo", couch_get_from_docinfo},
        {"changes", couch_changes},
        {"save_local", couch_save_local},
        {"delete_local", couch_delete_local},
        {"get_local", couch_get_local},
        {"commit", couch_commit},
        {"close", couch_close},
        {NULL, NULL}
    };

    static int docinfo_id(lua_State *ls) {
        DocInfo *di = getDocInfo(ls);
        lua_pushlstring(ls, di->id.buf, di->id.size);
        return 1;
    }

    static int docinfo_db_seq(lua_State *ls) {
        DocInfo *di = getDocInfo(ls);
        lua_pushnumber(ls, di->db_seq);
        return 1;
    }

    static int docinfo_rev_seq(lua_State *ls) {
        DocInfo *di = getDocInfo(ls);
        lua_pushnumber(ls, di->rev_seq);
        return 1;
    }

    static int docinfo_deleted(lua_State *ls) {
        DocInfo *di = getDocInfo(ls);
        lua_pushinteger(ls, di->deleted);
        return 1;
    }

    static int docinfo_content_meta(lua_State *ls) {
        DocInfo *di = getDocInfo(ls);
        lua_pushinteger(ls, di->content_meta);
        return 1;
    }

    static int docinfo_len(lua_State *ls) {
        DocInfo *di = getDocInfo(ls);
        lua_pushinteger(ls, di->size);
        return 1;
    }

    static int docinfo_cas(lua_State *ls) {
        DocInfo *di = getDocInfo(ls);
        if (di->rev_meta.size >= sizeof(revbuf_t)) {
            revbuf_t *rbt(reinterpret_cast<revbuf_t*>(di->rev_meta.buf));
            lua_pushnumber(ls, endianSwap(rbt->fields.cas));
        } else {
            lua_pushnumber(ls, 0);
        }
        return 1;
    }

    static int docinfo_exp(lua_State *ls) {
        DocInfo *di = getDocInfo(ls);
        if (di->rev_meta.size >= sizeof(revbuf_t)) {
            revbuf_t *rbt(reinterpret_cast<revbuf_t*>(di->rev_meta.buf));
            lua_pushnumber(ls, endianSwap(rbt->fields.exp));
        } else {
            lua_pushnumber(ls, 0);
        }
        return 1;
    }

    static int docinfo_flags(lua_State *ls) {
        DocInfo *di = getDocInfo(ls);
        if (di->rev_meta.size >= sizeof(revbuf_t)) {
            revbuf_t *rbt(reinterpret_cast<revbuf_t*>(di->rev_meta.buf));
            lua_pushnumber(ls, endianSwap(rbt->fields.flags));
        } else {
            lua_pushnumber(ls, 0);
        }
        return 1;
    }

    static int docinfo_gc(lua_State *ls) {
        DocInfo *di = getDocInfo(ls);
        free_docinfo(di);
        return 1;
    }

    static const luaL_Reg docinfo_methods[] = {
        {"id", docinfo_id},
        {"rev", docinfo_rev_seq},
        {"db_seq", docinfo_db_seq},
        {"cas", docinfo_cas},
        {"exp", docinfo_exp},
        {"flags", docinfo_flags},
        {"deleted", docinfo_deleted},
        {"content_meta", docinfo_content_meta},
        {"size", docinfo_len},
        {"__len", docinfo_len},
        {"__gc", docinfo_gc},
        {NULL, NULL}
    };

}

static void initCouch(lua_State *ls) {
    luaL_newmetatable(ls, "couch");

    lua_pushstring(ls, "__index");
    lua_pushvalue(ls, -2);  /* pushes the metatable */
    lua_settable(ls, -3);  /* metatable.__index = metatable */

    luaL_openlib(ls, NULL, couch_methods, 0);

    luaL_openlib(ls, "couch", couch_funcs, 0);
}

static void initDocInfo(lua_State *ls) {
    luaL_newmetatable(ls, "docinfo");

    lua_pushstring(ls, "__index");
    lua_pushvalue(ls, -2);  /* pushes the metatable */
    lua_settable(ls, -3);  /* metatable.__index = metatable */

    luaL_openlib(ls, NULL, docinfo_methods, 0);
}


int main(int argc, char **argv) {
    if (argc < 2) {
        std::cerr << "Give me a filename or give me death." << std::endl;
        exit(EX_USAGE);
    }

    lua_State *ls = luaL_newstate();
    luaL_openlibs(ls);

    initCouch(ls);
    initDocInfo(ls);

    int rv(luaL_dofile(ls, argv[1]));
    if (rv != 0) {
        std::cerr << "Error running stuff:  " << lua_tostring(ls, -1) << std::endl;
    }
    return rv;
}
