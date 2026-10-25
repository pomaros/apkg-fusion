/* PkgManager rc1 over the network, one phase per call:
 *
 *     rx nettest.rexx <run> <phase>
 *
 * The window under test is already running (run-on-pool.sh starts it); this
 * script drives it through its ARexx port, so every operation runs on the
 * window's own worker, the same process the buttons use. Each phase writes
 * RESULTS:<run>-<phase>.txt: the run id, VERSION (with the binary's
 * SHA-256), one PASS or FAIL line per check, a transcript, and SUMMARY.
 * The checks are made here against the package root itself; grade.py adds
 * what only the host can see.
 *
 * Roots, one per window:
 *   SYS:RcNet  the real catalogue: Update, then installs from AROS Archives
 *              (gmore) and GitHub Releases (micropolis) on an empty cache;
 *   SYS:RcTls  a local index (make-tls-index.py): tlsbad points at a server
 *              with an expired certificate, gmore is the real entry;
 *   SYS:RcEnt  empty, for the build that forces the entropy source to fail.
 *
 * RC is never tested (see tests/arexx/rxtest.rexx: the Regina in AROS One
 * 1.3 misreports it); RESULT is. */
PARSE ARG run phase .
OPTIONS RESULTS
apkg = 'RAM:rxt/apkg'
root = 'SYS:RcNet'
IF wordpos(phase, 'tls recover quittls gonetls') > 0 THEN root = 'SYS:RcTls'
IF wordpos(phase, 'entfail quitent goneent') > 0 THEN root = 'SYS:RcEnt'
out = 'RESULTS:'run'-'phase'.txt'
passes = 0; fails = 0
CALL say1 'run' run 'phase' phase 'root' root
noport = left(phase, 4) = 'gone'
IF SHOW('P', 'PKGMANAGER') = 0 & \noport THEN DO
  CALL check 0, 'port-present', 'no public port PKGMANAGER'
  CALL finish
  EXIT 10
END
IF \noport THEN DO
  IF ask('VERSION') THEN CALL say1 'version' answer
END

SELECT
  WHEN phase = 'update'   THEN CALL p_update
  WHEN phase = 'archives' THEN CALL p_install 'gmore', 'zip'
  WHEN phase = 'github'   THEN CALL p_install 'micropolis', 'lha'
  WHEN phase = 'tls'      THEN CALL p_tls
  WHEN phase = 'recover'  THEN CALL p_install 'gmore', 'zip'
  WHEN phase = 'entfail'  THEN CALL p_entfail
  WHEN left(phase, 4) = 'quit' THEN CALL p_quit
  WHEN left(phase, 4) = 'gone' THEN CALL p_gone
  OTHERWISE CALL check 0, 'phase', 'unknown phase' phase
END
CALL finish
EXIT 0

/* ------------------------------------------------------------ phases */

/* The real catalogue, fetched by the window's worker over HTTPS. */
p_update:
  CALL check \exists(root'/db/index.json'), 'no-index-before', ''
  CALL expect 'UPDATE', 'OK', 'update-accepted'
  id = answer
  CALL expect 'WAIT' id, 'OK', 'update-wait'
  CALL check field(answer, 'state') = 'done' & field(answer, 'op') = 'update', 'update-done', answer
  CALL check exists(root'/db/index.json') & \exists(root'/db/index.json.part'), 'index-written', ''
  CALL expect 'LIST', 'OK', 'list-after-update'
  CALL check words(answer) >= 20 & wordpos('gmore', answer) > 0 & wordpos('micropolis', answer) > 0, 'catalogue-has-test-packages', words(answer) 'ids'
  CALL expect 'INFO gmore FIELD state', 'OK', 'gmore-available'
  CALL check answer = 'available', 'gmore-not-installed-yet', answer
  RETURN

