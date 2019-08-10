#include "chibi.h"

// Scope for local variables, global variables or typedefs
typedef struct VarScope VarScope;
struct VarScope {
  VarScope *next;
  char *name;
  Var *var;
  Type *type_def;
};

// Scope for struct tags
typedef struct TagScope TagScope;
struct TagScope {
  TagScope *next;
  char *name;
  Type *ty;
};

typedef struct {
  VarScope *var_scope;
  TagScope *tag_scope;
} Scope;

// All local variable instances created during parsing are
// accumulated to this list.
VarList *locals;

// Likewise, global variables are accumulated to this list.
VarList *globals;

// C has two block scopes; one is for variables/typedefs and
// the other is for struct tags.
VarScope *var_scope;
TagScope *tag_scope;

// Begin a block scope
Scope *enter_scope() {
  Scope *sc = calloc(1, sizeof(Scope));
  sc->var_scope = var_scope;
  sc->tag_scope = tag_scope;
  return sc;
}

// End a block scope
void leave_scope(Scope *sc) {
  var_scope = sc->var_scope;
  tag_scope = sc->tag_scope;
}

// Find a variable or a typedef by name.
VarScope *find_var(Token *tok) {
  for (VarScope *sc = var_scope; sc; sc = sc->next)
    if (strlen(sc->name) == tok->len && !strncmp(tok->str, sc->name, tok->len))
      return sc;
  return NULL;
}

TagScope *find_tag(Token *tok) {
  for (TagScope *sc = tag_scope; sc; sc = sc->next)
    if (strlen(sc->name) == tok->len && !strncmp(tok->str, sc->name, tok->len))
      return sc;
  return NULL;
}

Node *new_node(NodeKind kind, Token *tok) {
  Node *node = calloc(1, sizeof(Node));
  node->kind = kind;
  node->tok = tok;
  return node;
}

Node *new_binary(NodeKind kind, Node *lhs, Node *rhs, Token *tok) {
  Node *node = new_node(kind, tok);
  node->lhs = lhs;
  node->rhs = rhs;
  return node;
}

Node *new_unary(NodeKind kind, Node *expr, Token *tok) {
  Node *node = new_node(kind, tok);
  node->lhs = expr;
  return node;
}

Node *new_num(long val, Token *tok) {
  Node *node = new_node(ND_NUM, tok);
  node->val = val;
  return node;
}

Node *new_var_node(Var *var, Token *tok) {
  Node *node = new_node(ND_VAR, tok);
  node->var = var;
  return node;
}

VarScope *push_scope(char *name) {
  VarScope *sc = calloc(1, sizeof(VarScope));
  sc->name = name;
  sc->next = var_scope;
  var_scope = sc;
  return sc;
}

Var *new_var(char *name, Type *ty, bool is_local) {
  Var *var = calloc(1, sizeof(Var));
  var->name = name;
  var->ty = ty;
  var->is_local = is_local;
  return var;
}

Var *new_lvar(char *name, Type *ty) {
  Var *var = new_var(name, ty, true);
  push_scope(name)->var = var;

  VarList *vl = calloc(1, sizeof(VarList));
  vl->var = var;
  vl->next = locals;
  locals = vl;
  return var;
}

Var *new_gvar(char *name, Type *ty) {
  Var *var = new_var(name, ty, false);
  push_scope(name)->var = var;

  VarList *vl = calloc(1, sizeof(VarList));
  vl->var = var;
  vl->next = globals;
  globals = vl;
  return var;
}

Type *find_typedef(Token *tok) {
  if (tok->kind == TK_IDENT) {
    VarScope *sc = find_var(tok);
    if (sc)
      return sc->type_def;
  }
  return NULL;
}

char *new_label() {
  static int cnt = 0;
  char buf[20];
  sprintf(buf, ".L.data.%d", cnt++);
  return strndup(buf, 20);
}

