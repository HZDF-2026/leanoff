"""Golden values for test_refdata.cpp: report rendering with canned results."""

import json


class R:
    def __init__(self, name, ok, errors, warnings, sorries, seconds, first_error=""):
        self.__dict__.update(name=name, ok=ok, errors=errors, warnings=warnings,
                              sorries=sorries, seconds=seconds, first_error=first_error)


results = [
    R("A", True, 0, 0, 0, 0.05),
    R("Sub.C", False, 2, 1, 1, 1.25, "Sub\\C.lean:3:0: error: unknown identifier 'foo'"),
    R("中文.模块", False, 1, 0, 0, 10.0, "error: 中文消息 with trailing spaces   "),
    R("zz.last", True, 0, 3, 2, 3600.0),
]


def report(results, wall, fmt):
    results = sorted(results, key=lambda r: r.name)
    failed = [r for r in results if not r.ok]
    if fmt == "json":
        return json.dumps({
            "modules": [r.__dict__ for r in results],
            "failed": len(failed),
            "wall_seconds": round(wall, 1),
        }, indent=2)
    w = max((len(r.name) for r in results), default=8)
    lines = [f"{'module':<{w}}  {'status':<6} {'err':>3} {'warn':>4} {'sorry':>5} {'time':>7}"]
    for r in results:
        status = "PASS" if r.ok else "FAIL"
        lines.append(f"{r.name:<{w}}  {status:<6} {r.errors:>3} {r.warnings:>4} {r.sorries:>5} {r.seconds:>6.1f}s")
        if not r.ok and r.first_error:
            lines.append(f"    {r.first_error.strip()[:160]}")
    tot_e = sum(r.errors for r in results)
    tot_w = sum(r.warnings for r in results)
    tot_s = sum(r.sorries for r in results)
    lines.append(f"\n{len(results)} modules: {len(failed)} failed, {tot_e} errors, {tot_w} warnings, {tot_s} sorry  ({wall:.1f}s wall)")
    return "\n".join(lines)


print("=== TEXT wall=12.34 ===")
print(report(results, 12.34, "text"))
print("=== END ===")
print("=== JSON wall=12.34 ===")
print(report(results, 12.34, "json"))
print("=== END ===")

# empty results report
print("=== EMPTY TEXT wall=0.04 ===")
print(report([], 0.04, "text"))
print("=== END ===")
print("=== EMPTY JSON wall=0.04 ===")
print(report([], 0.04, "json"))
print("=== END ===")
