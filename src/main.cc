#include <cstdlib>
#include <cstring>
#include <cerrno>

#include "platform.hh"
#include "parser.hh"
#include "ast.hh"
#include "lib.hh"
#include "lua.hh"
#include "ffi.hh"

/* sets up the metatable for library, i.e. the individual namespaces
 * of loaded shared libraries as well as the primary C namespace.
 */
struct lib_meta {
    static int gc(lua_State *L) {
        auto *c_ud = lua::touserdata<lib::handle>(L, 1);
        lib::close(*c_ud);
        return 0;
    }

    static int tostring(lua_State *L) {
        auto dl = lua::touserdata<lib::handle>(L, 1);
        if (*dl == lib::load(nullptr, L)) {
            lua_pushfstring(L, "library: default");
        } else {
            lua_pushfstring(L, "library: %p", static_cast<void *>(*dl));
        }
        return 1;
    }

    static int index(lua_State *L) {
        auto dl = *lua::touserdata<lib::handle>(L, 1);
        char const *sname = luaL_checkstring(L, 2);
        auto &ds = ast::decl_store::get_main(L);
        ffi::make_cdata(L, dl, ds.lookup(sname), sname);
        return 1;
    }

    static int newindex(lua_State *L) {
        auto dl = *lua::touserdata<lib::handle>(L, 1);
        char const *sname = luaL_checkstring(L, 2);

        auto &ds = ast::decl_store::get_main(L);
        auto const *decl = ds.lookup(sname);
        if (!decl) {
            luaL_error(
                L, "missing declaration for symbol '%s'", decl->name.c_str()
            );
            return 0;
        }
        if (decl->obj_type() != ast::c_object_type::VARIABLE) {
            luaL_error(L, "symbol '%s' is not mutable", decl->name.c_str());
        }

        void *symp = lib::get_var(dl, sname);
        if (!symp) {
            luaL_error(L, "undefined symbol: %s", sname);
            return 0;
        }

        size_t rsz;
        ffi::lua_check_cdata(
            L, decl->as<ast::c_variable>().type(), symp, 3, rsz
        );
        return 0;
    }

    static void setup(lua_State *L) {
        if (!luaL_newmetatable(L, lua::CFFI_LIB_MT)) {
            luaL_error(L, "unexpected error: registry reinitialized");
        }

        lua_pushcfunction(L, gc);
        lua_setfield(L, -2, "__gc");

        lua_pushcfunction(L, index);
        lua_setfield(L, -2, "__index");

        lua_pushcfunction(L, newindex);
        lua_setfield(L, -2, "__newindex");

        lua_pushcfunction(L, tostring);
        lua_setfield(L, -2, "__tostring");

        lua_setmetatable(L, -2);
        lua_setfield(L, -2, "C");
    }
};

struct fd2 {
    void *sym;
    ffi_cif cif;
    ast::c_value args[];
};

/* used by all kinds of cdata
 *
 * there are several kinds of cdata:
 * - callable cdata (functions)
 * - indexable cdata (pointers, arrays)
 * - value cdata (primitives)
 */
struct cdata_meta {
    static int gc(lua_State *L) {
        auto &cd = ffi::tocdata<ffi::fdata>(L, 1);
        if (cd.gc_ref != LUA_REFNIL) {
            lua_rawgeti(L, LUA_REGISTRYINDEX, cd.gc_ref); /* the finalizer */
            lua_pushvalue(L, 1); /* the cdata */
            if (lua_pcall(L, 1, 0, 0)) {
                lua_pop(L, 1);
            }
        }
        if (cd.decl.closure() && cd.val.cd) {
            /* this is O(n) which sucks a little */
            cd.val.cd->refs.remove(&cd.val.cd);
        }
        using T = ast::c_type;
        cd.decl.~T();
        return 0;
    }

