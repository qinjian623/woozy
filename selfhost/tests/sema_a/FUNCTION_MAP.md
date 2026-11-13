# A function map (CPP `src/sema/check.cpp`)

`a_types.wz`

- 208 `trait_key`, 214 `trait_prototype`, 221 `trait_decl`,
  227 `symbol_from_module`, 235 `std_iterator_trait`, 241 `std_option`,
  254 `iterator_candidate`, 280 `mentions_self`, 307 `require_object_safe`,
  361 `check_trait_application`, 376 `check_trait_parameter_bounds`.
- 502 `parameter_type`, 508 `template_inout_type`, 522 `owning_inout_type`,
  531 `owning_inout_reason`, 543 `trait_self_type`, 548 `impl_of`,
  555 `declared_self`, 556 `inherent_self_source`, 568 `inherent_method_source`,
  590 `inherent_method_route`, 606 `method_own_parameters`,
  623 `trait_impl_signature_matches`, 637 `trait_impl_inout_source`,
  693 `trait_impl_move_source`, 744 `constraint_method_source`,
  783 `constraint_method_move_source`, 825 `constraint_static_method_move_source`,
  868 `constraint_static_method_source`, 914 `free_generic_template`,
  960 `check_inout_graph`, 965 `check_trait_application_data`,
  1092 `check_generic_trait_method_modes`, 1137 `check_function_value_inout`,
  1155 `check_method_value_modes`, 1167 `generics`, 1189 `nominal`,
  1219 `nominal_subs`, 1222 `field_type`, 1229 `parse_type`,
  1426 `signature`, 2865 `contains_parameter`, 2891 `may_contain_weak`,
  2924 `validate_weak_type`, 2927 `validate_weak_at` (Source overload),
  2994 `validate_weak_types_before_publish`, 3270 `infer`, 3301 `try_infer`,
  3309 `finish_arguments`, 3324 `satisfies`, 3389 `check_constraints`,
  3409 `select_method`.
- 3623 `declared_foreign_type`, 3626 `c_type`, 3636 `c_decl`,
  3639 `c_callable`, 3646 `check_c_callback`.

`a_declarations.wz`

- 5497 `self_parameter`, 5503 `register_function`, 5599 `overlap`,
  5606 `impls_overlap`, 5624 `declarations`, 6169 `duplicate_symbol`.

`a_instantiate.wz`

- 6189 `explicit_method_type_refs`, 6217 `constraint_method_arguments`,
  6287 `transform_node`, 6412 `instantiate`.

`a_helpers.wz` contains only local helpers (copy/upsert/set/map lookups) and no
named CPP method.

## Suggested focused inputs for the unexecuted instantiation side

- generic free function and inherent method: concrete `Self`, exact instance
  reuse, recursive calls, non-Unit fallthrough rejection.
- generic capture/bound receiver: owning capture becomes `Move`; non-owning
  stays `Read`; bound receiver substituted through `instance_sub`.
- `ConstraintCall` and `TraitObjectInit`: explicit method type suffix, phantom
  own parameter requiring explicit arguments, trait prototype function value.
- `Defer.references`, `Try` catches, `Switch` arms and `ForIn` iterator:
  renamed locals, substituted types, preserved/sorted references, duplicate
  catch types rejected.
- instance budgets: 4096 cache entries, depth 48, 200000 cloned nodes; confirm
  `E_INSTANTIATION_LIMIT` diagnostics and no partial stable publication.
- `check_types`/`check_project` only after B and the Root facade exist.
