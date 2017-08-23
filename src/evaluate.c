#define _GNU_SOURCE 1
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <stdarg.h>
#include <string.h>
#include <dlfcn.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>

#include "evaluate.h"
#include "function_call.h"
#include "ptr_call.h"
#include "cfileparser.h"
#include "typename.h"
#include "gc.h"

static tree cur_ctx = NULL;

/* This holds the evaluator state after the evaluation of any #include
 * directives.  */
static tree include_ctx = NULL;

/* This is a chain of CPP_INCLUDE tree objects that is used to keep
 * track of all #include directives seen.  See `eval_cpp_include' for
 * more info. */
static tree include_chain = NULL;

static const char *current_filename;

static tree __evaluate_1(tree t, int depth);
static tree __evaluate(tree t, int depth);

enum identifier_search_scope
{
    SCOPE_CURRENT_CTX,
    SCOPE_GLOBAL
};

/* Context management functions. */
static void pop_ctx(void)
{
    if (!tPARENT_CTX(cur_ctx)) {
        /* We attempted to pop off the top-level context.  This is a
         * serious error. */
        fprintf(stderr, "Error: attempted to pop top-level context\n");
        exit(EXIT_FAILURE);
    }

    cur_ctx = tPARENT_CTX(cur_ctx);
}

static void push_ctx(const char *name)
{
    tree new_ctx = tree_make(E_CTX);

    if (!new_ctx) {
        perror("Error creating new context");
        exit(EXIT_FAILURE);
    }

    tPARENT_CTX(new_ctx) = cur_ctx;
    tCTX_NAME(new_ctx) = name;
    tID_MAP(new_ctx) = tree_make(CHAIN_HEAD);
    tALLOC_CHAIN(new_ctx) = tree_make(CHAIN_HEAD);

    cur_ctx = new_ctx;
}

static void ctx_backtrace(void)
{
    fprintf(stderr, "Evaluator Backtrace\n===================\n");
    tree_dump(cur_ctx);
}

void __attribute__((noreturn)) eval_die(tree t, const char *format, ...)
{
    fprintf(stderr, "%s:%zu:%zu: error: ", current_filename, t->locus.line_no,
            t->locus.column_no);
    va_list args;
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);

    ctx_backtrace();

    exit(EXIT_FAILURE);
}

static tree resolve_id(tree id, tree idmap)
{
    tree i;

    for_each_tree(i, idmap) {
        if (strcmp(tID_STR(tEMAP_LEFT(i)), tID_STR(id)) == 0)
            return tEMAP_RIGHT(i);
    }

    return NULL;
}

static tree resolve_identifier(tree id,
                               enum identifier_search_scope scope)
{
    tree search_ctx = cur_ctx;

    if (scope == SCOPE_CURRENT_CTX)
        return resolve_id(id, tID_MAP(search_ctx));

    /* Search through the identifier mappings of the current context
     * stack to find the appropriate object. */
    while (search_ctx) {
        tree search_result = resolve_id(id, tID_MAP(search_ctx));

        if (search_result)
            return search_result;

        search_ctx = tPARENT_CTX(search_ctx);
    }

    /* Also search the include_ctx if it exists. */
    if (include_ctx) {
        tree result = resolve_id(id, tID_MAP(include_ctx));

        if (result)
            return result;
    }

    return NULL;
}

static void __map_identifer(tree id, tree t, tree idmap)
{
    tree new_map = tree_make(E_MAP);

    if (!is_T_IDENTIFIER(id))
        eval_die(id, "Attempted to map non-identifier\n");

    tEMAP_LEFT(new_map) = id;
    tEMAP_RIGHT(new_map) = t;

    tree_chain(new_map, idmap);
}

static size_t get_size_of_type(tree type, int depth)
{
    if (is_T_DECL_COMPOUND(type) && tCOMP_DECL_SZ(type) != 0)
        /* Since the evaluation of a T_DECL_COMPOUND has possible side
         * affects (mapping itself into the current context), check to
         * see if it already knows it's size before evaluating a
         * sizeof() expression. */
        return tCOMP_DECL_SZ(type);
    else {
        tree sz, szof = tree_make(T_SIZEOF);
        tSZOF_EXP(szof) = type;
        sz = __evaluate_1(szof, depth);
        return mpz_get_ui(tINT(sz));
    }
}

static void map_identifier(tree id, tree t)
{
    if (resolve_identifier(id, SCOPE_CURRENT_CTX))
        eval_die(id, "Attempted to map already existing identifier %s.\n",
                 tID_STR(id));

    __map_identifer(id, t, tID_MAP(cur_ctx));
}

static tree eval_identifier(tree t, int depth)
{
    /* Search through the identifier mappings of the current context
     * stack to find the appropriate object. */
    tree object = resolve_identifier(t, SCOPE_GLOBAL);

    if (object)
        return object;

    eval_die(t, "Could not resolve identifier %s.\n",
             tID_STR(t));
}

static tree make_live_var(tree type)
{
    tree live_var = tree_make(T_LIVE_VAR);

    tLV_TYPE(live_var) = type;
    tLV_VAL(live_var) = GC_MALLOC(sizeof(*tLV_VAL(live_var)));

    return live_var;
}

static tree make_int_from_live_var(tree var)
{
    tree ret = tree_make(T_INTEGER);
    tree type = tLV_TYPE(var);

    switch (TYPE(type)) {
#define SETINT(type)                                                    \
        case type:                                                      \
            mpz_init_set_si(tINT(ret), tLV_VAL(var)->type);             \
            break;
        SETINT(D_T_CHAR);
        SETINT(D_T_SHORT);
        SETINT(D_T_INT);
        SETINT(D_T_LONG);
        SETINT(D_T_LONGLONG);
        SETINT(D_T_UCHAR);
        SETINT(D_T_USHORT);
        SETINT(D_T_UINT);
        SETINT(D_T_ULONG);
        SETINT(D_T_ULONGLONG);
#undef SETINT
    default:
        eval_die(var, "Could not create integer type from live var.");
    }

    return ret;
}

#define DEFCTYPE(tname, desc, ctype, fmt)                               \
    static inline tree convert_int_to_##tname(tree intval)              \
    {                                                                   \
        tree dest_type = tree_make(tname);                              \
        tree ret = make_live_var(dest_type);                            \
        if (!is_T_INTEGER(intval))                                      \
            eval_die(intval, "attempted to convert from non-integer\n"); \
        tLV_VAL(ret)->tname = (ctype)mpz_get_si(tINT(intval));          \
        return ret;                                                     \
    }
#include "ctypes.def"
#undef DEFCTYPE

static tree cast_live_var_to_type(tree dest_type, tree live_var)
{
    tree ret, ival = make_int_from_live_var(live_var);

    switch (TYPE(dest_type)) {
#define DEFCTYPE(tname, desc, ctype, fmt)                       \
        case tname:                                             \
            ret = convert_int_to_##tname(ival);                 \
            break;
#include "ctypes.def"
#undef DEFCTYPE
        default:
            eval_die(live_var, "attempted to cast to non C type");
        }

    return ret;
}

static tree make_fncall_result(tree type, ptrdiff_t result)
{
    tree ret;

    if (!type)
        return NULL;

    if (is_D_T_VOID(type))
        return NULL;

    ret = make_live_var(type);

    switch (TYPE(type))
    {
#define DEFCTYPE(TNAME, DESC, CTYPE, FMT)             \
        case TNAME:                                   \
            tLV_VAL(ret)->TNAME = (CTYPE)result;      \
            break;
#include "ctypes.def"
#undef DEFCTYPE
    default:
        eval_die(type, "Could not create function return value\n");
    }

    return ret;
}

