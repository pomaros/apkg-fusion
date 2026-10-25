/* `apkg show` acceptance: rx showtest.rexx <run> <phase>
 *
 * Phases, in the order run-show.sh calls them:
 *   cli     apkg show with no index, then over SHOWIDX1: a package not
 *           installed, unknown ids, other ABI, other CPU, an unknown ABI,
 *           two variants; then installed at another revision; then with the
 *           index gone. And that none of it changed anything.
 *   rexx    the same verdicts from ARexx INFO, with PkgManager running.
 *   noindex a PkgManager on a root with no index: LIST says NOINDEX; then
 *           an index whose ids, listed, exceed an ARexx string: TOOLONG.
 *
 * Every check is decided here; RESULTS:<run>-show-<phase>.txt carries PASS
 * and FAIL lines and what apkg printed. */
PARSE ARG run phase .
OPTIONS RESULTS
root = 'SYS:PkgShow'
apkg = 'RAM:rxt/apkg --root' root
out = 'RESULTS:'run'-show-'phase'.txt'
passes = 0; fails = 0
CALL lineout out, 'run' run 'phase' phase
IF phase = 'cli' THEN CALL p_cli
IF phase = 'rexx' THEN CALL p_rexx
IF phase = 'noindex' THEN CALL p_noindex
CALL lineout out, 'SUMMARY' phase 'pass' passes 'fail' fails
CALL lineout out
EXIT 0

