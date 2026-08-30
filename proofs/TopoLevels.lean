/-
Formal verification of `topo_levels` in `leanoff.py`.

`leanoff build` schedules project modules for parallel compilation:
  * modules are grouped into dependency *levels* (`topo_levels`);
  * each level is compiled concurrently (`ThreadPoolExecutor`);
  * a module's internal dependencies must already be compiled when its
    level starts, i.e. they must live in strictly earlier levels;
  * a dependency cycle is reported ("import cycle among …") instead of
    hanging.

The model:
  * `deps n` — internal dependencies of module `n`, exactly what
    `parse_imports` produces (imports naming project modules, `dep in known`).
  * `S` — the module set (`known`).
  * `levels deps S` — `topo_levels`, levels as sets of module names. The
    Python code sorts each level for deterministic output; ordering is a
    presentation detail, orthogonal to the correctness properties below.
  * `none` — the "import cycle" exit.

Theorems (all for `levels deps S = some lvls` under the standing assumption
`hS : ∀ n ∈ S, deps n ⊆ S`, i.e. dependencies are project modules):
  * `levels_cover`            — every module is placed in some level.
  * `levels_level_subset`     — levels only contain project modules.
  * `levels_disjoint`         — levels are pairwise disjoint: no module is
                                scheduled twice.
  * `levels_deps_earlier`     — every dependency of a module in level `L`
                                lies in the union of the strictly earlier
                                levels: level-by-level compilation is sound.
  * `levels_same_level_indep` — no module in `L` depends on another member
                                of `L`: intra-level parallelism is safe.
  * `levels_none_cycle`       — the `none` exit certifies an actual cycle:
                                the error is never spurious.
  * `levels_exists_of_acyclic`— acyclic graphs always levelize: `build`
                                never gets stuck.
-/

import Mathlib.Data.Finset.Basic
import Mathlib.Data.Finset.Card
import Mathlib.Data.Finset.Image
import Mathlib.Data.Finset.Range
import Mathlib.Tactic.Push

open Classical

set_option linter.unusedSectionVars false

noncomputable section

variable {α : Type*} [DecidableEq α]

/-! ### The algorithm -/

/-- Modules in `remaining` whose internal dependencies all sit in `done`:
the `ready` set of one round of the `topo_levels` loop. -/
def readySet (deps : α → Finset α) (remaining done : Finset α) : Finset α :=
  remaining.filter (fun n => deps n ⊆ done)

/-- The levelization loop with an explicit fuel (iteration budget).
`remaining.card` always suffices when no cycle exists; fuel exhaustion
returns `none`, which under `remaining.card ≤ fuel` only happens for a
genuine cycle. `none` = the "import cycle among …" exit. -/
def levelsAux (deps : α → Finset α) : ℕ → Finset α → Finset α → Option (List (Finset α))
  | 0, remaining, _ => if remaining = ∅ then some [] else none
  | fuel + 1, remaining, done =>
    if remaining = ∅ then some []
    else if readySet deps remaining done = ∅ then none
    else
      match levelsAux deps fuel (remaining \ readySet deps remaining done)
              (done ∪ readySet deps remaining done) with
      | none => none
      | some rest => some (readySet deps remaining done :: rest)

/-- `topo_levels`: levelize the whole module set. -/
def levels (deps : α → Finset α) (S : Finset α) : Option (List (Finset α)) :=
  levelsAux deps S.card S ∅

/-- Union of a level list. -/
def unionAll : List (Finset α) → Finset α
  | [] => ∅
  | L :: rest => L ∪ unionAll rest

theorem mem_unionAll {Ls : List (Finset α)} {x : α} :
    x ∈ unionAll Ls ↔ ∃ L ∈ Ls, x ∈ L := by
  induction Ls with
  | nil => simp [unionAll]
  | cons L rest ih => simp [unionAll, ih]

theorem levelsAux_empty (deps : α → Finset α) (fuel : ℕ) (done : Finset α) :
    levelsAux deps fuel ∅ done = some [] := by
  cases fuel <;> simp [levelsAux]

/-! ### Set and cardinality helpers -/