static tree eval_fn_args(tree args, int depth)
{
    tree arg, ret = tree_make(CHAIN_HEAD);

    for_each_tree(arg, args) {
        tree fn_arg = tree_make(T_FN_ARG);
        tFNARG_EXP(fn_arg) = __evaluate_1(arg, depth + 1);
        tree_chain(fn_arg, ret);
    }

    return ret;
}

static tree eval_fn_call(tree t, int depth)
{
    /* There are four possibilities here:
     *
     * 1: The function call is to a /defined/ function, i.e. one that
     *    has been parsed and has a corresponding T_FN_DEF tree.  If
     *    that is the case, evaluate all arguments and evaluate the
     *    function body.
     *
     * 2: The function call is an external symbol, i.e. the function
     *    should have been declared and should have a corresponding
     *    T_DECL_FN.  If that is the case, evaluate the arguments and
     *    use the symbolic linker to find the function.
     *
     * 3: The function evaluates to a live variable.  If this is the
     *    case, we ensure that the live variable is a function
     *    pointer, evaluate the arguments and call the address pointed
     *    to by that live variable.
     *
     * 4: The function couldn't be found.  In that case, error.
     */
    tree function = __evaluate_1(tFNCALL_ID(t), depth + 1);

    if (is_T_FN_DEF(function)) {
        tree arg_decls = tFNDEF_ARGS(function),
            arg_vals = tFNCALL_ARGS(t),
            arg_decl, arg_val, return_val;

        push_ctx(tID_STR(tFNDEF_NAME(function)));

        if (arg_decls) {
            size_t no_decls = 0, no_vals = 0;
            /* Ensure that the number of parameters passed to the
             * function matches the number in the function
             * declaration. */
            for_each_tree(arg_decl, arg_decls)
                no_decls++;

            for_each_tree(arg_val, arg_vals)
                no_vals++;

            if (no_vals != no_decls)
                eval_die(t, "Invalid number of parameters to function");

            /* Evaluate an assignment for each passed value. */
            arg_val = arg_vals;

            for_each_tree(arg_decl, arg_decls) {
                tree eval_arg_val,
                    decl_identifier;

                arg_val = list_entry(arg_val->chain.next,
                                     typeof(*arg_val), chain);

                /* We need to evaluate the argument value before
                 * adding the declaration on to the new context.  This
                 * prevents bugs with things such as:
                 *
                 * 0: int foo(int a)
                 * 1: {
                 * 2:      use(a);
                 * 3: }
                 * 4:
                 * 5: int main()
                 * 6: {
                 * 7:     int a = 10;
                 * 8:     foo(a);
                 * 9: }
                 *
                 * Notice that if we evaluated 'int a' on line 0
                 * first, then we evaluate the assignment 'a = a' the
                 * lhs would evaluate to the newly declared 'a' value.
                 * Therefore, if we evaluate 'a' on line 8 before the
                 * declaration on line 0, we obtain the correct lvalue
                 * for assignment. */
                eval_arg_val = __evaluate_1(arg_val, depth + 1);
                decl_identifier = __evaluate_1(arg_decl, depth + 1);

                __evaluate_1(tree_build_bin(T_ASSIGN, decl_identifier, eval_arg_val),
                             depth + 1);
            }
        }

        return_val = __evaluate(tFNDEF_STMTS(function), depth + 1);

        pop_ctx();

        return return_val;
    }

    if (is_T_DECL_FN(function)) {
        tree fn_arg_chain = NULL, args = tFNCALL_ARGS(t);
        char *function_name = tID_STR(tFNDECL_NAME(function));
        ptrdiff_t res;
        void *function_address = dlsym(RTLD_DEFAULT, function_name);

        if (function_address == NULL)
            eval_die(t, "Could not resolve external symbol: %s\n", function_name);

        /* Evaluate all arguments before passing into the marshalling
         * function. */
        fn_arg_chain = eval_fn_args(args, depth);

         res = do_call(function_address, fn_arg_chain);

         return make_fncall_result(tFNDECL_RET_TYPE(function), res);
    }

    if (is_T_LIVE_VAR(function)) {
        tree fn_arg_chain, args = tFNCALL_ARGS(t),
            live_var_type = tLV_TYPE(function),
            function_type;
        ptrdiff_t res;

        if (!is_D_T_PTR(live_var_type))
            eval_die(t, "could not call non-pointer type\n");

        function_type = tDTPTR_EXP(live_var_type);

        if (!is_T_DECL_FN(function_type))
            eval_die(t, "could not call non-function pointer type\n");

        /* Evaluate all arguments before passing into the marshalling
         * function. */
        fn_arg_chain = eval_fn_args(args, depth);

        res = do_call(tLV_VAL(function)->D_T_PTR, fn_arg_chain);

        return make_fncall_result(tFNDECL_RET_TYPE(function_type), res);

    }

    return NULL;
}

static tree eval_fn_def(tree t, int depth)
{
    map_identifier(tFNDEF_NAME(t), t);

    return NULL;
}

static void make_and_map_live_var(tree id, tree type)
{
    assert(TYPE(id) == T_IDENTIFIER);

    if (is_E_INCOMP_TYPE(type))
        eval_die(id, "Can not create incomplete type %s\n",
                 tID_STR(id));

    map_identifier(id, make_live_var(type));
}

static size_t get_array_size(tree array_decl, tree base_type, int depth)
{
    size_t sz_of_each_element = get_size_of_type(base_type, depth), no_elms;
    tree no_elements = __evaluate_1(tARRAY_SZ(array_decl), depth + 1);

    if (is_T_LIVE_VAR(no_elements))
        no_elements = make_int_from_live_var(no_elements);

    if (!is_T_INTEGER(no_elements))
        eval_die(array_decl, "Attempted to create array with non-constant size\n");

    no_elms = mpz_get_ui(tINT(no_elements));

    return sz_of_each_element * no_elms;
}

static tree instantiate_array(tree array_decl, tree base_type, void *base,
                              size_t length)
{
    tree live_var, ptr = tree_make(D_T_PTR),
        id = tARRAY_ID(array_decl);

    if (!is_T_IDENTIFIER(id))
        eval_die(array_decl, "Unknown array name type\n");

    /* An array is basically a pointer; it has no bounds checking.
     * Therefore all we need to do is create a live var with a pointer
     * type. */
    tDTPTR_EXP(ptr) = base_type;
    live_var = make_live_var(ptr);
    tLV_VAL(live_var)->D_T_PTR = base;

    /* These are used by the sizeof() expression to return the correct
     * size. */
    tLV_IS_ARRAY(live_var) = 1;
    tLV_ARRAY_SZ(live_var) = length;

    return live_var;
}

static tree instantiate_struct(tree struct_decl, int depth, void *base);