Function *function();
Type *basetype();
Type *declarator(Type *ty, char **name);
Type *type_suffix(Type *ty);
Type *struct_decl();
Member *struct_member();
void global_var();
Node *declaration();
bool is_typename();
Node *stmt();
Node *stmt2();
Node *expr();
Node *assign();
Node *equality();
Node *relational();
Node *add();
Node *mul();
Node *unary();
Node *postfix();
Node *primary();

// Determine whether the next top-level item is a function
// or a global variable by looking ahead input tokens.
bool is_function() {
  Token *tok = token;

  Type *ty = basetype();
  char *name = NULL;
  declarator(ty, &name);
  bool isfunc = name && consume("(");

  token = tok;
  return isfunc;
}

// program = (global-var | function)*
Program *program() {
  Function head = {};
  Function *cur = &head;
  globals = NULL;

  while (!at_eof()) {
    if (is_function()) {
      cur->next = function();
      cur = cur->next;
    } else {
      global_var();
    }
  }

  Program *prog = calloc(1, sizeof(Program));
  prog->globals = globals;
  prog->fns = head.next;
  return prog;
}

// basetype = builtin-type | struct-decl | typedef-name
// builtin-type   = "char" | "short" | "int" | "long"
Type *basetype() {
  if (!is_typename(token))
    error_tok(token, "typename expected");

  if (consume("char"))
    return char_type;
  if (consume("short"))
    return short_type;
  if (consume("int"))
    return int_type;
  if (consume("long"))
    return long_type;
  if (consume("struct"))
    return struct_decl();
  return find_var(consume_ident())->type_def;
}

// declarator = "*"* ("(" declarator ")" | ident) type-suffix
Type *declarator(Type *ty, char **name) {
  while (consume("*"))
    ty = pointer_to(ty);

  if (consume("(")) {
    Type *placeholder = calloc(1, sizeof(Type));
    Type *new_ty = declarator(placeholder, name);
    expect(")");
    memcpy(placeholder, type_suffix(ty), sizeof(Type));
    return new_ty;
  }

  *name = expect_ident();
  return type_suffix(ty);
}

// type-suffix = ("[" num "]" type-suffix)?
Type *type_suffix(Type *ty) {
  if (!consume("["))
    return ty;
  int sz = expect_number();
  expect("]");
  ty = type_suffix(ty);
  return array_of(ty, sz);
}

void push_tag_scope(Token *tok, Type *ty) {
  TagScope *sc = calloc(1, sizeof(TagScope));
  sc->next = tag_scope;
  sc->name = strndup(tok->str, tok->len);
  sc->ty = ty;
  tag_scope = sc;
}

// struct-decl = "struct" ident
//             | "struct" ident? "{" struct-member "}"
Type *struct_decl() {
  // Read a struct tag.
  Token *tag = consume_ident();
  if (tag && !peek("{")) {
    TagScope *sc = find_tag(tag);
    if (!sc)
      error_tok(tag, "unknown struct type");
    return sc->ty;
  }

  expect("{");

  // Read struct members.
  Member head = {};
  Member *cur = &head;

  while (!consume("}")) {
    cur->next = struct_member();
    cur = cur->next;
  }

  Type *ty = calloc(1, sizeof(Type));
  ty->kind = TY_STRUCT;
  ty->members = head.next;

  // Assign offsets within the struct to members.
  int offset = 0;
  for (Member *mem = ty->members; mem; mem = mem->next) {
    offset = align_to(offset, mem->ty->align);
    mem->offset = offset;
    offset += mem->ty->size;

    if (ty->align < mem->ty->align)
      ty->align = mem->ty->align;
  }
  ty->size = align_to(offset, ty->align);

  // Register the struct type if a name was given.
  if (tag)
    push_tag_scope(tag, ty);
  return ty;
}

// struct-member = basetype declarator type-suffix ";"
Member *struct_member() {
  Type *ty = basetype();
  char *name = NULL;
  ty = declarator(ty, &name);
  ty = type_suffix(ty);
  expect(";");

  Member *mem = calloc(1, sizeof(Member));
  mem->name = name;
  mem->ty = ty;
  return mem;
}