    static int tostring(lua_State *L) {
        auto &cd = ffi::tocdata<ast::c_value>(L, 1);
        auto const *tp = &cd.decl;
        ast::c_value const *val = &cd.val;
        if (tp->type() == ast::C_BUILTIN_REF) {
            tp = &tp->ptr_base();
            val = static_cast<ast::c_value const *>(cd.val.ptr);
        }
        /* 64-bit integers */
        /* XXX: special printing for lua builds with non-double numbers? */
        if (tp->integer() && (tp->libffi_type()->size == 8)) {
            char buf[32];
            int written;
            if (tp->is_unsigned()) {
                written = snprintf(buf, sizeof(buf), "%lluULL", val->ull);
            } else {
                written = snprintf(buf, sizeof(buf), "%lldLL", val->ll);
            }
            lua_pushlstring(L, buf, written);
            return 1;
        }
        auto s = cd.decl.serialize();
        lua_pushfstring(L, "cdata<%s>: %p", s.c_str(), cd.get_addr());
        return 1;
    }

    static int call(lua_State *L) {
        auto &fd = ffi::tocdata<ffi::fdata>(L, 1);
        switch (fd.decl.type()) {
            case ast::C_BUILTIN_FPTR:
            case ast::C_BUILTIN_FUNC:
                break;
            default: {
                auto s = fd.decl.serialize();
                luaL_error(L, "'%s' is not callable", s.c_str());
                break;
            }
        }
        if (fd.decl.closure() && !fd.val.cd) {
            luaL_error(L, "bad callback");
        }
        return ffi::call_cif(fd, L);
    }

    template<typename F>
    static void index_common(lua_State *L, F &&func) {
        auto &cd = ffi::tocdata<ast::c_value>(L, 1);
        /* TODO: add arrays */
        switch (cd.decl.type()) {
            case ast::C_BUILTIN_PTR:
            case ast::C_BUILTIN_REF:
                break;
            default: {
                auto s = cd.decl.serialize();
                luaL_error(L, "'%s' is not indexable", s.c_str());
                break;
            }
        }
        auto sidx = luaL_checkinteger(L, 2);
        luaL_argcheck(L, sidx >= 0, 2, "index is negative");
        auto *ptr = reinterpret_cast<unsigned char *>(cd.val.ptr);
        auto *type = cd.decl.ptr_base().libffi_type();
        func(cd, static_cast<void *>(&ptr[sidx * type->size]));
    }

    static int cb_free(lua_State *L) {
        auto &cd = ffi::checkcdata<ffi::fdata>(L, 1);
        luaL_argcheck(L, cd.decl.closure(), 1, "not a callback");
        if (!cd.val.cd) {
            luaL_error(L, "bad callback");
        }
        delete cd.val.cd;
        return 0;
    }

    static int cb_set(lua_State *L) {
        auto &cd = ffi::checkcdata<ffi::fdata>(L, 1);
        luaL_argcheck(L, cd.decl.closure(), 1, "not a callback");
        if (!cd.val.cd) {
            luaL_error(L, "bad callback");
        }
        if (!lua_isfunction(L, 2)) {
            lua::type_error(L, 2, "function");
        }
        luaL_unref(L, LUA_REGISTRYINDEX, cd.val.cd->fref);
        lua_pushvalue(L, 2);
        cd.val.cd->fref = luaL_ref(L, LUA_REGISTRYINDEX);
        return 0;
    }

    static int index(lua_State *L) {
        auto &cd = ffi::tocdata<ast::c_value>(L, 1);
        if (cd.decl.closure()) {
            /* callbacks have some methods */
            char const *mname = lua_tostring(L, 2);
            /* if we had more methods, we'd do a table */
            if (!strcmp(mname, "free")) {
                lua_pushcfunction(L, cb_free);
                return 1;
            } else if (!strcmp(mname, "set")) {
                lua_pushcfunction(L, cb_set);
                return 1;
            } else if (!mname) {
                luaL_error(
                    L, "'%s' cannot be indexed with '%s'",
                    cd.decl.serialize().c_str(),
                    lua_typename(L, lua_type(L, 2))
                );
            } else {
                luaL_error(
                    L, "'%s' has no member named '%s'",
                    cd.decl.serialize().c_str(), mname
                );
            }
            return 0;
        }
        index_common(L, [L](auto &pcd, void *val) {
            ffi::lua_push_cdata(L, pcd.decl.ptr_base(), val);
        });
        return 1;
    }

