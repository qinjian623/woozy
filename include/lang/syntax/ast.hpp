#pragma once
#include "lang/syntax/token.hpp"
namespace lang {
// Children are in source order; token ranges are half-open, including
// punctuation.
enum class AstKind {
  File,
  Package,
  Import,
  Binding,
  Function,
  Struct,
  Enum,
  Trait,
  Impl,
  Field,
  Variant,
  GenericParameter,
  Parameter,
  NamedType,
  FunctionType,
  ParenthesizedType,
  Block,
  ExpressionStatement,
  Assign,
  If,
  For,
  ForIn,
  Switch,
  Case,
  Try,
  Catch,
  Defer,
  Unsafe,
  Return,
  Throw,
  Break,
  Continue,
  Name,
  Literal,
  Unary,
  Binary,
  Call,
  Argument,
  Member,
  NeutralBracket,
  BracketItem,
  Slice,
  List,
  FunctionLiteral,
  Parenthesized,
  BindPattern,
  LiteralPattern,
  VariantPattern,
  Error
};
struct AstNode {
  // BracketItem children are syntactic candidates only. Even when a child is
  // a type syntax node, classification is deferred; token_begin/token_end
  // preserve the complete unresolved item for name/type analysis.
  AstKind kind;
  SourceSpan span;
  size_t token_begin, token_end;
  std::vector<AstNodeId> children;
  bool recovered = false;
};
struct AstFile {
  std::shared_ptr<const SourceSnapshot> source;
  std::vector<Token> tokens;
  std::vector<AstNode> nodes;
  AstNodeId root;
  bool recovered = false;
  const AstNode &node(AstNodeId id) const { return nodes.at(id.value); }
};
// Only direct children belong to this signature. In particular, a throws
// inside a parameter or returned FunctionType is not an outer throws.
struct SignatureSyntax {
  std::vector<AstNodeId> parameters;
  std::optional<AstNodeId> result, throws_type;
};
SignatureSyntax split_signature(const AstFile &, AstNodeId);
} // namespace lang