static void handle_struct_decl(tree decl, tree live_struct, int depth)
{
    tree live_element,
        decl_type = __evaluate_1(tDECL_TYPE(decl), depth + 1),
        decl_element = tDECL_DECLS(decl);

    void *base = tLV_COMP_BASE(live_struct);

    resolve_ptr_type(&decl_element, &decl_type);

    if (is_T_ARRAY(decl_element)) {
        size_t array_sz = get_array_size(decl_element, decl_type, depth);
        live_element = instantiate_array(decl_element, decl_type,
                                         base + tDECL_OFFSET(decl),
                                         array_sz);

        __map_identifer(tARRAY_ID(decl_element), live_element,
                        tLV_COMP_MEMBERS(live_struct));
        return;
    }

    if (is_CTYPE(decl_type)) {
        live_element = tree_make(T_LIVE_VAR);
        tLV_TYPE(live_element) = decl_type;
        tLV_VAL(live_element) = base + tDECL_OFFSET(decl);
        __map_identifer(decl_element, live_element,
                        tLV_COMP_MEMBERS(live_struct));
        return;
    }

    if (is_T_DECL_COMPOUND(decl_type)) {
        live_element = instantiate_struct(decl_type, depth,
                                          base + tDECL_OFFSET(decl));

        __map_identifer(decl_element, live_element,
                        tLV_COMP_MEMBERS(live_struct));

        return;
    }

    eval_die(decl, "Unknown structure member decl\n");
}

static tree instantiate_struct(tree struct_decl, int depth, void *base)
{
    tree i, live_struct = tree_make(T_LIVE_COMPOUND);

    tLV_COMP_DECL(live_struct) = struct_decl;
    tLV_COMP_BASE(live_struct) = base;
    tLV_COMP_MEMBERS(live_struct) = tree_make(CHAIN_HEAD);

    for_each_tree(i, tCOMP_DECL_DECLS(struct_decl))
        handle_struct_decl(i, live_struct, depth);

    return live_struct;
}

static tree alloc_struct(tree struct_decl, int depth)
{
    void *base = GC_MALLOC(tCOMP_DECL_SZ(struct_decl));

    return instantiate_struct(struct_decl, depth, base);
}

static tree alloc_array(tree array_decl, tree base_type, int depth)
{
    size_t array_sz = get_array_size(array_decl, base_type, depth);
    void *array_mem = GC_MALLOC(array_sz);
    tree live_var = instantiate_array(array_decl, base_type, array_mem,
                                      array_sz);

    return live_var;
}

static tree handle_decl(tree decl, tree base_type, int depth)
{
    tree decl_type = base_type;

    /* Strip off any pointer objects and add them to the base type. */
    resolve_ptr_type(&decl, &decl_type);

    if (is_T_DECL_COMPOUND(decl_type) && is_T_IDENTIFIER(decl)) {
        map_identifier(decl, alloc_struct(decl_type, depth));
        return decl;
    }

    if (is_T_ARRAY(decl)) {
        tree live_var = alloc_array(decl, decl_type, depth);
        tree id = tARRAY_ID(decl);

        /* We know that since the array was successfully instantiated,
         * the left element of the decl *must* be an identifier. */
        map_identifier(id, live_var);
        return id;
    }

    switch (TYPE(decl)) {
    case T_IDENTIFIER:
        make_and_map_live_var(decl, decl_type);
        return decl;
    case T_DECL_FN:
        tFNDECL_RET_TYPE(decl) = decl_type;
        map_identifier(tFNDECL_NAME(decl), decl);
        return decl;
    case T_ASSIGN:
    {
        tree ret = handle_decl(tASSIGN_LHS(decl), base_type, depth);
        __evaluate_1(decl, depth + 1);
        return ret;
    }
    default:
        eval_die(decl, "Unknown rvalue in declaration.\n");
    }
}

static tree map_typedef(tree id, tree type)
{
    resolve_ptr_type(&id, &type);

    if (is_T_DECL_FN(id)) {
        tree fndecl = id;
        id = tFNDECL_NAME(fndecl);
        tFNDECL_RET_TYPE(fndecl) = type;
        type = fndecl;
    }

    if (!is_T_IDENTIFIER(id))
        eval_die(id, "Attempted to map type to non-identifier\n");

    map_identifier(id, type);

    return id;
}

static tree handle_extern_fn(tree return_type, tree fndecl)
{
    void *func_addr;
    tree live_var, id, func_ptr_type, previous_decl;

    /* We handle an extern fn decl by creating a pointer to the
     * function decl, so that eval_fn_call will call the address after
     * evaluating the arguments. */

    /* We allow extern functions to be declared more than once, see if
     * this has already been declared, if so, just return the
     * identifier. */

    id = tFNDECL_NAME(fndecl);

    previous_decl = resolve_identifier(id, SCOPE_CURRENT_CTX);

    if (is_T_LIVE_VAR(previous_decl)) {
        tree live_var_type = tLV_TYPE(previous_decl);

        if (!is_D_T_PTR(live_var_type))
            eval_die(fndecl, "attempted to re-declare %s as different type",
                     tID_STR(id));

        if (!is_T_DECL_FN(tDTPTR_EXP(live_var_type)))
            eval_die(fndecl, "attempted to re-declare %s as different type",
                     tID_STR(id));

        return id;
    }

    tFNDECL_RET_TYPE(fndecl) = return_type;

    func_ptr_type = tree_make(D_T_PTR);
    tDTPTR_EXP(func_ptr_type) = fndecl;

    if (!is_T_IDENTIFIER(id))
        eval_die(fndecl, "attempted to extern function that isn't an "
                 "identifier\n");

    func_addr = dlsym(RTLD_DEFAULT, tID_STR(id));

    if (!func_addr)
        eval_die(fndecl, "Could not resolve external function %s\n",
                 tID_STR(id));

    live_var = make_live_var(func_ptr_type);
    tLV_VAL(live_var)->D_T_PTR = func_addr;

    map_identifier(id, live_var);

    return id;
}

static tree handle_extern_decl(tree extern_type, tree decl)
{
    tree live_var, id;
    void *sym_addr;

    resolve_ptr_type(&decl, &extern_type);

    id = decl;

    if (is_T_DECL_FN(decl))
        return handle_extern_fn(extern_type, decl);

    if (!is_T_IDENTIFIER(id))
        eval_die(decl, "attempted to extern something that isn't an "
                 "identifier\n");

    sym_addr = dlsym(RTLD_DEFAULT, tID_STR(id));

    if (!sym_addr)
        eval_die(decl, "Could not resolve extern symbol %s\n",
                 tID_STR(id));

    live_var = tree_make(T_LIVE_VAR);
    tLV_TYPE(live_var) = extern_type;
    tLV_VAL(live_var) = sym_addr;

    map_identifier(id, live_var);

    return id;
}

static tree handle_extern(tree extern_type, tree decls, int depth)
{
    tree ret;
    tree type = __evaluate_1(tEXTERN_EXP(extern_type), depth +1), i;

    if (is_CHAIN_HEAD(decls))
        for_each_tree(i, decls)
            ret = handle_extern_decl(type, i);
    else
        ret = handle_extern_decl(type, decls);

    return ret;
}

static tree handle_typedef(tree typedef_type, tree decls, int depth)
{
    tree ret;
    tree type = __evaluate_1(tTYPEDEF_EXP(typedef_type), depth + 1), i;

    if (is_CHAIN_HEAD(decls))
        for_each_tree(i, decls)
            ret = map_typedef(i, type);
    else
        ret = map_typedef(decls, type);

    return ret;
}

static tree handle_forward_decl(tree type)
{
    tree id;

    if (is_T_STRUCT(type))
        id = tSTRUCT_EXP(type);
    else if(is_T_UNION(type))
        id = tUNION_EXP(type);
    else
        eval_die(type, "unknown forward decl type\n");

    /* If the declaration already exists, don't attempt to map it
     * again. */
    if (resolve_identifier(id, SCOPE_CURRENT_CTX))
        return id;

    tree forward_decl = tree_make(E_INCOMP_TYPE);

    map_identifier(id, forward_decl);

    return id;
}