    static int newindex(lua_State *L) {
        index_common(L, [L](auto &cd, void *val) {
            size_t rsz;
            ffi::lua_check_cdata(L, cd.decl.ptr_base(), val, 3, rsz);
        });
        return 0;
    }

    static void setup(lua_State *L) {
        if (!luaL_newmetatable(L, lua::CFFI_CDATA_MT)) {
            luaL_error(L, "unexpected error: registry reinitialized");
        }

        lua_pushcfunction(L, gc);
        lua_setfield(L, -2, "__gc");

        lua_pushcfunction(L, call);
        lua_setfield(L, -2, "__call");

        lua_pushcfunction(L, index);
        lua_setfield(L, -2, "__index");

        lua_pushcfunction(L, newindex);
        lua_setfield(L, -2, "__newindex");

        lua_pushcfunction(L, tostring);
        lua_setfield(L, -2, "__tostring");

        lua_pop(L, 1);
    }
};

/* represents lua-side type definition */
struct ctype_meta {
    static int gc(lua_State *L) {
        auto &decl = *lua::touserdata<ast::c_type>(L, 1);
        using T = ast::c_type;
        decl.~T();
        return 0;
    }

    static int tostring(lua_State *L) {
        auto &decl = *lua::touserdata<ast::c_type>(L, 1);
        lua_pushfstring(L, "ctype<%s>", decl.serialize().c_str());
        return 1;
    }

    static int call(lua_State *L) {
        auto &decl = *lua::touserdata<ast::c_type>(L, 1);
        switch (decl.type()) {
            case ast::C_BUILTIN_FUNC:
                luaL_argcheck(
                    L, false, 1, "function types cannot be instantiated"
                );
                break;
            default: {
                break;
            }
        }
        ast::c_value stor{};
        void *cdp = nullptr;
        int fref = LUA_REFNIL;
        size_t rsz = 0;
        if (lua_gettop(L) >= 2) {
            if ((decl.type() == ast::C_BUILTIN_FPTR) && lua_isfunction(L, 2)) {
                lua_pushvalue(L, 2);
                fref = luaL_ref(L, LUA_REGISTRYINDEX);
            } else {
                cdp = ffi::lua_check_cdata(L, decl, &stor, 2, rsz);
            }
        } else {
            memset(&stor, 0, sizeof(stor));
            cdp = &stor;
            rsz = sizeof(stor);
        }
        if (decl.type() == ast::C_BUILTIN_FPTR) {
            if (fref == LUA_REFNIL) {
                if (ffi::iscdata(L, 2)) {
                    /* special handling for closures */
                    auto &cd = ffi::tocdata<ffi::fdata>(L, 2);
                    if (cd.decl.closure()) {
                        ffi::make_cdata_func(
                            L, nullptr, decl.function(), decl.type(), cd.val.cd
                        );
                        return 1;
                    }
                }
                using FP = void (*)();
                ffi::make_cdata_func(
                    L, *reinterpret_cast<FP *>(cdp), decl.function(),
                    decl.type()
                );
            } else {
                ffi::make_cdata_func(L, nullptr, decl.function(), decl.type());
                auto &cd = ffi::tocdata<ffi::fdata>(L, -1);
                cd.val.cd->fref = fref;
            }
        } else {
            auto &cd = ffi::newcdata<ast::c_value>(L, decl);
            memcpy(&cd.val, cdp, rsz);
        }
        return 1;
    }