theorem union_sdiff_of_subset {s t : Finset α} (h : s ⊆ t) : s ∪ (t \ s) = t := by
  ext x
  by_cases hx : x ∈ s
  · simp [hx, h hx]
  · simp [hx]

/-- One round strictly decreases the number of pending modules. -/
theorem card_sdiff_lt (rem ready : Finset α) (hsub : ready ⊆ rem) (hne : ready ≠ ∅) :
    (rem \ ready).card < rem.card := by
  have hcap : ready ∩ rem = ready := Finset.inter_eq_left.2 hsub
  have hcard : (rem \ ready).card = rem.card - (ready ∩ rem).card := Finset.card_sdiff
  rw [hcap] at hcard
  have hpos : 0 < ready.card := Finset.card_pos.2 (Finset.nonempty_iff_ne_empty.2 hne)
  have hle : ready.card ≤ rem.card := Finset.card_le_card hsub
  omega

/-- The split `done ∪ ready` / `rem \ ready` preserves the module universe. -/
theorem union_split (rem done ready : Finset α) (h : ready ⊆ rem) :
    (done ∪ ready) ∪ (rem \ ready) = done ∪ rem := by
  rw [Finset.union_assoc, union_sdiff_of_subset h]

/-- Pairwise-ness across an append: every element of the first list
relates to every element of the second. -/
private theorem pairwise_append {R : α → α → Prop} : ∀ {l₁ l₂ : List α},
    (l₁ ++ l₂).Pairwise R → ∀ x ∈ l₁, ∀ y ∈ l₂, R x y := by
  intro l₁
  induction l₁ with
  | nil => intro l₂ _ x hx; simp at hx
  | cons a l ih =>
    intro l₂ h
    have h' : (a :: (l ++ l₂)).Pairwise R := h
    cases h' with
    | cons hhead htail =>
      intro x hx y hy
      rcases List.mem_cons.1 hx with rfl | hx'
      · exact hhead y (List.mem_append.2 (Or.inr hy))
      · exact ih htail x hx' y hy

/-! ### Finite graphs: every node having a successor yields a cycle -/

/-- Strengthening the step relation strengthens the transitive closure. -/
private theorem transGenMono {r p : α → α → Prop}
    (h : ∀ x y, r x y → p x y) {x y : α} (ht : Relation.TransGen r x y) :
    Relation.TransGen p x y := by
  induction ht with
  | single hr => exact Relation.TransGen.single (h _ _ hr)
  | tail _ h' ih => exact Relation.TransGen.tail ih (h _ _ h')

/-- A classical successor function; `succOf rem R n` picks some `d` with
`R n d ∧ d ∈ rem` when one exists (used only where one does). -/
private noncomputable def succOf (rem : Finset α) (R : α → α → Prop) (n : α) : α :=
  if h : ∃ d, R n d ∧ d ∈ rem then h.choose else n

private theorem succOf_spec {rem : Finset α} {R : α → α → Prop}
    (hsucc : ∀ n ∈ rem, ∃ d, R n d ∧ d ∈ rem) {n : α} (hn : n ∈ rem) :
    R n (succOf rem R n) ∧ succOf rem R n ∈ rem := by
  by_cases h : ∃ d, R n d ∧ d ∈ rem
  · unfold succOf
    rw [dif_pos h]
    exact h.choose_spec
  · exact absurd (hsucc n hn) h

/-- `walk f n₀ k` — the `k`-th iterate of `f` starting at `n₀`. -/
private def walk (f : α → α) (n₀ : α) : ℕ → α
  | 0 => n₀
  | k + 1 => f (walk f n₀ k)

/-- Along a walk whose steps satisfy `R`, any positive stretch is an
`R`-transitive chain: pigeonhole material for cycles. -/
private theorem walk_chain {R : α → α → Prop} (f : α → α) (n₀ : α)
    (hstep : ∀ k, R (walk f n₀ k) (walk f n₀ (k + 1))) (start : ℕ) :
    ∀ k, 0 < k → Relation.TransGen R (walk f n₀ start) (walk f n₀ (start + k)) := by
  intro k
  induction k with
  | zero => intro hk; exact absurd hk (by omega)
  | succ k ih =>
    intro _
    by_cases hk0 : k = 0
    · subst hk0
      exact Relation.TransGen.single (hstep start)
    · exact Relation.TransGen.tail (ih (by omega)) (hstep (start + k))