/* Download, verify, install, on a cache that does not hold the archive. */
p_install:
  PARSE ARG pkg, ext
  arc = root'/cache/'pkg'.'ext
  CALL check \exists(arc) & \exists(root'/'pkg), 'cold-cache-'pkg, ''
  CALL expect 'INSTALL' pkg, 'OK', 'install-'pkg'-accepted'
  id = answer
  CALL expect 'WAIT' id, 'OK', 'install-'pkg'-wait'
  CALL check field(answer, 'state') = 'done' & field(answer, 'op') = 'install' & field(answer, 'origin') = 'arexx', 'install-'pkg'-done', answer
  CALL expect 'INFO' pkg 'FIELD state', 'OK', 'info-'pkg
  CALL check answer = 'installed', 'state-installed-'pkg, answer
  /* the download happened: the archive is in the cache, no partial file */
  CALL check exists(arc) & \exists(arc'.part'), 'downloaded-'pkg, ''
  CALL check exists(root'/'pkg) & exists(root'/db/installed/'pkg'.json'), 'drawer-and-registry-'pkg, ''
  /* the CLI of the same release checks every installed file against the
     registry, which records the archive's verified contents */
  ADDRESS COMMAND apkg '--root' root 'verify' pkg '>RAM:rxt-verify-'pkg
  v = readall('RAM:rxt-verify-'pkg)
  CALL say1 'verify:' translate(v, '|', '0a'x)
  PARSE VAR v vid': 'nok' as installed, 'rest
  CALL check vid = pkg & datatype(nok, 'W') & nok > 0 & pos(', 0 changed locally, 0 missing, 0 not the package', v) > 0, 'verify-'pkg, v
  ADDRESS COMMAND 'List >RAM:rxt-cache-'pkg root'/cache'
  CALL say1 'cache:' translate(readall('RAM:rxt-cache-'pkg), '|', '0a'x)
  RETURN

/* A certificate the client must refuse; nothing may change. */
p_tls:
  CALL check exists(root'/db/index.json'), 'tls-index-present', ''
  CALL expect 'INSTALL tlsbad', 'OK', 'tlsbad-accepted'
  id = answer
  CALL expect 'WAIT' id, 'OK', 'tlsbad-wait'
  st = field(answer, 'state'); msg = field(answer, 'message')
  CALL say1 'job:' translate(answer, '|', '0a'x)
  CALL check st = 'failed', 'tlsbad-failed', answer
  CALL check pos('certificate', msg) > 0 & pos('expired', msg) > 0, 'tlsbad-says-certificate-expired', msg
  CALL expect 'INFO tlsbad FIELD state', 'OK', 'tlsbad-info'
  CALL check answer = 'available', 'tlsbad-not-installed', answer
  CALL check \exists(root'/tlsbad') & \exists(root'/db/installed/tlsbad.json'), 'no-drawer-no-registry', ''
  CALL check \exists(root'/cache/tlsbad.zip') & \exists(root'/cache/tlsbad.zip.part'), 'no-download-left', ''
  CALL check \exists(root'/db/lock.owner'), 'lock-released', ''
  /* the window still answers, and a second attempt fails the same way */
  CALL expect 'INSTALL tlsbad', 'OK', 'tlsbad-again-accepted'
  CALL expect 'WAIT' answer 'FIELD state', 'OK', 'tlsbad-again-wait'
  CALL check answer = 'failed', 'tlsbad-again-failed', answer
  RETURN

/* The build with the forced entropy failure: no connection, no index. */
p_entfail:
  CALL check \exists(root'/db/index.json'), 'no-index-before', ''
  CALL expect 'UPDATE', 'OK', 'update-accepted'
  id = answer
  CALL expect 'WAIT' id, 'OK', 'update-wait'
  CALL say1 'job:' translate(answer, '|', '0a'x)
  CALL check field(answer, 'state') = 'failed', 'update-failed', answer
  CALL check pos('randomness', field(answer, 'message')) > 0, 'says-no-randomness', field(answer, 'message')
  CALL check \exists(root'/db/index.json') & \exists(root'/db/index.json.part'), 'nothing-written', ''
  CALL check \exists(root'/db/lock.owner'), 'lock-released', ''
  CALL expect 'LIST', 'NOINDEX', 'still-no-catalogue'
  /* responsive: it takes and ends another job */
  CALL expect 'UPDATE', 'OK', 'second-update-accepted'
  CALL expect 'WAIT' answer 'FIELD state', 'OK', 'second-update-wait'
  CALL check answer = 'failed', 'second-update-failed', answer
  RETURN