    static void setup(lua_State *L) {
        if (!luaL_newmetatable(L, lua::CFFI_CTYPE_MT)) {
            luaL_error(L, "unexpected error: registry reinitialized");
        }

        lua_pushcfunction(L, gc);
        lua_setfield(L, -2, "__gc");

        lua_pushcfunction(L, call);
        lua_setfield(L, -2, "__call");

        lua_pushcfunction(L, tostring);
        lua_setfield(L, -2, "__tostring");

        lua_pop(L, 1);
    }
};

/* the ffi module itself */
struct ffi_module {
    static int cdef_f(lua_State *L) {
        parser::parse(L, luaL_checkstring(L, 1));
        return 0;
    }

    /* either gets a ctype or makes a ctype from a string */
    static ast::c_type const &check_ct(lua_State *L, int idx) {
        if (luaL_testudata(L, idx, lua::CFFI_CTYPE_MT)) {
            lua_pushvalue(L, idx);
        } else {
            auto *ud = lua::newuserdata<ast::c_type>(L);
            new (ud) ast::c_type{
                parser::parse_type(L, luaL_checkstring(L, idx))
            };
            lua::mark_ctype(L);
        }
        return *lua::touserdata<ast::c_type>(L, -1);
    }

    static int new_f(lua_State *L) {
        /* stack: <first arg> <other args> */
        int nargs = lua_gettop(L);
        /* first arg: ctype */
        check_ct(L, 1);
        /* stack: <first arg> <other args> <ctype> */
        lua_insert(L, 2);
        /* stack: <first arg> <ctype> <other args> */
        lua_call(L, nargs - 1, 1);
        /* stack: <first arg> <retval> */
        return 1;
    }

    static int load_f(lua_State *L) {
        char const *path = luaL_checkstring(L, 1);
        bool glob = (lua_gettop(L) >= 2) && lua_toboolean(L, 2);
        auto *c_ud = lua::newuserdata<lib::handle>(L);
        *c_ud = lib::load(path, L, glob);
        return 1;
    }

    static int typeof_f(lua_State *L) {
        auto *ud = lua::newuserdata<ast::c_type>(L);
        if (ffi::iscdata(L, 1)) {
            new (ud) ast::c_type{ffi::tocdata<ast::c_value>(L, 1).decl};
        } else {
            new (ud) ast::c_type{
                parser::parse_type(L, luaL_checkstring(L, 1))
            };
        }
        lua::mark_ctype(L);
        return 1;
    }

    static int gc_f(lua_State *L) {
        auto &cd = ffi::checkcdata<ast::c_value>(L, 1);
        if (lua_isnil(L, 2)) {
            /* if nil and there is an existing finalizer, unset */
            if (cd.gc_ref != LUA_REFNIL) {
                luaL_unref(L, LUA_REGISTRYINDEX, cd.gc_ref);
                cd.gc_ref = LUA_REFNIL;
            }
        } else {
            /* new finalizer can be any type, it's pcall'd */
            lua_pushvalue(L, 2);
            cd.gc_ref = luaL_ref(L, LUA_REGISTRYINDEX);
        }
        lua_pushvalue(L, 1); /* return the cdata */
        return 1;
    }

    static int sizeof_f(lua_State *L) {
        auto &ct = check_ct(L, 1);
        lua_pushinteger(L, ct.libffi_type()->size);
        return 1;
    }

    static int alignof_f(lua_State *L) {
        auto &ct = check_ct(L, 1);
        lua_pushinteger(L, ct.libffi_type()->alignment);
        return 1;
    }