/-- Pigeonhole: on a finite set where every element has an `R`-successor
inside the set, an `R`-cycle exists. -/
private theorem exists_cycle_of_succ {rem : Finset α} {R : α → α → Prop}
    (hrem : rem.Nonempty) (hsucc : ∀ n ∈ rem, ∃ d, R n d ∧ d ∈ rem) :
    ∃ x, x ∈ rem ∧ Relation.TransGen R x x := by
  obtain ⟨n₀, hn₀⟩ := hrem
  have hmem : ∀ k, walk (succOf rem R) n₀ k ∈ rem := by
    intro k
    induction k with
    | zero => exact hn₀
    | succ k ih => exact (succOf_spec hsucc ih).2
  have hstep : ∀ k, R (walk (succOf rem R) n₀ k) (walk (succOf rem R) n₀ (k + 1)) :=
    fun k => (succOf_spec hsucc (hmem k)).1
  have himg : Finset.image (walk (succOf rem R) n₀) (Finset.range (rem.card + 1)) ⊆ rem := by
    intro x hx
    rw [Finset.mem_image] at hx
    obtain ⟨k, _, rfl⟩ := hx
    exact hmem k
  have hle : (Finset.image (walk (succOf rem R) n₀) (Finset.range (rem.card + 1))).card
      ≤ rem.card := Finset.card_le_card himg
  by_contra hno
  have hinj : Set.InjOn (walk (succOf rem R) n₀) ↑(Finset.range (rem.card + 1)) := by
    intro a _ha b _hb hab
    rcases Nat.lt_trichotomy a b with hlt | heq | hgt
    · have hchain := walk_chain (succOf rem R) n₀ hstep a (b - a) (by omega)
      rw [show a + (b - a) = b from by omega,
        show walk (succOf rem R) n₀ b = walk (succOf rem R) n₀ a from hab.symm] at hchain
      exact absurd ⟨walk (succOf rem R) n₀ a, hmem a, hchain⟩ hno
    · exact heq
    · have hchain := walk_chain (succOf rem R) n₀ hstep b (a - b) (by omega)
      rw [show b + (a - b) = a from by omega,
        show walk (succOf rem R) n₀ a = walk (succOf rem R) n₀ b from hab] at hchain
      exact absurd ⟨walk (succOf rem R) n₀ b, hmem b, hchain⟩ hno
  have hcard : (Finset.image (walk (succOf rem R) n₀) (Finset.range (rem.card + 1))).card
      = (Finset.range (rem.card + 1)).card := Finset.card_image_of_injOn hinj
  rw [Finset.card_range] at hcard
  omega

/-! ### Correctness of the levelization -/