static tree handle_static_decl(tree decl, int depth)
{
    tree base_type = tSTATIC_EXP(tDECL_TYPE(decl)),
        decls = tDECL_DECLS(decl),
        i,
        ret;

    /* This function will be called the very first time this static
     * decl is encountered.  We will change the type of tree object
     * here from a T_DECL to an E_CTX so subsequent evaluations of
     * this object result in the static objects being found from the
     * map (see eval_evaluator_ctx).
     *
     * For this to work, we push a temporary context, evaluate all
     * decls and retain the id_map and alloc chain.  We then use them
     * to construct the E_CTX object. */

    push_ctx("Static declaration");
    if (is_CHAIN_HEAD(decls))
        for_each_tree(i, decls)
            ret = handle_decl(i, base_type, depth);
    else
        ret = handle_decl(decls, base_type, depth);

    TYPE(decl) = E_CTX;

    tID_MAP(decl) = tID_MAP(cur_ctx);
    tPARENT_CTX(decl) = NULL;
    tALLOC_CHAIN(decl) = tALLOC_CHAIN(cur_ctx);
    tCTX_NAME(decl) = "Static declaration";
    tIS_STATIC(decl) = 1;

    pop_ctx();

    __evaluate_1(decl, depth + 1);

    return ret;
}

static tree eval_decl(tree t, int depth)
{
    tree base_type = tDECL_TYPE(t),
        decls = tDECL_DECLS(t),
        i, ret;

    if (!decls && (is_T_STRUCT(base_type) || is_T_UNION(base_type)))
        return handle_forward_decl(base_type);

    base_type = __evaluate_1(tDECL_TYPE(t), depth + 1);

    if (is_T_TYPEDEF(base_type))
        return handle_typedef(base_type, decls, depth + 1);

    if (is_T_EXTERN(base_type))
        return handle_extern(base_type, decls, depth + 1);

    if (is_T_STATIC(base_type))
        return handle_static_decl(t, depth + 1);

    if (!decls)
        return NULL;

    if (is_CHAIN_HEAD(decls))
        for_each_tree(i, decls)
            ret = handle_decl(i, base_type, depth);
    else
        ret = handle_decl(decls, base_type, depth);

    return ret;
}

static void assign_integer(tree var, tree right)
{
    signed long int val;

    /* Obtain the value with which to assign. */
    switch (TYPE(right))
    {
    case T_INTEGER:
         val = mpz_get_si(tINT(right));
         break;
    case T_LIVE_VAR:
        val = tLV_VAL(right)->D_T_LONG;
        break;
    default:
        eval_die(right, "unknown rvalue assignment to integer.\n");
    }

    switch (TYPE(tLV_TYPE(var))) {
        #define DEFCTYPE(TNAME, DESC, CTYPE, FMT)        \
            case TNAME:                                  \
                tLV_VAL(var)->TNAME = (CTYPE)val; \
                break;
        #include "ctypes.def"
        #undef DEFCTYPE
    default:
        eval_die(var, "could not assign to non-integer type");
    }
}

static void assign_float(tree var, tree right)
{
    double val;

    /* Obtain the value with which to assign. */
    switch (TYPE(right))
    {
    case T_INTEGER:
        val = (double)mpz_get_si(tINT(right));
         break;
    case T_FLOAT:
        val = (double)mpf_get_d(tFLOAT(right));
        break;
    case T_LIVE_VAR:
        val = tLV_VAL(right)->D_T_DOUBLE;
        break;
    default:
        eval_die(right, "unknown rvalue assignment to float.\n");
    }

    switch (TYPE(tLV_TYPE(var))) {
    case D_T_FLOAT:
        tLV_VAL(var)->D_T_FLOAT = (float)val;
        break;
    case D_T_DOUBLE:
        tLV_VAL(var)->D_T_DOUBLE = val;
        break;
    default:
        eval_die(var, "could not assign to non-float type");
    }
}

static void assign_ptr(tree var, tree right)
{
    void *ptr;

    switch (TYPE(right))
    {
    case T_STRING:
        ptr = tSTRING(right);
        break;
    case T_LIVE_VAR:
        ptr = (void *)tLV_VAL(right)->D_T_PTR;
        break;
    case T_INTEGER:
        ptr = (void *)mpz_get_ui(tINT(right));
        break;
    default:
        eval_die(right, "Could not assign to non-pointer type");
    }

    tLV_VAL(var)->D_T_PTR = ptr;
}

static tree eval_assign(tree t, int depth)
{
    tree left = __evaluate_1(tASSIGN_LHS(t), depth + 1);
    tree right = __evaluate_1(tASSIGN_RHS(t), depth + 1);

    /* Ensure we have a valid lvalue. */
    if (!is_T_LIVE_VAR(left))
        eval_die(t, "Not a valid lvalue.\n");

    switch (TYPE(tLV_TYPE(left))) {
    case D_T_CHAR ... D_T_ULONGLONG:
        assign_integer(left, right);
        break;
    case D_T_FLOAT ... D_T_DOUBLE:
        assign_float(left, right);
        break;
    case D_T_PTR:
        assign_ptr(left, right);
        break;
    default:
        eval_die(t, "Unknown assignment rvalue type.");
    }

    return NULL;
}

static void live_var_add(tree var, unsigned long int val)
{
    tree type = tLV_TYPE(var);
    switch (TYPE(type)) {
#define DEFCTYPE(TNAME, DESC, CTYPE, FMT)               \
        case TNAME:                                     \
            tLV_VAL(var)->TNAME += val;   \
            break;
#include "ctypes.def"
#undef DEFCTYPE
    default:
        eval_die(var, "Could not add to unknown live var type.");
    }
}

static void live_var_sub(tree var, unsigned long int val)
{
    tree type = tLV_TYPE(var);
    switch (TYPE(type)) {
#define DEFCTYPE(TNAME, DESC, CTYPE, FMT)               \
        case TNAME:                                     \
            tLV_VAL(var)->TNAME -= val;   \
            break;
#include "ctypes.def"
#undef DEFCTYPE
    default:
        eval_die(var, "Could not subtract to unknown live var type.");
    }
}

static tree eval_post_inc(tree t, int depth)
{
    tree ret;
    tree exp = __evaluate_1(tPINC_EXP(t), depth + 1);
    if (!is_T_LIVE_VAR(exp))
        eval_die(t, "Not a valid lvalue.\n");

    ret = make_int_from_live_var(exp);

    live_var_add(exp, 1);

    return ret;
}

static tree eval_inc(tree t, int depth)
{
    tree ret;
    tree exp = __evaluate_1(tINC_EXP(t), depth + 1);
    if (!is_T_LIVE_VAR(exp))
        eval_die(t, "Not a valid lvalue.\n");

    live_var_add(exp, 1);

    ret = make_int_from_live_var(exp);

    return ret;
}

static tree eval_post_dec(tree t, int depth)
{
    tree ret;
    tree exp = __evaluate_1(tPDEC_EXP(t), depth + 1);
    if (!is_T_LIVE_VAR(exp))
        eval_die(t, "Not a valid lvalue.\n");

    ret = make_int_from_live_var(exp);
    live_var_sub(exp, 1);
    return ret;
}

static tree eval_dec(tree t, int depth)
{
    tree ret;
    tree exp = __evaluate_1(tDEC_EXP(t), depth + 1);
    if (!is_T_LIVE_VAR(exp))
        eval_die(t, "Not a valid lvalue.\n");

    live_var_sub(exp, 1);
    ret = make_int_from_live_var(exp);
    return ret;
}