    static int istype_f(lua_State *L) {
        auto &ct = check_ct(L, 1);
        if (!ffi::iscdata(L, 2)) {
            lua_pushboolean(L, false);
            return 1;
        }
        auto &cd = ffi::tocdata<ast::c_value>(L, 2);
        if (ct.type() == ast::C_BUILTIN_STRUCT) {
            /* if ct is a struct, accept pointers/refs to the struct */
            /* TODO: also applies to union */
            auto ctp = cd.decl.type();
            if ((ctp == ast::C_BUILTIN_PTR) || (ctp == ast::C_BUILTIN_REF)) {
                lua_pushboolean(L, ct.is_same(cd.decl.ptr_base(), true));
                return 1;
            }
        }
        lua_pushboolean(L, ct.is_same(cd.decl, true));
        return 1;
    }

    static int errno_f(lua_State *L) {
        int cur = errno;
        if (lua_gettop(L) >= 1) {
            errno = int(luaL_checkinteger(L, 1));
        }
        lua_pushinteger(L, cur);
        return 1;
    }

    static int string_f(lua_State *L) {
        if (!ffi::iscdata(L, 1)) {
            lua_pushfstring(
                L, "cannot convert '%s' to 'char const *'",
                luaL_typename(L, 1)
            );
            luaL_argcheck(L, false, 1, lua_tostring(L, -1));
        }
        /* FIXME: check argument type conversions */
        auto &ud = ffi::tocdata<ast::c_value>(L, 1);
        if (lua_gettop(L) <= 1) {
            lua_pushstring(L, static_cast<char const *>(ud.val.ptr));
        } else {
            lua_pushlstring(
                L, static_cast<char const *>(ud.val.ptr),
                size_t(luaL_checkinteger(L, 2))
            );
        }
        return 1;
    }

    /* FIXME: type conversions (constness etc.) */
    static void *check_voidptr(lua_State *L, int idx) {
        if (ffi::iscdata(L, idx)) {
            auto &cd = ffi::tocdata<ast::c_value>(L, idx);
            auto ctp = cd.decl.type();
            if ((ctp != ast::C_BUILTIN_PTR) && (ctp != ast::C_BUILTIN_REF)) {
                lua_pushfstring(
                    L, "cannot convert '%s' to 'void *'",
                    cd.decl.serialize().c_str()
                );
                luaL_argcheck(L, false, idx, lua_tostring(L, -1));
            }
            return cd.val.ptr;
        } else if (lua_isuserdata(L, idx)) {
            return lua_touserdata(L, idx);
        }
        lua_pushfstring(
            L, "cannot convert '%s' to 'void *'",
            luaL_typename(L, 1)
        );
        luaL_argcheck(L, false, idx, lua_tostring(L, -1));
        return nullptr;
    }

    /* FIXME: lengths (and character) in these APIs may be given by cdata... */

    static int copy_f(lua_State *L) {
        void *dst = check_voidptr(L, 1);
        void const *src;
        size_t len;
        if (lua_isstring(L, 2)) {
            src = lua_tostring(L, 2);
            if (lua_gettop(L) <= 2) {
                len = lua_rawlen(L, 2);
            } else {
                len = size_t(luaL_checkinteger(L, 3));
            }
        } else {
            src = check_voidptr(L, 2);
            len = size_t(luaL_checkinteger(L, 3));
        }
        memcpy(dst, src, len);
        return 0;
    }

    static int fill_f(lua_State *L) {
        void *dst = check_voidptr(L, 1);
        size_t len = size_t(luaL_checkinteger(L, 2));
        int byte = int(luaL_optinteger(L, 3, 0));
        memset(dst, byte, len);
        return 0;
    }

    static ast::c_value &new_scalar(lua_State *L, int cbt, std::string name) {
        auto &cd = ffi::newcdata<ast::c_value>(
            L, ast::c_type{std::move(name), cbt, 0}
        );
        return cd.val;
    }

