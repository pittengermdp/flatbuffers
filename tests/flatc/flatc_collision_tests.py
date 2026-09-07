# Copyright 2026 Google Inc. All rights reserved.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

"""Tests for --output-manifest, which refuses a write that would overwrite
another schema's generated output.

Most languages name their output from the schema's BASENAME -- and, for
TypeScript, additionally from its namespace -- but never from the schema's
directory. Two schemas in different directories that share a basename therefore
resolve to the same output path, and since flatc is normally invoked once per
schema, the second invocation silently overwrites the first. Both exit 0.

collision/alpha/thing.fbs and collision/beta/thing.fbs are that pair.
"""

from pathlib import Path
import shutil

from flatc_test import (
    assert_file_exists,
    flatc,
    flatc_fails,
    make_absolute,
    script_path,
)

ALPHA = "collision/alpha/thing.fbs"
BETA = "collision/beta/thing.fbs"
UNRELATED_NESTED = "nsbarrel/unrelated.fbs"
REAL_CHILD = "nsbarrel/real_child.fbs"


def _fresh(name):
  """An empty output directory, plus the manifest path inside it."""
  out = Path(script_path, "collision", name)
  shutil.rmtree(out, ignore_errors=True)
  out.mkdir(parents=True, exist_ok=True)
  return str(out), str(Path(out, "manifest.txt"))


class OutputCollisionTests:

  def RustCollisionFailsAndNamesBothSchemas(self):
    out, manifest = _fresh("out_rust")

    flatc(["--rust", "--output-manifest", manifest, "-o", out, ALPHA])
    assert_file_exists("thing_generated.rs", out)

    stderr = flatc_fails(
        ["--rust", "--output-manifest", manifest, "-o", out, BETA]
    )

    # Naming only one side would leave whoever hits this hunting for the other.
    assert "two schemas generate the same output file" in stderr, stderr
    assert ALPHA in stderr, stderr
    assert BETA in stderr, stderr

  def TypeScriptCollisionFails(self):
    # TypeScript is the case worth pinning: its writers used to discard save
    # failures, so the collision was reported on stderr while flatc still
    # exited 0. flatc_fails asserts the status, not just the text.
    out, manifest = _fresh("out_ts")

    flatc(["--ts", "--output-manifest", manifest, "-o", out, ALPHA])
    stderr = flatc_fails(["--ts", "--output-manifest", manifest, "-o", out, BETA])

    assert "two schemas generate the same output file" in stderr, stderr

  def RegeneratingTheSameSchemaIsAllowed(self):
    # The manifest records provenance, not content: a schema rewriting its own
    # output is ordinary regeneration and must stay silent, or every second run
    # of a build would fail.
    out, manifest = _fresh("out_repeat")

    flatc(["--rust", "--output-manifest", manifest, "-o", out, ALPHA])
    flatc(["--rust", "--output-manifest", manifest, "-o", out, ALPHA])
    flatc(["--rust", "--output-manifest", manifest, "-o", out, ALPHA])

    assert_file_exists("thing_generated.rs", out)

  def WithoutTheFlagTheCollisionStillHappensSilently(self):
    # The check is opt-in. This pins the unguarded behaviour so the test suite
    # says out loud what the flag is protecting against: without it, the second
    # schema wins and nothing reports it.
    out, _ = _fresh("out_unguarded")

    flatc(["--rust", "-o", out, ALPHA])
    flatc(["--rust", "-o", out, BETA])

    generated = Path(out, "thing_generated.rs").read_text()
    assert "BetaThing" in generated, "the later schema should have won"
    assert "AlphaThing" not in generated, (
        "the earlier schema's types should be gone -- that is the bug"
    )

  def IdenticalContentFromTwoSchemasIsAllowed(self):
    # The check is about a schema's types being silently REPLACED, not about two
    # schemas touching one path. A schema that includes another re-emits the
    # included namespace's barrel byte-for-byte, and TypeScript does this
    # constantly -- ln2/cdo.fbs includes ln2/value.fbs and rewrites an identical
    # value.ts. Rejecting that made the flag unusable on a real schema tree.
    #
    # nsbarrel/unrelated.fbs includes nsbarrel/parent.fbs but declares a
    # namespace of its own, so it re-emits parent.ts unchanged. (A schema that
    # EXTENDS the namespace, like real_child.fbs, genuinely changes the barrel by
    # adding its own child export -- that is a real difference, not a duplicate.)
    out, manifest = _fresh("out_identical")

    flatc(["--ts", "--output-manifest", manifest, "-o", out, "nsbarrel/parent.fbs"])
    # Would raise CalledProcessError if the duplicate write were refused.
    flatc(["--ts", "--output-manifest", manifest, "-o", out, UNRELATED_NESTED])

    assert Path(out, "parent.ts").exists()

  def OneSchemaWritingAPathTwiceDoesNotPoisonTheRecord(self):
    # A single schema can write one path more than once in a run: value.fbs-style
    # schemas emit <name>.ts as both the schema barrel and the namespace barrel.
    # The manifest has to remember the LAST of those, because that is the file
    # left on disk -- comparing a later schema against the first one rejected an
    # identical file.
    out, manifest = _fresh("out_rewrite")

    flatc(["--ts", "--output-manifest", manifest, "-o", out, "nsbarrel/parent.fbs"])
    recorded = Path(manifest).read_text()
    on_disk_len = len(Path(out, "parent.ts").read_bytes())

    # The manifest digest is "<length>-<hash>"; the length must be the file's.
    line = [l for l in recorded.splitlines() if l.endswith("parent.fbs")][0]
    digest = line.split("\t")[1]
    assert digest.startswith(str(on_disk_len) + "-"), (
        f"manifest recorded {digest} but parent.ts is {on_disk_len} bytes"
    )

  def NamespaceDirectoryLanguagesAreNotFalselyAccused(self):
    # Go names its output from the namespace, so the same basename pair does NOT
    # collide there. Reporting one would be a false positive that blocks a
    # legitimate build.
    out, manifest = _fresh("out_go")

    flatc(["--go", "--output-manifest", manifest, "-o", out, ALPHA])
    flatc(["--go", "--output-manifest", manifest, "-o", out, BETA])

    assert Path(out, "Alpha").is_dir(), "expected a namespace directory for Alpha"
    assert Path(out, "Beta").is_dir(), "expected a namespace directory for Beta"

  def StaleManifestAdviceIsInTheMessage(self):
    # A renamed or deleted schema leaves an entry behind that can accuse a
    # newcomer which legitimately took over the name. The way out has to be in
    # the message, because the manifest is not a file anyone thinks to look for.
    out, manifest = _fresh("out_stale")

    flatc(["--rust", "--output-manifest", manifest, "-o", out, ALPHA])
    stderr = flatc_fails(
        ["--rust", "--output-manifest", manifest, "-o", out, BETA]
    )

    assert make_absolute(manifest) in stderr or manifest in stderr, stderr
    assert "delete" in stderr.lower(), stderr


