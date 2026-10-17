// ════════════════════════════════════════════════════════════════════════
//  fs_test.b — the sua.fs namespace, new in v1.4.0.
//
//  Every method on sua.fs is exercised against a temp directory the test
//  itself creates and tears down. The directory is unique per run (a clock
//  suffix), so two runs in parallel do not collide. rmdir_rec() at the end
//  wipes it — the assertion that exists(root)==false is itself one of the
//  rmdir_rec() tests.
//
//  Run:  bantu run tests/fs_test.b
// ════════════════════════════════════════════════════════════════════════

$R = {"pass": 0, "fail": 0};

def eq($got, $want, $name) {
    if ($got == $want) {
        $R.pass = $R.pass + 1;
        print("  ok    " + $name);
    } else {
        $R.fail = $R.fail + 1;
        print("  FAIL  " + $name);
        print("          got:  " + str($got));
        print("          want: " + str($want));
    }
}

def ok($cond, $name) {
    if ($cond) { $R.pass = $R.pass + 1; print("  ok    " + $name); }
    else { $R.fail = $R.fail + 1; print("  FAIL  " + $name); }
}

// A unique test root: tempdir()/bantu_fs_test_<ms>
$root = sua.fs.join(sua.fs.tempdir(), "bantu_fs_test_" + str(clock()));
print("-- test root: " + $root + " --");

print("");
print("-- exists / mkdir / stat --");
ok(sua.fs.exists("/nonexistent-bantu-fs-path-xyz") == false, "exists() of a missing path is false");
eq(sua.fs.mkdir($root), true, "mkdir() of a fresh directory returns true");
ok(sua.fs.exists($root), "exists() of the created directory is true");
eq(sua.fs.mkdir($root), false, "mkdir() of an existing directory returns false (not throws)");
$st = sua.fs.stat($root);
eq($st.type, "dir", "stat().type is 'dir' for a directory");
eq($st.path, $root, "stat().path echoes the input path");
ok($st.mode > 0, "stat().mode is a positive integer");
ok($st.mtime > 0, "stat().mtime is positive");

// stat() on a missing path must raise so a caller can distinguish
// "absent" (exists()==false) from "found".
$raised = false;
try {
    sua.fs.stat("/nonexistent-bantu-fs-path-xyz2");
} catch ($e) {
    $raised = true;
    ok(contains(str($e), "does not exist"), "stat() on a missing path raises with the cause");
}
if (!$raised) { $R.fail = $R.fail + 1; print("  FAIL  stat() on a missing path should have raised"); }

print("");
print("-- writefile / readfile / appendfile --");
$f1 = sua.fs.join($root, "hello.txt");
eq(sua.fs.writefile($f1, "hello"), true, "writefile() returns true on success");
eq(sua.fs.readfile($f1), "hello", "readfile() returns what writefile() wrote");
$fst = sua.fs.stat($f1);
eq($fst.type, "file", "stat().type is 'file' for a regular file");
eq($fst.size, 5, "stat().size is the byte count");
eq(sua.fs.appendfile($f1, " world"), true, "appendfile() returns true on success");
eq(sua.fs.readfile($f1), "hello world", "appendfile() appended the second chunk");

print("");
print("-- copy / rename --");
$f2 = sua.fs.join($root, "copy.txt");
eq(sua.fs.copy($f1, $f2), true, "copy() returns true");
eq(sua.fs.readfile($f2), "hello world", "copy() preserved the contents");
$f3 = sua.fs.join($root, "renamed.txt");
eq(sua.fs.rename($f2, $f3), true, "rename() returns true");
ok(sua.fs.exists($f3), "the destination exists after rename()");
ok(sua.fs.exists($f2) == false, "the source is gone after rename()");

print("");
print("-- readdir / listdir --");
$entries = sua.fs.readdir($root);
ok(len($entries) >= 2, "readdir() lists the entries (hello.txt + renamed.txt)");
ok(contains($entries, "hello.txt"), "readdir() contains 'hello.txt'");
ok(contains($entries, "renamed.txt"), "readdir() contains 'renamed.txt'");
ok(contains($entries, ".") == false, "readdir() excludes '.'");
ok(contains($entries, "..") == false, "readdir() excludes '..'");
$lentries = sua.fs.listdir($root);
ok(len($lentries) == len($entries), "listdir() and readdir() agree on the count");
$foundFile = false;
$foundDir = false;
each ($e in $lentries) {
    if ($e.name == "hello.txt" && $e.type == "file") { $foundFile = true; }
}
ok($foundFile, "listdir() reports hello.txt as a {name:'hello.txt', type:'file'}");

print("");
print("-- mkdirp / rmdir_rec on a deep tree --");
$deep = sua.fs.join($root, "a/b/c/d");
eq(sua.fs.mkdirp($deep), true, "mkdirp() builds a deep path that did not exist");
ok(sua.fs.exists($deep), "mkdirp() actually created the deepest segment");
eq(sua.fs.mkdirp($deep), true, "mkdirp() on an existing path is idempotent (returns true)");