    static int tonumber_f(lua_State *L) {
        if (ffi::iscdata(L, 1)) {
            auto &cd = ffi::tocdata<ast::c_value>(L, 1);
            ast::c_type const *tp = &cd.decl;
            void *val = &cd.val;
            int btp = cd.decl.type();
            if (btp == ast::C_BUILTIN_REF) {
                tp = &cd.decl.ptr_base();
                btp = tp->type();
                val = cd.val.ptr;
            }
            if (tp->scalar()) {
                ffi::lua_push_cdata(L, *tp, val, true);
                return 1;
            }
            switch (btp) {
                case ast::C_BUILTIN_PTR:
                case ast::C_BUILTIN_FPTR:
                case ast::C_BUILTIN_STRUCT:
                case ast::C_BUILTIN_FUNC:
                    /* these may appear */
                    lua_pushnil(L);
                    return 1;
                default:
                    /* these should not */
                    assert(false);
                    lua_pushnil(L);
                    return 1;
            }
        } else {
            lua_pushvalue(L, lua_upvalueindex(1));
            lua_insert(L, 1);
            lua_call(L, lua_gettop(L) - 1, LUA_MULTRET);
            return lua_gettop(L);
        }
        assert(false);
        return 0;
    }

    static int toretval_f(lua_State *L) {
        auto &cd = ffi::checkcdata<ast::c_value>(L, 1);
        ffi::lua_push_cdata(L, cd.decl, &cd.val);
        return 1;
    }

    static int eval_f(lua_State *L) {
        /* TODO: accept expressions */
        char const *str = luaL_checkstring(L, 1);
        parser::lex_token_u outv;
        auto v = parser::parse_number(L, outv, str, str + lua_rawlen(L, 1));
        switch (v) {
            case ast::c_expr_type::INT:
                new_scalar(L, ast::C_BUILTIN_INT, "int").i = outv.i;
                break;
            case ast::c_expr_type::UINT:
                new_scalar(L, ast::C_BUILTIN_UINT, "unsigned int").u = outv.u;
                break;
            case ast::c_expr_type::LONG:
                new_scalar(L, ast::C_BUILTIN_LONG, "long").l = outv.l;
                break;
            case ast::c_expr_type::ULONG:
                new_scalar(
                    L, ast::C_BUILTIN_ULONG, "unsigned long"
                ).ul = outv.ul;
                break;
            case ast::c_expr_type::LLONG:
                new_scalar(L, ast::C_BUILTIN_LLONG, "long long").ll = outv.ll;
                break;
            case ast::c_expr_type::ULLONG:
                new_scalar(
                    L, ast::C_BUILTIN_ULLONG, "unsigned long long"
                ).ull = outv.ull;
                break;
            default:
                luaL_error(L, "NYI");
                break;
        }
        return 1;
    }

    static int type_f(lua_State *L) {
        if (lua_isuserdata(L, 1)) {
            lua_getmetatable(L, 1);
            lua_getfield(L, LUA_REGISTRYINDEX, lua::CFFI_CDATA_MT);
            lua_getfield(L, LUA_REGISTRYINDEX, lua::CFFI_CTYPE_MT);
            if (lua_rawequal(L, -2, -3) || lua_rawequal(L, -1, -3)) {
                lua_pop(L, 3);
                lua_pushliteral(L, "cdata");
                return 1;
            }
            lua_pop(L, 3);
        }
        luaL_checkany(L, 1);
        lua_pushstring(L, luaL_typename(L, 1));
        return 1;
    }

    static int abi_f(lua_State *L) {
        luaL_checkstring(L, 1);
        lua_pushvalue(L, 1);
        lua_rawget(L, lua_upvalueindex(1));
        if (lua_isnil(L, -1)) {
            lua_pop(L, 1);
            lua_pushboolean(L, false);
        }
        return 1;
    }