/* Close the window; the worker must end with it. */
p_quit:
  CALL expect 'QUIT', 'OK', 'quit-accepted'
  CALL check answer = 'closing', 'quit-closing', answer
  RETURN

p_gone:
  DO i = 1 TO 40 WHILE SHOW('P', 'PKGMANAGER')
    ADDRESS COMMAND 'Wait 1'
  END
  CALL check SHOW('P', 'PKGMANAGER') = 0, 'port-gone', ''
  ADDRESS COMMAND 'Wait 2'
  ADDRESS COMMAND 'Status >RAM:rxt-status-'phase
  s = readall('RAM:rxt-status-'phase)
  CALL say1 'status:' translate(s, '|', '0a'x)
  /* the window's Shell process; the worker is not a Shell process, and the
     window leaves only after it (docs/arexx.md, QUIT) -- the lock says so */
  CALL check pos('PkgManager', s) = 0, 'no-window-process-left', s
  CALL check \exists(root'/db/lock.owner'), 'no-lock-left', root
  RETURN

/* ------------------------------------------------------------ helpers
   The same as tests/arexx/rxtest.rexx. */

ask: PROCEDURE EXPOSE answer error code out
  PARSE ARG line
  DROP result
  ADDRESS PKGMANAGER line
  IF symbol('RESULT') = 'VAR' THEN DO
    answer = result; error = ''; code = 'OK'
    CALL say1 '  >' line
    CALL say1 '  <' translate(left(answer, min(length(answer), 160)), '|', '0a'x)
    RETURN 1
  END
  answer = ''
  IF SHOW('P', 'PKGMANAGER') THEN DO
    ADDRESS PKGMANAGER 'LASTERROR'
    error = result
  END
  ELSE error = 'NOPORT PKGMANAGER is not there'
  IF error = '' THEN error = 'NOREPLY no error was recorded'
  code = word(error, 1)
  CALL say1 '  >' line
  CALL say1 '  !' error
  RETURN 0

expect: PROCEDURE EXPOSE answer error code out passes fails
  PARSE ARG line, want, name
  CALL ask line
  CALL check code = want, name, 'sent ['line'] wanted' want 'got' code '['error']'
  RETURN

check: PROCEDURE EXPOSE out passes fails
  PARSE ARG ok, name, detail
  IF ok = 1 THEN DO; passes = passes + 1; CALL lineout out, 'PASS' name; END
  ELSE DO; fails = fails + 1; CALL lineout out, 'FAIL' name ':' translate(detail, '|', '0a'x); END
  RETURN

say1: PROCEDURE EXPOSE out
  CALL lineout out, arg(1)
  RETURN

finish:
  CALL lineout out, 'SUMMARY' phase 'pass' passes 'fail' fails
  CALL lineout out
  RETURN

field: PROCEDURE
  PARSE ARG text, name
  DO WHILE text <> ''
    PARSE VAR text l '0a'x text
    IF word(l, 1) = name THEN RETURN subword(l, 2)
  END
  RETURN ''

exists: PROCEDURE
  RETURN stream(arg(1), 'C', 'QUERY EXISTS') <> ''

readall: PROCEDURE
  PARSE ARG f
  s = ''
  IF stream(f, 'C', 'QUERY EXISTS') = '' THEN RETURN ''
  DO WHILE lines(f) > 0
    IF s = '' THEN s = linein(f); ELSE s = s || '0a'x || linein(f)
  END
  CALL stream f, 'C', 'CLOSE'
  RETURN s