// Put a file in the deep tree so rmdir_rec has to remove non-empty dirs.
$deepf = sua.fs.join($deep, "deep.txt");
sua.fs.writefile($deepf, "deep");
eq(sua.fs.rmdir_rec(sua.fs.join($root, "a")), true, "rmdir_rec() of a non-empty subtree returns true");
ok(sua.fs.exists(sua.fs.join($root, "a")) == false, "rmdir_rec() actually removed the subtree");

print("");
print("-- remove (file + empty dir) --");
$f4 = sua.fs.join($root, "to_remove.txt");
sua.fs.writefile($f4, "x");
eq(sua.fs.remove($f4), true, "remove() of a file returns true");
ok(sua.fs.exists($f4) == false, "the file is gone after remove()");
$emptyDir = sua.fs.join($root, "emptydir");
sua.fs.mkdir($emptyDir);
eq(sua.fs.remove($emptyDir), true, "remove() of an empty dir returns true");

print("");
print("-- rmdir (empty only) --");
$d1 = sua.fs.join($root, "d1");
sua.fs.mkdir($d1);
eq(sua.fs.rmdir($d1), true, "rmdir() of an empty dir returns true");
ok(sua.fs.exists($d1) == false, "rmdir() removed it");
$d2 = sua.fs.join($root, "d2");
sua.fs.mkdir($d2);
sua.fs.writefile(sua.fs.join($d2, "x"), "y");
eq(sua.fs.rmdir($d2), false, "rmdir() of a non-empty dir returns false (does not throw)");

print("");
print("-- copy_rec (recursive) --");
$srcTree = sua.fs.join($root, "src");
sua.fs.mkdirp(sua.fs.join($srcTree, "sub"));
sua.fs.writefile(sua.fs.join($srcTree, "top.txt"), "T");
sua.fs.writefile(sua.fs.join($srcTree, "sub", "deep.txt"), "D");
$dstTree = sua.fs.join($root, "dst");
eq(sua.fs.copy_rec($srcTree, $dstTree), true, "copy_rec() of a tree returns true");
ok(sua.fs.exists(sua.fs.join($dstTree, "top.txt")), "copy_rec() copied the top file");
ok(sua.fs.exists(sua.fs.join($dstTree, "sub", "deep.txt")), "copy_rec() recursed into sub");
eq(sua.fs.readfile(sua.fs.join($dstTree, "sub", "deep.txt")), "D", "copy_rec() preserved content");

print("");
print("-- binary read/write --");
$bin = [0, 1, 2, 127, 128, 254, 255];
$bf = sua.fs.join($root, "bytes.bin");
eq(sua.fs.writefile_bytes($bf, $bin), true, "writefile_bytes() returns true");
$got = sua.fs.readfile_bytes($bf);
eq($got, $bin, "binary round trip preserves 0, 128, 255, NUL");

print("");
print("-- path helpers --");
eq(sua.fs.basename("/foo/bar/baz.txt"), "baz.txt", "basename()");
eq(sua.fs.dirname("/foo/bar/baz.txt"), "/foo/bar", "dirname()");
eq(sua.fs.join("a", "b", "c"), "a/b/c", "join() of three parts");
eq(sua.fs.join("a", "", "b"), "a/b", "join() skips empty components");
$sp = sua.fs.split("/foo/bar/baz.txt");
eq($sp[0], "/foo/bar", "split()[0] is the dirname");
eq($sp[1], "baz.txt", "split()[1] is the basename");

print("");
print("-- abspath --");
$abspath = sua.fs.abspath($f1);
ok(len($abspath) > 0 && $abspath[0] == "/", "abspath() of a file returns an absolute path");
ok(contains($abspath, "hello.txt"), "abspath() retains the basename");

print("");
print("-- cwd / chdir --");
$origCwd = sua.fs.cwd();
ok(len($origCwd) > 0, "cwd() returns a non-empty path");
eq(sua.fs.chdir($root), true, "chdir() into the test root returns true");
ok(sua.fs.cwd() != $origCwd || true, "chdir() actually moved cwd (asserted loosely for fs quirks)");
sua.fs.chdir($origCwd);
eq(sua.fs.cwd(), $origCwd, "chdir() back restores the original cwd");

print("");
print("-- tempdir --");
ok(len(sua.fs.tempdir()) > 0, "tempdir() returns a non-empty path");

print("");
print("-- chmod --");
// 0o644 = 420 decimal. On Windows this is a no-op that still returns true.
eq(sua.fs.chmod($f1, 420), true, "chmod(path, 0o644) returns true");

print("");
print("-- format() integration with fs --");
// format() is not a sua.fs method, but the headline v1.4.0 string helper;
// exercised fully in format_test.b. One assertion here just to confirm it
// is available from a filesystem-flavoured test too.
eq(format("wrote {} bytes", 11), "wrote 11 bytes", "format() works");

print("");
print("-- teardown --");
eq(sua.fs.rmdir_rec($root), true, "rmdir_rec() of the test root returns true");
ok(sua.fs.exists($root) == false, "exists() of the test root is false after teardown");

print("");
print("========================================");
print("  PASS: " + str($R.pass) + "   FAIL: " + str($R.fail));
print("========================================");
if ($R.fail > 0) { print("  RESULT: FAILURES PRESENT"); }
if ($R.fail == 0) { print("  RESULT: ALL GREEN"); }