    static void setup_abi(lua_State *L) {
        lua_newtable(L);
        lua_pushboolean(L, true);
#if FFI_WORDSIZE == 64
        lua_setfield(L, -2, "64bit");
#elif FFI_WORDSIZE == 32
        lua_setfield(L, -2, "32bit");
#elif FFI_WORDSIZE == 16
        lua_setfield(L, -2, "16bit");
#else
        lua_setfield(L, -2, "8bit");
#endif
        lua_pushboolean(L, true);
#ifdef FFI_BIG_ENDIAN
        lua_setfield(L, -2, "be");
#else
        lua_setfield(L, -2, "le");
#endif
#ifdef FFI_WINDOWS_ABI
        lua_pushboolean(L, true);
        lua_setfield(L, -2, "win");
#endif
#ifdef FFI_ARM_EABI
        lua_pushboolean(L, true);
        lua_setfield(L, -2, "eabi");
#endif
#if FFI_ARCH == FFI_ARCH_PPC64 && defined(_CALL_ELF) && _CALL_ELF == 2
        lua_pushboolean(L, true);
        lua_setfield(L, -2, "elfv2");
#endif
#if FFI_ARCH_HAS_FPU == 1
        lua_pushboolean(L, true);
        lua_setfield(L, -2, "fpu");
#endif
        lua_pushboolean(L, true);
#if FFI_ARCH_SOFTFP == 1
        lua_setfield(L, -2, "softfp");
#else
        lua_setfield(L, -2, "hardfp");
#endif
    }

    static void setup(lua_State *L) {
        static luaL_Reg const lib_def[] = {
            /* core */
            {"cdef", cdef_f},
            {"load", load_f},

            /* data handling */
            {"new", new_f},
            {"typeof", typeof_f},
            {"gc", gc_f},

            /* type info */
            {"sizeof", sizeof_f},
            {"alignof", alignof_f},
            {"istype", istype_f},

            /* utilities */
            {"errno", errno_f},
            {"string", string_f},
            {"copy", copy_f},
            {"fill", fill_f},
            {"toretval", toretval_f},
            {"eval", eval_f},
            {"type", type_f},

            {NULL, NULL}
        };
        luaL_newlib(L, lib_def);

        lua_pushliteral(L, FFI_OS_NAME);
        lua_setfield(L, -2, "os");

        lua_pushliteral(L, FFI_ARCH_NAME);
        lua_setfield(L, -2, "arch");

        setup_abi(L);
        lua_pushcclosure(L, abi_f, 1);
        lua_setfield(L, -2, "abi");

        lua_getglobal(L, "tonumber");
        lua_pushcclosure(L, tonumber_f, 1);
        lua_setfield(L, -2, "tonumber");
    }

    static void setup_dstor(lua_State *L) {
        /* our declaration storage is a userdata in the registry */
        auto *ud = lua::newuserdata<ast::decl_store>(L);
        new (ud) ast::decl_store{};
        /* stack: dstor */
        lua_newtable(L);
        /* stack: dstor, mt */
        lua_pushcfunction(L, [](lua_State *LL) -> int {
            using T = ast::decl_store;
            auto *ds = lua::touserdata<T>(LL, 1);
            ds->~T();
            return 0;
        });
        /* stack: dstor, mt, __gc */
        lua_setfield(L, -2, "__gc");
        /* stack: dstor, __mt */
        lua_setmetatable(L, -2);
        /* stack: dstor */
        lua_setfield(L, LUA_REGISTRYINDEX, lua::CFFI_DECL_STOR);
        /* stack: empty */
    }

    static void open(lua_State *L) {
        setup_dstor(L); /* declaration store */

        setup(L); /* push table to stack */

        /* lib handles */
        auto *c_ud = lua::newuserdata<lib::handle>(L);
        *c_ud = lib::load(nullptr, L, false);
        lib_meta::setup(L);

        /* cdata handles */
        cdata_meta::setup(L);

        /* ctype handles */
        ctype_meta::setup(L);
    }
};

extern "C" int luaopen_cffi(lua_State *L) {
    ffi_module::open(L);
    return 1;
}