static tree eval_add(tree t, int depth)
{
    tree left = __evaluate_1(tADD_LHS(t), depth + 1);
    tree right = __evaluate_1(tADD_RHS(t), depth + 1);

    tree ret = tree_make(T_INTEGER);
    mpz_init(tINT(ret));

    /* Resolve all identifiers. */
    if (is_T_LIVE_VAR(left))
        left = make_int_from_live_var(left);

    if (is_T_LIVE_VAR(right))
        right = make_int_from_live_var(right);

    if (!(is_T_INTEGER(left) && is_T_INTEGER(right)))
        eval_die(t, "Could not add to non integer type\n");

    mpz_add(tINT(ret), tINT(left), tINT(right));

    return ret;
}

static tree eval_sub(tree t, int depth)
{
    tree left = __evaluate_1(tSUB_LHS(t), depth + 1);
    tree right = __evaluate_1(tSUB_RHS(t), depth + 1);

    tree ret = tree_make(T_INTEGER);
    mpz_init(tINT(ret));

    if (is_T_LIVE_VAR(left))
        left = make_int_from_live_var(left);

    if (is_T_LIVE_VAR(right))
        right = make_int_from_live_var(right);

    if (!(is_T_INTEGER(left) && is_T_INTEGER(right)))
        eval_die(t, "Could not subtract to non integer type\n");

    mpz_sub(tINT(ret), tINT(left), tINT(right));

    return ret;
}

static tree eval_mul(tree t, int depth)
{
    tree left = __evaluate_1(tMUL_LHS(t), depth + 1);
    tree right = __evaluate_1(tMUL_RHS(t), depth + 1);


    tree ret = tree_make(T_INTEGER);
    mpz_init(tINT(ret));

    if (is_T_LIVE_VAR(left))
        left = make_int_from_live_var(left);

    if (is_T_LIVE_VAR(right))
        right = make_int_from_live_var(right);

    if (!(is_T_INTEGER(left) && is_T_INTEGER(right)))
        eval_die(t, "Could not subtract to non integer type\n");

    mpz_mul(tINT(ret), tINT(left), tINT(right));

    return ret;
}

static tree eval_div(tree t, int depth)
{
    tree left = __evaluate_1(tDIV_LHS(t), depth + 1);
    tree right = __evaluate_1(tDIV_RHS(t), depth + 1);


    tree ret = tree_make(T_INTEGER);
    mpz_init(tINT(ret));

    if (is_T_LIVE_VAR(left))
        left = make_int_from_live_var(left);

    if (is_T_LIVE_VAR(right))
        right = make_int_from_live_var(right);

    if (!(is_T_INTEGER(left) && is_T_INTEGER(right)))
        eval_die(t, "Could not subtract to non integer type\n");

    mpz_div(tINT(ret), tINT(left), tINT(right));

    return ret;
}

static tree convert_to_comparable_type(tree t, int depth)
{
    tree evaluated = __evaluate_1(t, depth + 1),
        ret;

    switch (TYPE(evaluated)) {
    case T_INTEGER:
        ret = evaluated;
        break;
    case T_LIVE_VAR:
        switch (TYPE(tLV_TYPE(evaluated))) {
        case D_T_CHAR ... D_T_ULONGLONG:
            ret = make_int_from_live_var(evaluated);
            break;
        default:
            eval_die(t, "Could not convert live var type to comparable type\n");
        }
        break;
    default:
        eval_die(t, "Could not convert type to comparable type\n");
    }

    return ret;
}

static tree eval_lt(tree t, int depth)
{
    tree left  = convert_to_comparable_type(tLT_LHS(t), depth),
        right = convert_to_comparable_type(tLT_RHS(t), depth),
        ret;

    if (TYPE(left) != TYPE(right))
        eval_die(t, "Could not compare different types\n");

    switch (TYPE(left)) {
    case T_INTEGER:
    {
        int result = mpz_cmp(tINT(left), tINT(right));
        ret = tree_make(T_INTEGER);
        mpz_init_set_ui(tINT(ret), result < 0 ? 1 : 0);
        break;
    }
    default:
        eval_die(t, "Unknown comparable types for lt\n");
    }

    return ret;
}

static tree eval_gt(tree t, int depth)
{
    tree left  = convert_to_comparable_type(tGT_LHS(t), depth),
        right = convert_to_comparable_type(tGT_RHS(t), depth),
        ret;

    if (TYPE(left) != TYPE(right))
        eval_die(t, "Could not compare different types\n");

    switch (TYPE(left)) {
    case T_INTEGER:
    {
        int result = mpz_cmp(tINT(left), tINT(right));
        ret = tree_make(T_INTEGER);
        mpz_init_set_ui(tINT(ret), result > 0 ? 1 : 0);
        break;
    }
    default:
        eval_die(t, "Unknown comparable types for gt\n");
    }

    return ret;
}

static tree eval_lteq(tree t, int depth)
{
    tree left  = convert_to_comparable_type(tLTEQ_LHS(t), depth),
        right = convert_to_comparable_type(tLTEQ_RHS(t), depth),
        ret;

    if (TYPE(left) != TYPE(right))
        eval_die(t, "Could not compare different types\n");

    switch (TYPE(left)) {
    case T_INTEGER:
    {
        int result = mpz_cmp(tINT(left), tINT(right));
        ret = tree_make(T_INTEGER);
        mpz_init_set_ui(tINT(ret), result <= 0 ? 1 : 0);
        break;
    }
    default:
        eval_die(t, "Unknown comparable types for lteq\n");
    }

    return ret;
}

static tree eval_gteq(tree t, int depth)
{
    tree left  = convert_to_comparable_type(tGTEQ_LHS(t), depth),
        right = convert_to_comparable_type(tGTEQ_RHS(t), depth),
        ret;

    if (TYPE(left) != TYPE(right))
        eval_die(t, "Could not compare different types\n");

    switch (TYPE(left)) {
    case T_INTEGER:
    {
        int result = mpz_cmp(tINT(left), tINT(right));
        ret = tree_make(T_INTEGER);
        mpz_init_set_ui(tINT(ret), result >= 0 ? 1 : 0);
        break;
    }
    default:
        eval_die(t, "Unknown comparable types for gteq\n");
    }

    return ret;
}

static tree eval_logic_or(tree t, int depth)
{
    int iret = 0;
    tree left  = convert_to_comparable_type(tL_OR_LHS(t), depth),
        right,
        ret;

    switch (TYPE(left)) {
    case T_INTEGER:
    {
        if (mpz_cmp_ui(tINT(left), 0)) {
            iret = 1;
            /* Short circuit! */
            goto out;
        }
        break;
    }
    default:
        eval_die(t, "Unknown comparable types for gt\n");
    }

    right = convert_to_comparable_type(tL_OR_RHS(t), depth);

    switch (TYPE(right)) {
    case T_INTEGER:
    {
        if (mpz_cmp_ui(tINT(right), 0))
            iret = 1;
        break;
    }
    default:
        eval_die(t, "Unknown comparable types for gt\n");
    }


out:
    ret = tree_make(T_INTEGER);
    mpz_init_set_ui(tINT(ret), iret);
    return ret;
}