VarList *read_func_param() {
  Type *ty = basetype();
  char *name = NULL;
  ty = declarator(ty, &name);
  ty = type_suffix(ty);

  VarList *vl = calloc(1, sizeof(VarList));
  vl->var = new_lvar(name, ty);
  return vl;
}

VarList *read_func_params() {
  if (consume(")"))
    return NULL;

  VarList *head = read_func_param();
  VarList *cur = head;

  while (!consume(")")) {
    expect(",");
    cur->next = read_func_param();
    cur = cur->next;
  }

  return head;
}

// function = basetype declarator "(" params? ")" "{" stmt* "}"
// params   = param ("," param)*
// param    = basetype declarator type-suffix
Function *function() {
  locals = NULL;

  Type *ty = basetype();
  char *name = NULL;
  declarator(ty, &name);

  Function *fn = calloc(1, sizeof(Function));
  fn->name = name;
  expect("(");

  Scope *sc = enter_scope();
  fn->params = read_func_params();
  expect("{");

  Node head = {};
  Node *cur = &head;
  while (!consume("}")) {
    cur->next = stmt();
    cur = cur->next;
  }
  leave_scope(sc);

  fn->node = head.next;
  fn->locals = locals;
  return fn;
}

// global-var = basetype declarator type-suffix ";"
void global_var() {
  Type *ty = basetype();
  char *name = NULL;
  ty = declarator(ty, &name);
  ty = type_suffix(ty);
  expect(";");
  new_gvar(name, ty);
}

// declaration = basetype declarator type-suffix ("=" expr)? ";"
//             | basetype ";"
Node *declaration() {
  Token *tok = token;
  Type *ty = basetype();
  if (consume(";"))
    return new_node(ND_NULL, tok);

  char *name = NULL;
  ty = declarator(ty, &name);
  ty = type_suffix(ty);
  Var *var = new_lvar(name, ty);

  if (consume(";"))
    return new_node(ND_NULL, tok);

  expect("=");
  Node *lhs = new_var_node(var, tok);
  Node *rhs = expr();
  expect(";");
  Node *node = new_binary(ND_ASSIGN, lhs, rhs, tok);
  return new_unary(ND_EXPR_STMT, node, tok);
}

Node *read_expr_stmt() {
  Token *tok = token;
  return new_unary(ND_EXPR_STMT, expr(), tok);
}

// Returns true if the next token represents a type.
bool is_typename() {
  return peek("char") || peek("short") || peek("int") || peek("long") ||
         peek("struct") || find_typedef(token);
}

Node *stmt() {
  Node *node = stmt2();
  add_type(node);
  return node;
}

// stmt2 = "return" expr ";"
//       | "if" "(" expr ")" stmt ("else" stmt)?
//       | "while" "(" expr ")" stmt
//       | "for" "(" expr? ";" expr? ";" expr? ")" stmt
//       | "{" stmt* "}"
//       | "typedef" basetype declarator type-suffix ";"
//       | declaration
//       | expr ";"
Node *stmt2() {
  Token *tok;
  if (tok = consume("return")) {
    Node *node = new_unary(ND_RETURN, expr(), tok);
    expect(";");
    return node;
  }

  if (tok = consume("if")) {
    Node *node = new_node(ND_IF, tok);
    expect("(");
    node->cond = expr();
    expect(")");
    node->then = stmt();
    if (consume("else"))
      node->els = stmt();
    return node;
  }

  if (tok = consume("while")) {
    Node *node = new_node(ND_WHILE, tok);
    expect("(");
    node->cond = expr();
    expect(")");
    node->then = stmt();
    return node;
  }

  if (tok = consume("for")) {
    Node *node = new_node(ND_FOR, tok);
    expect("(");
    if (!consume(";")) {
      node->init = read_expr_stmt();
      expect(";");
    }
    if (!consume(";")) {
      node->cond = expr();
      expect(";");
    }
    if (!consume(")")) {
      node->inc = read_expr_stmt();
      expect(")");
    }
    node->then = stmt();
    return node;
  }

  if (tok = consume("{")) {
    Node head = {};
    Node *cur = &head;

    Scope *sc = enter_scope();
    while (!consume("}")) {
      cur->next = stmt();
      cur = cur->next;
    }
    leave_scope(sc);

    Node *node = new_node(ND_BLOCK, tok);
    node->body = head.next;
    return node;
  }

  if (tok = consume("typedef")) {
    Type *ty = basetype();
    char *name = NULL;
    ty = declarator(ty, &name);
    ty = type_suffix(ty);
    expect(";");

    push_scope(name)->type_def = ty;
    return new_node(ND_NULL, tok);
  }

  if (is_typename())
    return declaration();

  Node *node = read_expr_stmt();
  expect(";");
  return node;
}