p_cli:
  ADDRESS COMMAND 'RAM:rxt/apkg --version >RAM:sv'
  CALL lineout out, 'apkg' translate(readall('RAM:sv'), '|', '0a'x)
  t = show('soliton')
  CALL has t, 'no index, and no such package installed', 'no-index-unknown'
  ADDRESS COMMAND 'Copy RAM:rxt/SHOWIDX1' root'/db/index.json'
  /* the operations: a target that is not this machine's is refused before
     anything is fetched (their URLs do not resolve), and two builds for one
     target are refused rather than picked from */
  ADDRESS COMMAND apkg 'install armthing >RAM:sv'
  CALL has readall('RAM:sv'), 'wrong CPU for this system', 'install-other-cpu-refused'
  ADDRESS COMMAND apkg 'install oldthing >RAM:sv'
  CALL has readall('RAM:sv'), 'wrong ABI for this system', 'install-other-abi-refused'
  ADDRESS COMMAND apkg 'install futurething >RAM:sv'
  CALL has readall('RAM:sv'), 'wrong CPU for this system', 'install-i386-refused-on-cpu-first'
  ADDRESS COMMAND apkg 'install twin >RAM:sv'
  CALL has readall('RAM:sv'), 'more than one build of this package for this system', 'install-twin-refused'
  t = show('twin')
  CALL has t, 'ambiguous:    the index lists 2 builds of it for this system; install refuses', 'show-says-ambiguous'
  CALL check \exists(root'/cache/armthing.zip.part') & \exists(root'/cache/oldthing.zip') & \exists(root'/armthing'), 'refused-before-download', ''
  CALL keep 'b'
  t = show('soliton')
  CALL has t, 'target:       x86_64, ABI v11', 'target'
  CALL has t, 'compatibility: native -- built for this machine: x86_64, ABIv11', 'compatible-native'
  CALL has t, 'download:     219 KB from https://archives.arosworld.org/share/game/card/soliton', 'download'
  CALL has t, 'sha256:       62c9b8a9', 'sha256'
  CALL has t, 'requirements: satisfied, probed now:', 'requirements-summary'
  CALL has t, 'crt.library: satisfied', 'needs-probed'
  CALL has t, 'installed:    no', 'not-installed'
  CALL hasnot t, 'source:', 'no-source-when-none'
  t = show('nosuch')
  CALL has t, 'no such package in the index, and none installed', 'unknown-id'
  t = show('oldthing')
  CALL has t, 'compatibility: incompatible -- built for ABIv1, mainline AROS; this machine runs ABIv11', 'other-abi'
  CALL has t, 'requirements: not checked -- it is not compatible with this machine', 'other-abi-needs-not-probed'
  t = show('armthing')
  CALL has t, 'compatibility: incompatible -- built for aarch64; this machine is x86_64', 'other-cpu'
  t = show('futurething')
  CALL has t, 'compatibility: undetermined -- the catalogue gives no ABI this client knows ("v0")', 'unknown-abi'
  t = show('rescode')
  CALL has t, 'target:       x86_64, ABI v11 (the index lists: i386/v0, x86_64/v11)', 'variant-described'
  CALL has t, 'compatibility: native', 'variant-native'
  CALL has t, 'source:       https://example.invalid/rescode-source', 'source'
  CALL has t, 'license:      Freeware', 'license'
  CALL has t, 'crt.library: satisfied', 'needs-of-the-described-variant'
  ADDRESS COMMAND 'Copy RAM:rxt/SHOWIDX1R' root'/db/index.json'
  t2 = show('rescode')
  CALL check t2 = t, 'reversed-index-same-description', 'show rescode changed with the order of the index'
  ADDRESS COMMAND 'Copy RAM:rxt/SHOWIDX1' root'/db/index.json'
  ADDRESS COMMAND apkg 'search >RAM:sv'
  r = readall('RAM:sv')
  CALL has r, '4 package(s) hidden: built for another CPU or ABI', 'search-says-why-hidden'
  CALL hasnot r, 'oldthing', 'search-hides-other-targets'
  CALL check same_as_kept('b'), 'show-changes-nothing', 'the listing or the content of a file differs'
  /* installed at revision 1, while the index offers revision 2 */
  ADDRESS COMMAND apkg 'install soliton >RAM:sv'
  ADDRESS COMMAND 'Copy RAM:rxt/SHOWIDX2' root'/db/index.json'
  t = show('soliton')
  CALL has t, 'soliton 2.2-aros2', 'header-is-the-index-revision'
  CALL has t, 'installed:    2.2 (x86_64), in SYS:PkgShow/soliton, since 20', 'installed-where-when'
  CALL has t, 'upgrade:      available, 2.2 -> 2.2-aros2', 'upgrade-available'
  CALL has t, 'roll back:    no --', 'no-rollback'
  /* SHOWIDX2 also offers soliton revision 3 for i386/v0: the upgrade of the
     installed x86_64 build must stay on x86_64, in either order */
  ADDRESS COMMAND apkg '--dry-run upgrade soliton >RAM:sv'
  r = readall('RAM:sv')
  CALL has r, '2.2-aros2', 'upgrade-stays-on-target'
  CALL hasnot r, '2.2-aros3', 'upgrade-does-not-cross'
  ADDRESS COMMAND 'Copy RAM:rxt/SHOWIDX2R' root'/db/index.json'
  ADDRESS COMMAND apkg '--dry-run upgrade soliton >RAM:sv'
  r2 = readall('RAM:sv')
  CALL has r2, '2.2-aros2', 'upgrade-stays-on-target-reversed'
  CALL hasnot r2, '2.2-aros3', 'upgrade-does-not-cross-reversed'
  ADDRESS COMMAND 'Copy RAM:rxt/SHOWIDX2' root'/db/index.json'
  /* the i386 rescode comes first in the index; install must take the native one */
  ADDRESS COMMAND apkg 'install rescode >RAM:sv'
  ADDRESS COMMAND apkg 'info rescode >RAM:sv'
  r = readall('RAM:sv')
  CALL has r, '"arch":"x86_64"', 'install-took-the-native-variant'
  CALL has r, 'rescode.x86_64-aros-v11.zip', 'install-took-the-native-url'
  /* `info` is what it was: the registry entry */
  ADDRESS COMMAND apkg 'info soliton >RAM:sv'
  CALL has readall('RAM:sv'), '"name":"soliton"', 'info-unchanged'
  /* no index at all, the package installed */
  ADDRESS COMMAND 'Delete' root'/db/index.json QUIET'
  t = show('soliton')
  CALL has t, 'index:        none on this machine', 'no-index-installed'
  CALL has t, 'compatibility: native -- built for this machine: x86_64, ABIv11', 'no-index-verdict-from-registry'
  CALL has t, 'installed:    2.2 (x86_64)', 'no-index-installed-line'
  ADDRESS COMMAND 'Copy RAM:rxt/SHOWIDX2' root'/db/index.json'
  RETURN

p_rexx:
  CALL info 'soliton', 'compatibility', 'native'
  CALL info 'soliton', 'requirements', 'satisfied'
  CALL info 'oldthing', 'compatibility', 'incompatible'
  CALL info 'oldthing', 'requirements', 'not checked'
  CALL info 'armthing', 'compatibility', 'incompatible'
  CALL info 'futurething', 'compatibility', 'undetermined'
  CALL info 'rescode', 'abi', 'v11'
  CALL info 'rescode', 'compatibility', 'native'
  CALL ask 'SHOW soliton'
  RETURN