static tree eval_logic_and(tree t, int depth)
{
    int iret = 0;
    tree left  = convert_to_comparable_type(tL_AND_LHS(t), depth),
        right,
        ret;

    switch (TYPE(left)) {
    case T_INTEGER:
    {
        if (!mpz_cmp_ui(tINT(left), 0))
            /* Short circuit! */
            goto out;

        break;
    }
    default:
        eval_die(t, "Unknown comparable types for gt\n");
    }

    right = convert_to_comparable_type(tL_AND_RHS(t), depth);

    switch (TYPE(right)) {
    case T_INTEGER:
    {
        if (mpz_cmp_ui(tINT(right), 0))
            iret = 1;
        break;
    }
    default:
        eval_die(t, "Unknown comparable types for gt\n");
    }


out:
    ret = tree_make(T_INTEGER);
    mpz_init_set_ui(tINT(ret), iret);
    return ret;
}



/* All types evaluate to themselves. */
#define DEFCTYPE(TNAME, DESC, CTYPE, FMT)       \
    static tree eval_##TNAME(tree t, int depth) \
    {                                           \
        return t;                               \
    }
#include "ctypes.def"
#undef DEFCTYPE

static tree eval_loop_for(tree t, int depth)
{
    __evaluate_1(tFLOOP_INIT(t), depth + 1);

    do {

        tree cond_result = __evaluate_1(tFLOOP_COND(t),
                                        depth + 1);

        if (!is_T_INTEGER(cond_result))
            eval_die(t, "Unknown condition result");

        if (!mpz_get_si(tINT(cond_result)))
            break;

        push_ctx("For Loop");
        __evaluate(tFLOOP_STMTS(t), depth + 1);
        pop_ctx();

        __evaluate_1(tFLOOP_AFTER(t), depth + 1);

    } while (1);

    return NULL;
}

static tree eval_if(tree t, int depth)
{
    tree cond_result = convert_to_comparable_type(tIF_COND(t), depth);

    if (!is_T_INTEGER(cond_result))
        eval_die(t, "Unknown condition result");

    if (mpz_get_si(tINT(cond_result)))
        __evaluate(tIF_TRUE_STMTS(t), depth + 1);
    else
        __evaluate(tIF_ELSE_STMTS(t), depth + 1);

    return NULL;
}

static tree eval_return(tree t, int depth)
{
    tRET_EXP(t) = __evaluate_1(tRET_EXP(t), depth + 1);

    return t;
}

/* Build a simplified decl chain, ensuring all decls with multiple
 * identifiers are split.
 *
 * This is the equivalent of mutating
 *
 *     struct foobar {
 *         int a, b;
 *         float c, d;
 *         char *e, f;
 *     }
 *
 * to
 *
 *     struct foobar {
 *         int a;
 *         int b;
 *         float c;
 *         float d;
 *         char *e;
 *         char f;
 *     };
 */
static tree expand_decl_chain(tree decl_chain)
{
    tree decl, decl_element, new_chain = tree_make(CHAIN_HEAD);

    for_each_tree(decl, decl_chain) {
        for_each_tree(decl_element, tDECL_DECLS(decl)) {
            tree new_decl = tree_make(T_DECL);

            tDECL_TYPE(new_decl) = tDECL_TYPE(decl);
            tDECL_DECLS(new_decl) = decl_element;

            tree_chain(new_decl, new_chain);
        }
    }

    return new_chain;
}

/* Calculate the byte-alignment requirements for a particular type or
 * live variable. */
static size_t get_alignment_for_type(tree t, int depth)
{
    size_t alignment;

    switch (TYPE(t))
    {
    case T_LIVE_VAR:
        alignment = get_size_of_type(tLV_TYPE(t), depth);
        break;
    case T_LIVE_COMPOUND:
    {
        tree first_member;

        for_each_tree(first_member, tLV_COMP_MEMBERS(t))
            break;

        first_member = tEMAP_RIGHT(first_member);

        alignment = get_alignment_for_type(first_member, depth);
    }
    break;
    case D_T_CHAR...D_T_PTR:
        alignment = get_size_of_type(t, depth);
        break;
    default:
        eval_die(t, "Could not calculate offset for member");
    }

    return alignment;
}

static tree eval_decl_compound(tree t, int depth)
{
    tree i, struct_id = tCOMP_DECL_ID(t);
    int offset = 0;
    size_t max_member_sz = 0;

    /* We map to ourselves here so that any references to the same
     * compound in the decls will result in a reference to the
     * struct_decl.  Also check to see if there are any forward
     * declarations of the type.  If there are, copy 't' to the
     * unresolved decl and use that as our new 't' so all referencies
     * to it will be updated.*/
    if (struct_id) {
        tree forward_decl = resolve_identifier(struct_id, SCOPE_CURRENT_CTX);

        if (forward_decl) {
            tree_copy(forward_decl, t);
            t = forward_decl;
        } else
            map_identifier(struct_id, t);
    }

    /* Don't attempt to expand an already expanded struct. */
    if (tCOMP_DECL_EXPANDED(t))
        return t;

    /* Expand the compound decl chain so each decl can have it's own
     * offset.  */
    tCOMP_DECL_DECLS(t) = expand_decl_chain(tCOMP_DECL_DECLS(t));

    /* Populate the decls with their offsets, as well as the total
     * size of the compound. */
    for_each_tree(i, tCOMP_DECL_DECLS(t)) {
        tree id, live_var;
        size_t member_size, member_alignment, padding_size;

        /* To calculate the size of each element of the compound, we
         * temporarily push the evaluation ctx, evaluate each decl
         * within the new evaluation ctx, resolve the identifier back
         * to it's live var and finally pass that through a sizeof()
         * evaluation.
         *
         * NOTE: we don't evaluate the types of the decls here as we
         * could create a loop back to ourselves (when declaring a
         * pointer to the same struct).  Instead, we evaluate all decl
         * types at struct installation time. */
        push_ctx("Compound Declaration");

        id = __evaluate_1(i, depth + 1);
        live_var = resolve_identifier(id, SCOPE_CURRENT_CTX);

        /* Calculate the offset of the member within the struct.
         *
         * The algorithm we use to calculate the offset of a given
         * member is as follows: If the current alignment is 0, use
         * that.  Otherwise, a given member's offset must be a
         * multiple of it's alignment requirement from the current
         * offset.
         */
        member_size = get_size_of_type(live_var, depth);
        member_alignment = get_alignment_for_type(live_var, depth);

        if (offset == 0)
            padding_size = 0;
        else
            padding_size = (~offset + 1) & (member_alignment - 1);

        tDECL_OFFSET(i) = offset + padding_size;

        if (tCOMP_DECL_TYPE(t) == sstruct)
            offset += padding_size + member_size;
        else {
            offset = 0;
            if (member_size > max_member_sz)
                max_member_sz = member_size;
        }

        pop_ctx();
    }

    if (tCOMP_DECL_TYPE(t) == sstruct)
        tCOMP_DECL_SZ(t) = offset;
    else
        tCOMP_DECL_SZ(t) = max_member_sz;

    tCOMP_DECL_EXPANDED(t) = 1;

    return t;
}

static tree eval_enumerator(tree t, int depth)
{
    tree enum_id, enum_base_type = tree_make(ENUMTYPE);
    unsigned int enum_val = 0;

    /* Whenever we encounter this decl, we wish to treat it as just a
     * normal integer. */
    if (tENUM_NAME(t))
        map_identifier(tENUM_NAME(t), enum_base_type);

    /* Allocate a number for each member of the enumerator and map the
     * identifier to the value. */
    for_each_tree(enum_id, tENUM_ENUMS(t)) {
        tree integer = tree_make(T_INTEGER);
        mpz_init_set_ui(tINT(integer), enum_val);

        map_identifier(enum_id, integer);

        enum_val++;
    }

    return enum_base_type;
}