// expr = assign
Node *expr() {
  return assign();
}

// assign = equality ("=" assign)?
Node *assign() {
  Node *node = equality();
  Token *tok;
  if (tok = consume("="))
    node = new_binary(ND_ASSIGN, node, assign(), tok);
  return node;
}

// equality = relational ("==" relational | "!=" relational)*
Node *equality() {
  Node *node = relational();
  Token *tok;

  for (;;) {
    if (tok = consume("=="))
      node = new_binary(ND_EQ, node, relational(), tok);
    else if (tok = consume("!="))
      node = new_binary(ND_NE, node, relational(), tok);
    else
      return node;
  }
}

// relational = add ("<" add | "<=" add | ">" add | ">=" add)*
Node *relational() {
  Node *node = add();
  Token *tok;

  for (;;) {
    if (tok = consume("<"))
      node = new_binary(ND_LT, node, add(), tok);
    else if (tok = consume("<="))
      node = new_binary(ND_LE, node, add(), tok);
    else if (tok = consume(">"))
      node = new_binary(ND_LT, add(), node, tok);
    else if (tok = consume(">="))
      node = new_binary(ND_LE, add(), node, tok);
    else
      return node;
  }
}

Node *new_add(Node *lhs, Node *rhs, Token *tok) {
  add_type(lhs);
  add_type(rhs);

  if (is_integer(lhs->ty) && is_integer(rhs->ty))
    return new_binary(ND_ADD, lhs, rhs, tok);
  if (lhs->ty->base && is_integer(rhs->ty))
    return new_binary(ND_PTR_ADD, lhs, rhs, tok);
  if (is_integer(lhs->ty) && rhs->ty->base)
    return new_binary(ND_PTR_ADD, rhs, lhs, tok);
  error_tok(tok, "invalid operands");
}

Node *new_sub(Node *lhs, Node *rhs, Token *tok) {
  add_type(lhs);
  add_type(rhs);

  if (is_integer(lhs->ty) && is_integer(rhs->ty))
    return new_binary(ND_SUB, lhs, rhs, tok);
  if (lhs->ty->base && is_integer(rhs->ty))
    return new_binary(ND_PTR_SUB, lhs, rhs, tok);
  if (lhs->ty->base && rhs->ty->base)
    return new_binary(ND_PTR_DIFF, lhs, rhs, tok);
  error_tok(tok, "invalid operands");
}

// add = mul ("+" mul | "-" mul)*
Node *add() {
  Node *node = mul();
  Token *tok;

  for (;;) {
    if (tok = consume("+"))
      node = new_add(node, mul(), tok);
    else if (tok = consume("-"))
      node = new_sub(node, mul(), tok);
    else
      return node;
  }
}

// mul = unary ("*" unary | "/" unary)*
Node *mul() {
  Node *node = unary();
  Token *tok;

  for (;;) {
    if (tok = consume("*"))
      node = new_binary(ND_MUL, node, unary(), tok);
    else if (tok = consume("/"))
      node = new_binary(ND_DIV, node, unary(), tok);
    else
      return node;
  }
}

