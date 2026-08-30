/- Module B: imports the sibling module A (tests the olean dependency chain). -/

import A

theorem double_add_two (n : Nat) : n + n + 2 = 2 * n + 2 := by
  omega