p_noindex:
  r = ask('LIST')
  CALL lineout out, '  > LIST <' r
  CALL check word(r, 1) = 'ERROR' & word(r, 2) = 'NOINDEX', 'list-without-index', r
  ADDRESS COMMAND 'Copy RAM:rxt/BIGIDX SYS:PkgEmpty/db/index.json'
  r = ask('LIST')
  CALL lineout out, '  > LIST <' left(r, 80)
  CALL check word(r, 1) = 'ERROR' & word(r, 2) = 'TOOLONG', 'list-too-long', left(r, 80)
  r = ask('LIST MATCH p0007-')
  CALL check words(r) = 1 & left(r, 6) = 'p0007-', 'list-narrowed-fits', left(r, 80)
  CALL ask 'QUIT'
  RETURN

/* ------------------------------------------------------------ helpers */

show: PROCEDURE EXPOSE apkg out
  PARSE ARG id
  ADDRESS COMMAND apkg 'show' id '>RAM:sv'
  t = readall('RAM:sv')
  CALL lineout out, '  $ apkg show' id
  DO WHILE t <> ''
    PARSE VAR t l '0a'x t
    CALL lineout out, '  |' l
  END
  RETURN readall('RAM:sv')

has: PROCEDURE EXPOSE out passes fails
  PARSE ARG text, want, name
  CALL check pos(want, text) > 0, name, 'wanted "'want'"'
  RETURN

hasnot: PROCEDURE EXPOSE out passes fails
  PARSE ARG text, want, name
  CALL check pos(want, text) = 0, name, 'did not want "'want'"'
  RETURN

info: PROCEDURE EXPOSE out passes fails
  PARSE ARG id, field, want
  got = ask('INFO' id 'FIELD' field)
  CALL lineout out, '  > INFO' id 'FIELD' field '<' got
  CALL check got = want, 'rexx-'id'-'field, 'wanted' want 'got' got
  RETURN

ask: PROCEDURE
  DROP result
  ADDRESS PKGMANAGER arg(1)
  IF symbol('RESULT') = 'VAR' THEN RETURN result
  ADDRESS PKGMANAGER 'LASTERROR'
  RETURN 'ERROR' result

check: PROCEDURE EXPOSE out passes fails
  PARSE ARG ok, name, detail
  IF ok = 1 THEN DO; passes = passes + 1; CALL lineout out, 'PASS' name; END
  ELSE DO; fails = fails + 1; CALL lineout out, 'FAIL' name ':' detail; END
  RETURN

readall: PROCEDURE
  PARSE ARG f
  s = ''
  IF stream(f, 'C', 'QUERY EXISTS') = '' THEN RETURN ''
  DO WHILE lines(f) > 0
    IF s = '' THEN s = linein(f); ELSE s = s || '0a'x || linein(f)
  END
  CALL stream f, 'C', 'CLOSE'
  RETURN s

/* Every file under the root and its whole content, kept by name to be
   compared later: a listing alone would miss a file rewritten in place, and
   the order List gives changes when a file is recreated. */
keep: PROCEDURE EXPOSE root kept.
  PARSE ARG tag
  ADDRESS COMMAND 'List >RAM:keep-'tag root 'ALL FILES LFORMAT="%p%n"'
  n = 0
  DO WHILE lines('RAM:keep-'tag) > 0
    f = linein('RAM:keep-'tag)
    IF f = '' THEN ITERATE
    n = n + 1; kept.tag.f = charin(f, 1, 1000000); kept.tag.f.seen = 1
    CALL stream f, 'C', 'CLOSE'
  END
  CALL stream 'RAM:keep-'tag, 'C', 'CLOSE'
  kept.tag.0 = n
  RETURN

same_as_kept: PROCEDURE EXPOSE root kept.
  PARSE ARG tag
  ADDRESS COMMAND 'List >RAM:keep-now' root 'ALL FILES LFORMAT="%p%n"'
  n = 0; ok = 1
  DO WHILE lines('RAM:keep-now') > 0
    f = linein('RAM:keep-now')
    IF f = '' THEN ITERATE
    n = n + 1
    IF symbol('kept.tag.f.seen') <> 'VAR' THEN ok = 0
    ELSE DO
      body = charin(f, 1, 1000000)
      IF body <> kept.tag.f THEN ok = 0
    END
    CALL stream f, 'C', 'CLOSE'
  END
  CALL stream 'RAM:keep-now', 'C', 'CLOSE'
  RETURN ok & n = kept.tag.0 & n > 0

exists: PROCEDURE
  RETURN stream(arg(1), 'C', 'QUERY EXISTS') <> ''
