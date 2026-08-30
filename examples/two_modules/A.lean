/- Module A: a tiny standalone module, core Lean only (no Mathlib). -/

theorem add_comm_small (a b : Nat) : a + b = b + a := Nat.add_comm a b
