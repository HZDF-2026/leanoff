import json

strs = [
    "simple",
    'with "quote"',
    "back\\slash",
    "tab\there",
    "nl\nhere",
    "ctrl\x01",
    "euro \u20ac",
    "emoji \U0001F600",
    "math \u00df und \u00e4",
    "cjk \u4e2d\u6587",
    "del\x7f",
    "",
]
print(json.dumps([json.dumps(s) for s in strs], ensure_ascii=True))
ints = ["1_0", " 4 ", "+4", "-4", "x", "1__0", "_1", "1_", "", "9999999999999999999999", "0", "  -12  "]
out = []
for x in ints:
    try:
        out.append(int(x))
    except ValueError:
        out.append(None)
print(json.dumps(out))