// unary = ("+" | "-" | "*" | "&")? unary
//       | postfix
Node *unary() {
  Token *tok;
  if (consume("+"))
    return unary();
  if (tok = consume("-"))
    return new_binary(ND_SUB, new_num(0, tok), unary(), tok);
  if (tok = consume("&"))
    return new_unary(ND_ADDR, unary(), tok);
  if (tok = consume("*"))
    return new_unary(ND_DEREF, unary(), tok);
  return postfix();
}

Member *find_member(Type *ty, char *name) {
  for (Member *mem = ty->members; mem; mem = mem->next)
    if (!strcmp(mem->name, name))
      return mem;
  return NULL;
}

Node *struct_ref(Node *lhs) {
  add_type(lhs);
  if (lhs->ty->kind != TY_STRUCT)
    error_tok(lhs->tok, "not a struct");

  Token *tok = token;
  Member *mem = find_member(lhs->ty, expect_ident());
  if (!mem)
    error_tok(tok, "no such member");

  Node *node = new_unary(ND_MEMBER, lhs, tok);
  node->member = mem;
  return node;
}

// postfix = primary ("[" expr "]" | "." ident | "->" ident)*
Node *postfix() {
  Node *node = primary();
  Token *tok;

  for (;;) {
    if (tok = consume("[")) {
      // x[y] is short for *(x+y)
      Node *exp = new_add(node, expr(), tok);
      expect("]");
      node = new_unary(ND_DEREF, exp, tok);
      continue;
    }

    if (tok = consume(".")) {
      node = struct_ref(node);
      continue;
    }

    if (tok = consume("->")) {
      // x->y is short for (*x).y
      node = new_unary(ND_DEREF, node, tok);
      node = struct_ref(node);
      continue;
    }

    return node;
  }
}

// stmt-expr = "(" "{" stmt stmt* "}" ")"
//
// Statement expression is a GNU C extension.
Node *stmt_expr(Token *tok) {
  Scope *sc = enter_scope();
  Node *node = new_node(ND_STMT_EXPR, tok);
  node->body = stmt();
  Node *cur = node->body;

  while (!consume("}")) {
    cur->next = stmt();
    cur = cur->next;
  }
  expect(")");
  leave_scope(sc);

  if (cur->kind != ND_EXPR_STMT)
    error_tok(cur->tok, "stmt expr returning void is not supported");
  memcpy(cur, cur->lhs, sizeof(Node));
  return node;
}

// func-args = "(" (assign ("," assign)*)? ")"
Node *func_args() {
  if (consume(")"))
    return NULL;

  Node *head = assign();
  Node *cur = head;
  while (consume(",")) {
    cur->next = assign();
    cur = cur->next;
  }
  expect(")");
  return head;
}

// primary = "(" "{" stmt-expr-tail
//         | "(" expr ")"
//         | "sizeof" unary
//         | ident func-args?
//         | str
//         | num
Node *primary() {
  Token *tok;

  if (tok = consume("(")) {
    if (consume("{"))
      return stmt_expr(tok);

    Node *node = expr();
    expect(")");
    return node;
  }

  if (tok = consume("sizeof")) {
    Node *node = unary();
    add_type(node);
    return new_num(node->ty->size, tok);
  }

  if (tok = consume_ident()) {
    // Function call
    if (consume("(")) {
      Node *node = new_node(ND_FUNCALL, tok);
      node->funcname = strndup(tok->str, tok->len);
      node->args = func_args();
      return node;
    }

    // Variable
    VarScope *sc = find_var(tok);
    if (sc && sc->var)
      return new_var_node(sc->var, tok);
    error_tok(tok, "undefined variable");
  }

  tok = token;
  if (tok->kind == TK_STR) {
    token = token->next;

    Type *ty = array_of(char_type, tok->cont_len);
    Var *var = new_gvar(new_label(), ty);
    var->contents = tok->contents;
    var->cont_len = tok->cont_len;
    return new_var_node(var, tok);
  }

  if (tok->kind != TK_NUM)
    error_tok(tok, "expected expression");
  return new_num(expect_number(), tok);
}