class NamespaceBarrelTests:
  """A namespace barrel must re-export its own children and nothing else."""

  def UnrelatedNamespaceIsNotReExportedAsAChild(self):
    # The child test used to compare only DEPTH, so any namespace one level
    # deeper was treated as a child of every shallower one. Generating a schema
    # whose closure holds both `Parent` and the unrelated `Other.Child` put
    # `export * as Child` into parent.ts. In this repo that is how
    # `Reporting.UsageReport` ended up re-exported from usage.ts, service.ts and
    # traffic.ts, none of which own it.
    out, _ = _fresh("out_nsbarrel")

    flatc(["--ts", "-o", out, UNRELATED_NESTED])

    parent_barrel = Path(out, "parent.ts")
    assert parent_barrel.exists(), "expected a barrel for the Parent namespace"
    contents = parent_barrel.read_text()

    assert "Child" not in contents, (
        "Parent's barrel must not re-export the unrelated Other.Child: "
        + contents
    )
    assert "ParentThing" in contents, (
        "Parent's barrel should still export its own types: " + contents
    )

  def RealChildNamespaceIsStillReExported(self):
    # The fix must not throw the feature out with the bug: Parent.Real IS a
    # child of Parent and still belongs in Parent's barrel. Without this, a
    # prefix check that simply never matched would pass the test above.
    out, _ = _fresh("out_nsbarrel_child")

    flatc(["--ts", "-o", out, REAL_CHILD])

    contents = Path(out, "parent.ts").read_text()
    assert "export * as Real" in contents, (
        "Parent's barrel must still re-export its genuine child: " + contents
    )