static tree eval_comp_access(tree t, int depth)
{
    tree left = __evaluate_1(tCOMP_ACCESS_OBJ(t), depth + 1),
        id = tCOMP_ACCESS_MEMBER(t);

    if (!is_T_LIVE_COMPOUND(left))
        eval_die(t, "Unknown compound type in compound access\n");

    if (!is_T_IDENTIFIER(id))
        eval_die(t, "Unknown accessor in compound access\n");

    return resolve_id(id, tLV_COMP_MEMBERS(left));
}

static tree eval_struct(tree t, int depth)
{
    tree struct_name = tSTRUCT_EXP(t);

    if (resolve_identifier(tSTRUCT_EXP(t), SCOPE_GLOBAL))
        return __evaluate_1(tSTRUCT_EXP(t), depth);
    else
        return tree_make(E_INCOMP_TYPE);
}

static tree eval_union(tree t, int depth)
{
    return __evaluate_1(tUNION_EXP(t), depth);
}

static tree eval_array_access(tree t, int depth)
{
    tree array = __evaluate_1(tARR_ACCESS_OBJ(t), depth + 1),
        index = __evaluate_1(tARR_ACCESS_IDX(t), depth + 1),
        new_ptr, deref;
    size_t idx, base_type_length;
    void *base;

    if (!is_T_LIVE_VAR(array))
        eval_die(t, "Attempted to access non-live variable.\n");

    if (!is_D_T_PTR(tLV_TYPE(array)))
        eval_die(t, "Attempted deference on non pointer variable.\n");

    if (is_T_LIVE_VAR(index))
        index = make_int_from_live_var(index);

    if (!is_T_INTEGER(index))
        eval_die(index, "Unknown index type\n");

    idx = mpz_get_ui(tINT(index));

    base = tLV_VAL(array)->D_T_PTR;

    /* Find the size of the type that the pointer points to. */
    base_type_length = get_size_of_type(tDTPTR_EXP(tLV_TYPE(array)), depth);

    /* Calculate the offset into the array and make a pointer to point
     * to that offset. */
    new_ptr = make_live_var(tLV_TYPE(array));
    tLV_VAL(new_ptr)->D_T_PTR = base + (base_type_length * idx);

    /* Dereference the pointer and return the result. */
    deref = tree_make(T_DEREF);
    tDEREF_EXP(deref) = new_ptr;

    return __evaluate_1(deref, depth + 1);
}

static int get_ctype_size(tree t)
{
    switch (TYPE(t))
    {
#define DEFCTYPE(TNAME, DESC, CTYPE, FMT)       \
        case TNAME:                             \
            return sizeof(CTYPE);
#include "ctypes.def"
#undef DEFCTYPE
        default:
            eval_die(t, "Unknown ctype\n");
    }
}

static tree eval_sizeof(tree t, int depth)
{
    tree exp = __evaluate_1(tSZOF_EXP(t), depth + 1),
        type = exp,
         ret = tree_make(T_INTEGER);

    if (is_T_LIVE_VAR(exp)) {

        type = tLV_TYPE(exp);

        if (is_D_T_PTR(type) && tLV_IS_ARRAY(exp)) {
            mpz_init_set_ui(tINT(ret), tLV_ARRAY_SZ(exp));
            return ret;
        }
    }

    if (is_T_LIVE_COMPOUND(exp))
        type = tLV_COMP_DECL(exp);

    if (is_T_POINTER(exp))
        type = tree_make(D_T_PTR);

    if (is_CTYPE(type)) {
        mpz_init_set_ui(tINT(ret), get_ctype_size(type));
        return ret;
    }

    if (is_T_DECL_COMPOUND(type)) {
        mpz_init_set_ui(tINT(ret), tCOMP_DECL_SZ(type));
        return ret;
    }

    eval_die(t, "Could not calculate size of expression");
}

static tree handle_addr_fn_def(tree fndef)
{
    tree ptr_type = tree_make(D_T_PTR),
        fun_sig = tree_make(T_DECL_FN),
        live_var;

    tFNDECL_RET_TYPE(fun_sig) = tFNDEF_RET_TYPE(fndef);
    tFNDECL_ARGS(fun_sig) = tFNDEF_ARGS(fndef);

    tDTPTR_EXP(ptr_type) = fun_sig;

    live_var = make_live_var(ptr_type);

    tLV_VAL(live_var)->D_T_PTR = get_entry_point_for_fn(fndef);

    return live_var;
}

static tree eval_addr(tree t, int depth)
{
    tree exp = __evaluate_1(tADDR_EXP(t), depth + 1),
        ptr_type, ret, live_type;

    void *addr;

    if (is_T_FN_DEF(exp))
        return handle_addr_fn_def(exp);

    if (!is_LIVE(exp))
        eval_die(t, "attempted to take address of non-live variable.\n");

    if (is_T_LIVE_VAR(exp)) {
        live_type = tLV_TYPE(exp);
        addr = tLV_VAL(exp);
    }
    else if (is_T_LIVE_COMPOUND(exp)) {
        live_type = tLV_COMP_DECL(exp);
        addr = tLV_COMP_BASE(exp);
    }
    else
        eval_die(t, "Can't take address of unknown live type\n");

    ptr_type = tree_make(D_T_PTR);
    tDTPTR_EXP(ptr_type) = live_type;

    ret = make_live_var(ptr_type);

    tLV_VAL(ret)->D_T_PTR = addr;

    return ret;
}

static tree eval_deref(tree t, int depth)
{
    tree exp = __evaluate_1(tDEREF_EXP(t), depth + 1),
        new_type, ret;

    if (!is_T_LIVE_VAR(exp))
        eval_die(t, "Derefencing something that isn't live\n");

    if (!is_D_T_PTR(tLV_TYPE(exp)))
        eval_die(t, "Attempted to dereference a non-pointer\n");

    new_type = tDTPTR_EXP(tLV_TYPE(exp));

    if (is_T_DECL_COMPOUND(new_type))
        return instantiate_struct(new_type, depth,
                                  tLV_VAL(exp)->D_T_PTR);

    /* All live vars that are created by a pointer dereference won't
     * have their values free'd by the GC. */
    ret = tree_make(T_LIVE_VAR);
    tLV_TYPE(ret) = new_type;
    tLV_VAL(ret) = tLV_VAL(exp)->D_T_PTR;

    return ret;
}

