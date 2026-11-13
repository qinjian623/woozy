This source requires the test-only `demo.Point` property binding constructed
by `selfhost/tests/sema_b/src/main.wz` (`build_property`).  A plain project
without that binding cannot resolve `point`; it is kept here as the exact
source input for the PropertyRead HIR marker assertion.
