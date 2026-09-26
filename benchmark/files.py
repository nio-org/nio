# The Python version of files.nio. Python's `re` is a backtracker. Nio's
# engine is an NFA simulation.
import os
import re as regexp
import shutil
import tempfile

n = 1000
lines = 400

doc = ""
k = 0
while k < lines:
    doc = doc + "field_" + str(k) + " = value_" + str(k) + "\n"
    k = k + 1
doc = doc + "counter = 0000\n"

dir = tempfile.mkdtemp(prefix="nio-bench-files-")
file = os.path.join(dir, "data.txt")
with open(file, "w") as f:
    f.write(doc)

re = regexp.compile("counter = [0-9]{4}")
checksum = 0
i = 0

while i < n:
    with open(file) as f:
        text = f.read()
    at = re.search(text).start()
    checksum = checksum + at

    v = int(text[at + 10:at + 14]) + 1
    num = "%04d" % v
    edited = text[:at + 10] + num + text[at + 14:]

    with open(file, "w") as f:
        f.write(edited)
    i = i + 1

with open(file) as f:
    text = f.read()
at = re.search(text).start()
counter = int(text[at + 10:at + 14])

shutil.rmtree(dir)
print(checksum + counter)