/-- Full specification of a successful run, by induction on the fuel. -/
private theorem levelsAux_spec (deps : α → Finset α) :
    ∀ (fuel : ℕ) (remaining done : Finset α) (lvls : List (Finset α)),
      remaining.card ≤ fuel →
      levelsAux deps fuel remaining done = some lvls →
      (∀ n ∈ done ∪ remaining, deps n ⊆ done ∪ remaining) →
      unionAll lvls = remaining ∧
      (∀ L ∈ lvls, L ⊆ remaining) ∧
      lvls.Pairwise (fun L₁ L₂ => ∀ x ∈ L₁, x ∉ L₂) ∧
      (∀ pre L rest, lvls = pre ++ L :: rest → ∀ x ∈ L, deps x ⊆ done ∪ unionAll pre) := by
  intro fuel
  induction fuel with
  | zero =>
    intro remaining done lvls _hcard hsome _hinv
    rw [levelsAux] at hsome
    by_cases hrem : remaining = ∅
    · rw [if_pos hrem] at hsome
      injection hsome with hlvls
      subst hlvls
      subst hrem
      refine ⟨?_, ?_, ?_, ?_⟩
      · rfl
      · intro L hL; simp at hL
      · exact List.Pairwise.nil
      · intro pre L rest hsplit; simp at hsplit
    · rw [if_neg hrem] at hsome
      exact absurd hsome.symm (Option.some_ne_none lvls)
  | succ fuel IH =>
    intro remaining done lvls _hcard hsome hinv
    rw [levelsAux] at hsome
    by_cases hrem : remaining = ∅
    · rw [if_pos hrem] at hsome
      injection hsome with hlvls
      subst hlvls
      subst hrem
      refine ⟨?_, ?_, ?_, ?_⟩
      · rfl
      · intro L hL; simp at hL
      · exact List.Pairwise.nil
      · intro pre L rest hsplit; simp at hsplit
    · rw [if_neg hrem] at hsome
      by_cases hready : readySet deps remaining done = ∅
      · rw [if_pos hready] at hsome
        exact absurd hsome.symm (Option.some_ne_none lvls)
      · rw [if_neg hready] at hsome
        have hsub : readySet deps remaining done ⊆ remaining := Finset.filter_subset _ _
        have hdec : (remaining \ readySet deps remaining done).card < remaining.card :=
          card_sdiff_lt remaining (readySet deps remaining done) hsub hready
        cases hrec : levelsAux deps fuel (remaining \ readySet deps remaining done)
            (done ∪ readySet deps remaining done) with
        | none =>
          rw [hrec] at hsome
          exact absurd hsome.symm (Option.some_ne_none lvls)
        | some rest =>
          rw [hrec] at hsome
          have hsome' : some (readySet deps remaining done :: rest) = some lvls := hsome
          injection hsome' with hlvls
          subst hlvls
          have hinv' : ∀ n ∈ (done ∪ readySet deps remaining done) ∪
              (remaining \ readySet deps remaining done),
              deps n ⊆ (done ∪ readySet deps remaining done) ∪
                (remaining \ readySet deps remaining done) := by
            rw [union_split remaining done (readySet deps remaining done) hsub]
            exact hinv
          obtain ⟨hcover, hsubl, hpw, hdeps⟩ :=
            IH (remaining \ readySet deps remaining done) (done ∪ readySet deps remaining done)
              rest (by omega) hrec hinv'
          refine ⟨?_, ?_, ?_, ?_⟩
          · show readySet deps remaining done ∪ unionAll rest = remaining
            rw [hcover, union_sdiff_of_subset hsub]
          · intro L hL
            rcases List.mem_cons.1 hL with rfl | hL'
            · exact hsub
            · intro x hx
              exact Finset.sdiff_subset (hsubl L hL' hx)
          · refine List.Pairwise.cons ?_ hpw
            intro L' hL' x hx hxL'
            exact (Finset.mem_sdiff.1 (hsubl L' hL' hxL')).2 hx
          · intro pre L rest' hsplit x hx
            cases pre with
            | nil =>
              have hsplit' : readySet deps remaining done :: rest = L :: rest' := hsplit
              injection hsplit' with hL _
              subst hL
              have hdx := (Finset.mem_filter.1 hx).2
              intro d hd
              have hd' : d ∈ done := hdx hd
              simpa [unionAll] using hd'
            | cons h pre' =>
              have hsplit' : readySet deps remaining done :: rest
                  = h :: (pre' ++ L :: rest') := hsplit
              injection hsplit' with hhead htail
              subst hhead
              have hd := hdeps pre' L rest' htail x hx
              show deps x ⊆ done ∪ (readySet deps remaining done ∪ unionAll pre')
              rw [← Finset.union_assoc]
              exact hd

/-- The `none` exit: fuel is never the reason, so a real cycle exists. -/
private theorem levelsAux_none_cycle (deps : α → Finset α) :
    ∀ (fuel : ℕ) (remaining done : Finset α),
      remaining.card ≤ fuel →
      levelsAux deps fuel remaining done = none →
      remaining.Nonempty →
      (∀ n ∈ done ∪ remaining, deps n ⊆ done ∪ remaining) →
      ∃ x, x ∈ remaining ∧ Relation.TransGen (fun a b => b ∈ deps a ∧ b ∈ remaining) x x := by
  intro fuel
  induction fuel with
  | zero =>
    intro remaining done hcard _hnone hrem _hinv
    exact absurd (Finset.card_pos.2 hrem) (by omega)
  | succ fuel IH =>
    intro remaining done hcard hnone hrem hinv
    rw [levelsAux] at hnone
    by_cases hremE : remaining = ∅
    · rw [if_pos hremE] at hnone
      exact absurd hnone (Option.some_ne_none [])
    · rw [if_neg hremE] at hnone
      by_cases hready : readySet deps remaining done = ∅
      · -- no module is ready: every remaining module has a remaining dependency
        have hsucc : ∀ n ∈ remaining, ∃ d, (d ∈ deps n ∧ d ∈ remaining) ∧ d ∈ remaining := by
          intro n hn
          have hnready : n ∉ readySet deps remaining done := fun hcontra =>
            (Finset.eq_empty_iff_forall_notMem.1 hready n) hcontra
          have hnd : ¬(deps n ⊆ done) := fun hsub => hnready (Finset.mem_filter.2 ⟨hn, hsub⟩)
          obtain ⟨d, hddep, hddone⟩ := Finset.not_subset.1 hnd
          have hdrem : d ∈ remaining := by
            rcases Finset.mem_union.1 ((hinv n (Finset.mem_union.2 (Or.inr hn))) hddep) with
              h | h
            · exact absurd h hddone
            · exact h
          exact ⟨d, ⟨hddep, hdrem⟩, hdrem⟩
        exact exists_cycle_of_succ hrem hsucc
      · -- ready nonempty: the none came from the recursive call
        have hsub : readySet deps remaining done ⊆ remaining := Finset.filter_subset _ _
        have hdec : (remaining \ readySet deps remaining done).card < remaining.card :=
          card_sdiff_lt remaining (readySet deps remaining done) hsub hready
        have hrem' : (remaining \ readySet deps remaining done).Nonempty := by
          by_contra hne
          have hE : remaining \ readySet deps remaining done = ∅ := by
            rcases Classical.em (remaining \ readySet deps remaining done = ∅) with h | h
            · exact h
            · exact absurd (Finset.nonempty_iff_ne_empty.2 h) hne
          have hbad : some (readySet deps remaining done :: [] : List (Finset α)) = none := by
            rw [hE, levelsAux_empty, if_neg hready] at hnone
            exact hnone
          exact absurd hbad (Option.some_ne_none _)
        cases hrec : levelsAux deps fuel (remaining \ readySet deps remaining done)
            (done ∪ readySet deps remaining done) with
        | none =>
          rw [hrec] at hnone
          have hinv' : ∀ n ∈ (done ∪ readySet deps remaining done) ∪
              (remaining \ readySet deps remaining done),
              deps n ⊆ (done ∪ readySet deps remaining done) ∪
                (remaining \ readySet deps remaining done) := by
            rw [union_split remaining done (readySet deps remaining done) hsub]
            exact hinv
          obtain ⟨x, hx, hcyc⟩ :=
            IH (remaining \ readySet deps remaining done) (done ∪ readySet deps remaining done)
              (by omega) hrec hrem' hinv'
          exact ⟨x, Finset.sdiff_subset hx,
            transGenMono (fun _ _ hab => ⟨hab.1, Finset.sdiff_subset hab.2⟩) hcyc⟩
        | some rest =>
          have hbad : some (readySet deps remaining done :: rest) = none := by
            rw [hrec, if_neg hready] at hnone
            exact hnone
          exact absurd hbad (Option.some_ne_none _)

/-! ### Public theorems -/

variable {deps : α → Finset α} {S : Finset α} {lvls : List (Finset α)}

private theorem top_inv (hS : ∀ n ∈ S, deps n ⊆ S) :
    ∀ n ∈ ∅ ∪ S, deps n ⊆ ∅ ∪ S := by simpa using hS

/-- Every module is placed in some level: `build` compiles the whole
project. -/
theorem levels_cover (hS : ∀ n ∈ S, deps n ⊆ S) (h : levels deps S = some lvls) :
    unionAll lvls = S :=
  (levelsAux_spec deps S.card S ∅ lvls (Nat.le_refl _) h (top_inv hS)).1

/-- Levels only contain project modules. -/
theorem levels_level_subset (hS : ∀ n ∈ S, deps n ⊆ S) (h : levels deps S = some lvls) :
    ∀ L ∈ lvls, L ⊆ S :=
  (levelsAux_spec deps S.card S ∅ lvls (Nat.le_refl _) h (top_inv hS)).2.1

/-- Levels are pairwise disjoint: no module is scheduled twice. -/
theorem levels_disjoint (hS : ∀ n ∈ S, deps n ⊆ S) (h : levels deps S = some lvls) :
    lvls.Pairwise (fun L₁ L₂ => ∀ x ∈ L₁, x ∉ L₂) :=
  (levelsAux_spec deps S.card S ∅ lvls (Nat.le_refl _) h (top_inv hS)).2.2.1

/-- Build soundness: every dependency of a module in level `L` lies in the
union of the *strictly earlier* levels (`pre`), so it has already been
compiled when `L` starts. -/
theorem levels_deps_earlier (hS : ∀ n ∈ S, deps n ⊆ S) (h : levels deps S = some lvls)
    (pre rest : List (Finset α)) (L : Finset α) (hsplit : lvls = pre ++ L :: rest)
    {x : α} (hx : x ∈ L) {d : α} (hd : d ∈ deps x) :
    d ∈ unionAll pre := by
  have hsub := (levelsAux_spec deps S.card S ∅ lvls (Nat.le_refl _) h (top_inv hS)).2.2.2
  have hdsub := hsub pre L rest hsplit x hx
  have hd' : d ∈ ∅ ∪ unionAll pre := hdsub hd
  simpa using hd'

/-- Parallel safety: no member of a level depends on another member of the
same level, so compiling a level concurrently is sound. -/
theorem levels_same_level_indep (hS : ∀ n ∈ S, deps n ⊆ S) (h : levels deps S = some lvls)
    (pre rest : List (Finset α)) (L : Finset α) (hsplit : lvls = pre ++ L :: rest)
    {x : α} (hx : x ∈ L) {d : α} (hd : d ∈ deps x) : d ∉ L := by
  have hdp := levels_deps_earlier hS h pre rest L hsplit hx hd
  obtain ⟨L', hL', hdL'⟩ := mem_unionAll.1 hdp
  have hpw := levels_disjoint hS h
  rw [hsplit] at hpw
  have hcross := pairwise_append hpw
  exact hcross L' hL' L List.mem_cons_self d hdL'

/-- The "import cycle among …" report is never spurious: `none` certifies
an actual dependency cycle in the module set. -/
theorem levels_none_cycle (hS : ∀ n ∈ S, deps n ⊆ S) (h : levels deps S = none)
    (hSne : S.Nonempty) :
    ∃ x, x ∈ S ∧ Relation.TransGen (fun a b => b ∈ deps a ∧ b ∈ S) x x :=
  levelsAux_none_cycle deps S.card S ∅ (Nat.le_refl _) h hSne (top_inv hS)

/-- Acyclic dependency graphs always levelize: `build` never gets stuck. -/
theorem levels_exists_of_acyclic (hS : ∀ n ∈ S, deps n ⊆ S)
    (hacy : ∀ x, ¬Relation.TransGen (fun a b => b ∈ deps a ∧ b ∈ S) x x) :
    ∃ lvls, levels deps S = some lvls := by
  rcases Classical.em (S = ∅) with h | h
  · subst h
    exact ⟨[], by simp [levels, levelsAux_empty]⟩
  · cases hEq : levels deps S with
    | none =>
      obtain ⟨x, _hx, hcyc⟩ := levels_none_cycle hS hEq (Finset.nonempty_iff_ne_empty.2 h)
      exact absurd hcyc (hacy x)
    | some lvls => exact ⟨lvls, rfl⟩

end