static tree eval_cpp_include(tree t, int depth)
{
    char cpp_file_name[] = "/tmp/bic.cpp.XXXXXX.c";
    char out_file_name[] = "/tmp/bic.cpp.XXXXXX.c";
    char *command;
    extern FILE* cfilein;
    extern tree cfile_parse_head;
    tree i, cpp_include = tree_make(CPP_INCLUDE);
    FILE *out_file_stream;
    int cpp_file_fd, out_file_fd, ret;

    /* We don't have a real C preprocessor that we can call upon
     * yet. Therefore, we have to call out to the system's
     * preprocessor (assumed to be `cpp') and parse the results.
     *
     * NOTE: this is very limited, since we are calling out to the
     * system's CPP every time, we can't maintain the state of
     * definitions.  Therefore, we WILL define things multiple
     * times. */
    tCPP_INCLUDE_STR(cpp_include) = GC_STRDUP(tCPP_INCLUDE_STR(t));
    tree_chain(cpp_include, include_chain);

    cpp_file_fd = mkstemps(cpp_file_name, 2);

    if (cpp_file_fd == -1)
        eval_die(t, "Could not open temp file: %s\n", strerror(errno));

    out_file_fd = mkstemps(out_file_name, 2);

    if (out_file_fd == -1)
        eval_die(t, "Could not open temp file: %s\n", strerror(errno));

    out_file_stream = fdopen(out_file_fd, "r");

    if (!out_file_stream)
        eval_die(t, "Could not create stream: %s\n", strerror(errno));

    for_each_tree (i, include_chain) {
        write(cpp_file_fd, tCPP_INCLUDE_STR(i), strlen(tCPP_INCLUDE_STR(i)));
        write(cpp_file_fd, "\n", 1);
    }

    close(cpp_file_fd);

    ret = asprintf(&command, "gcc -E -P %s > %s", cpp_file_name, out_file_name);

    if (ret == -1)
        eval_die(t, "Could not compose preprocessor command: %s\n",
                 strerror(errno));

    ret = system(command);

    free(command);

    if (ret < 0)
        eval_die(t, "Invocation of preprocessor process failed\n");

    if (ret > 0)
        eval_die(t, "Preprocessor command exited with non-zero status\n");

    cfilein = out_file_stream;

    reset_include_typenames();
    typename_set_include_file();
    ret = cfileparse();
    typename_unset_include_file();

    fclose(out_file_stream);

    if (ret)
        eval_die(t, "Parsing of preprocessor output failed\n");

    push_ctx("Include file declarations");

    /* Delete old #include evaluator state before evaluating the parse
     * tree for the new #includes. */
    include_ctx = NULL;
    __evaluate(cfile_parse_head, depth + 1);
    include_ctx = cur_ctx;
    pop_ctx();

    tPARENT_CTX(include_ctx) = NULL;

    return NULL;
}

static tree eval_evaluator_ctx(tree t, int depth)
{
    tree ret, mapping;

    /* We should only encounter an E_CTX object when it has been set
     * up by handle_static_decl.  Ensure this is the case by checking
     * the is_static flag.  If this is a static declaration, we need
     * to merge it's id_map into the cur_ctx's.  We can easily do this
     * by iterating through it's map and calling `map_identifier'.  We
     * don't need to worry about adding the mappings allocations to
     * cur_ctx since they are stored within the E_CTX object itself
     * and it's lifetime will outlive that of cur_ctx. */

    if (!tIS_STATIC(t))
        eval_die(t, "Non static E_CTX object evaluated.\n");

    for_each_tree(mapping, tID_MAP(t)) {
        tree id;

        if (!is_E_MAP(mapping))
            eval_die(t, "Unknown type in static mapping\n");

        id = tEMAP_LEFT(mapping);

        map_identifier(tEMAP_LEFT(mapping), tEMAP_RIGHT(mapping));

        ret = id;
    }

    return ret;
}

static tree eval_self(tree t, int depth)
{
    return t;
}

static tree __evaluate_1(tree t, int depth)
{
    tree result = NULL;

    if (!t)
        return result;

    switch (TYPE(t))
    {
    case T_IDENTIFIER: result = eval_identifier(t, depth + 1); break;
    case T_FN_CALL:    result = eval_fn_call(t, depth + 1);    break;
    case T_FN_DEF:     result = eval_fn_def(t, depth + 1);     break;
    case T_DECL_FN:    result = eval_self(t, depth + 1);       break;
    case T_DECL:       result = eval_decl(t, depth + 1);       break;
    case T_ASSIGN:     result = eval_assign(t, depth + 1);     break;
    case T_FLOAT:      result = eval_self(t, depth + 1);       break;
    case T_INTEGER:    result = eval_self(t, depth + 1);       break;
    case T_STRING:     result = eval_self(t, depth + 1);       break;
    case T_P_INC:      result = eval_post_inc(t, depth + 1);   break;
    case T_P_DEC:      result = eval_post_dec(t, depth + 1);   break;
    case T_INC:        result = eval_inc(t, depth + 1);        break;
    case T_DEC:        result = eval_dec(t, depth + 1);        break;
    case T_ADD:        result = eval_add(t, depth + 1);        break;
    case T_SUB:        result = eval_sub(t, depth + 1);        break;
    case T_MUL:        result = eval_mul(t, depth + 1);        break;
    case T_DIV:        result = eval_div(t, depth + 1);        break;
    case T_LT:         result = eval_lt(t, depth + 1);         break;
    case T_GT:         result = eval_gt(t, depth + 1);         break;
    case T_LTEQ:       result = eval_lteq(t, depth + 1);       break;
    case T_GTEQ:       result = eval_gteq(t, depth + 1);       break;
    case T_L_OR:       result = eval_logic_or(t, depth + 1);   break;
    case T_L_AND:      result = eval_logic_and(t, depth + 1);  break;
    case T_LIVE_VAR:   result = eval_self(t, depth + 1);       break;
    case T_LIVE_COMPOUND: result = eval_self(t, depth + 1);    break;
    case T_EXTERN:     result = eval_self(t, depth + 1);       break;
    case T_TYPEDEF:    result = eval_self(t, depth + 1);       break;
    case T_STATIC:     result = eval_self(t, depth + 1);       break;
    case T_LOOP_FOR:   result = eval_loop_for(t, depth + 1);   break;
    case T_IF:         result = eval_if(t, depth + 1);         break;
    case T_RETURN:     result = eval_return(t, depth + 1);     break;
    case T_DECL_COMPOUND:result = eval_decl_compound(t, depth + 1);break;
    case T_ENUMERATOR: result = eval_enumerator(t, depth + 1); break;
    case T_SIZEOF:     result = eval_sizeof(t, depth + 1);     break;
    case T_COMP_ACCESS:result = eval_comp_access(t, depth + 1);break;
    case T_STRUCT:     result = eval_struct(t, depth + 1);     break;
    case T_UNION:      result = eval_union(t, depth + 1);      break;
    case T_ARRAY_ACCESS:result = eval_array_access(t, depth + 1); break;
    case T_ADDR:       result = eval_addr(t, depth + 1);       break;
    case T_DEREF:      result = eval_deref(t, depth + 1);      break;
    case T_POINTER:    result = eval_self(t, depth + 1);       break;
    case D_T_VOID:     result = eval_self(t, depth + 1);       break;
    case CPP_INCLUDE:  result = eval_cpp_include(t, depth + 1); break;
    case E_CTX:        result = eval_evaluator_ctx(t, depth + 1); break;
#define DEFCTYPE(TNAME, DESC, CTYPE, FMT)                               \
    case TNAME:        result = eval_##TNAME(t, depth + 1);    break;
#include "ctypes.def"
#undef DEFCTYPE
    default:           result = NULL;                          break;
    }

    return result;
}

static tree __evaluate(tree head, int depth)
{
    tree result, i;

    if (!head)
        return NULL;

    if (!is_CHAIN_HEAD(head))
        return __evaluate_1(head, depth);

    for_each_tree(i, head) {
        result = __evaluate_1(i, depth);

        if (is_T_RETURN(result))
            return tRET_EXP(result);
    }

    return result;
}

tree evaluate(tree t, const char *fname)
{
    current_filename = fname;
    return __evaluate(t, 0);
}

static void eval_init_builtins(void)
{
    map_identifier(get_identifier("__builtin_va_list"), tree_make(D_T_INT));
}

void eval_init(void)
{
    push_ctx("Toplevel");
    eval_init_builtins();
    include_chain = tree_make(CHAIN_HEAD);
}
