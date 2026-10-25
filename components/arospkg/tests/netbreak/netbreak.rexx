/* A download whose connection breaks, made deterministic with the test
 * option --break-download-at (net.c): it fails the next 2xx download once
 * that many body bytes have arrived: the TLS adapter's next receive fails
 * as a reset socket (or, with --corrupt-download-at, gets corrupted data),
 * so the error takes the same path as a real one. Then it disarms itself.
 * One phase per call:
 *
 *     rx netbreak.rexx <run> <phase>
 *
 * Writes RESULTS:<run>-<phase>.txt with PASS/FAIL lines and SUMMARY, like
 * tests/rc1net/nettest.rexx. CLI phases use RAM:rxt/apkg on SYS:NbCli; GUI
 * phases drive the PkgManager window the driver started on SYS:NbGui. */
PARSE ARG run phase .
OPTIONS RESULTS
apkg = 'RAM:rxt/apkg'
out = 'RESULTS:'run'-'phase'.txt'
passes = 0; fails = 0
CALL say1 'run' run 'phase' phase
SELECT
  WHEN phase = 'cli'   THEN CALL p_cli
  WHEN phase = 'gui'   THEN CALL p_gui
  WHEN phase = 'gone'  THEN CALL p_gone
  OTHERWISE CALL check 0, 'phase', 'unknown phase' phase
END
CALL finish
EXIT 0

p_cli:
  root = 'SYS:NbCli'
  CALL cli 'version', '--version'
  CALL cli 'update', '--root' root 'update'
  CALL check exists(root'/db/index.json'), 'index-fetched', ''
  /* a break in the archive download */
  r = cli('break', '--root' root '--break-download-at 100000 install micropolis')
  CALL check pos('cannot fetch the archive', r) > 0, 'break-reported-as-fetch-failure', r
  CALL check pos('broke after', r) > 0 & pos('of 2593505 bytes', r) > 0, 'break-says-bytes-of-total', r
  CALL check pos('connection reset', r) > 0, 'break-says-connection-reset', r
  CALL check pos('wrong size', r) = 0, 'not-reported-as-wrong-size', r
  CALL check \exists(root'/cache/micropolis.lha') & \exists(root'/cache/micropolis.lha.part'), 'nothing-in-cache', ''
  CALL check \exists(root'/micropolis') & \exists(root'/db/installed/micropolis.json'), 'not-installed', ''
  CALL check \exists(root'/db/lock.owner'), 'lock-released', ''
  /* a TLS error, not a socket one: corrupted data in the same download */
  r = cli('corrupt', '--root' root '--corrupt-download-at 100000 install micropolis')
  CALL check pos('broke after', r) > 0 & pos('TLS error -0x', r) > 0, 'corrupt-says-tls-error', r
  CALL check pos('connection reset', r) = 0 & pos('socket error', r) = 0, 'corrupt-not-a-socket-error', r
  CALL check \exists(root'/cache/micropolis.lha') & \exists(root'/cache/micropolis.lha.part') & \exists(root'/db/installed/micropolis.json'), 'corrupt-nothing-kept', ''
  CALL check \exists(root'/db/lock.owner'), 'corrupt-lock-released', ''
  /* the next attempt, without the option, ends the stream normally */
  r = cli('retry', '--root' root 'install micropolis')
  CALL check pos('installed micropolis', r) > 0, 'retry-installs', r
  CALL check exists(root'/cache/micropolis.lha') & \exists(root'/cache/micropolis.lha.part'), 'retry-cached', ''
  /* a break in the index download leaves the index as it was */
  ADDRESS COMMAND 'Copy' root'/db/index.json RAM:nb-index-before'
  r = cli('index-break', '--root' root '--break-download-at 1000 update')
  CALL check pos('broke after', r) > 0 & pos('connection reset', r) > 0, 'index-break-reported', r
  /* byte for byte, which says more than a hash of each would */
  CALL check same(root'/db/index.json', 'RAM:nb-index-before'), 'index-unchanged', ''
  ADDRESS COMMAND 'List' root'/db/index.json RAM:nb-index-before LFORMAT="%n %l" >RAM:nb-index-sizes'
  CALL say1 'index sizes:' translate(readall('RAM:nb-index-sizes'), '|', '0a'x)
  CALL check \exists(root'/db/index.json.part'), 'no-partial-index', ''
  /* a cancel in the middle of a transfer still says cancelled */
  r = cli('cancel', '--root' root '--slow 100 --cancel-at download-mid install folio')
  CALL check pos('cancelled', r) > 0 & pos('broke', r) = 0, 'cancel-still-cancel', r
  CALL check \exists(root'/cache/Folio-0.4.x86_64-aros-v11.lha.part') & \exists(root'/cache/folio.lha.part') & \exists(root'/cache/folio.lha'), 'cancel-nothing-cached', ''
  RETURN

/* The window was started with --break-download-at 50000. soliton (224 KB)
   keeps the window's downloads short; each job's time is logged. */
p_gui:
  root = 'SYS:NbGui'
  IF SHOW('P', 'PKGMANAGER') = 0 THEN DO; CALL check 0, 'port-present', ''; RETURN; END
  IF ask('VERSION') THEN CALL say1 'version' answer
  /* the driver copied the index in, so the first download is the archive */
  CALL check exists(root'/db/index.json'), 'gui-index-present', ''
  t0 = time('E')
  CALL ask 'INSTALL soliton'; id = answer
  CALL check datatype(id, 'W'), 'gui-install-accepted', id
  CALL ask 'WAIT' id
  CALL say1 'job:' translate(answer, '|', '0a'x) 'seconds' format(time('E') - t0,, 1)
  CALL check field(answer, 'op') = 'install' & field(answer, 'state') = 'failed', 'gui-break-failed', answer
  m = field(answer, 'message')
  CALL check pos('connection reset', m) > 0 & pos('broke after', m) > 0, 'gui-break-message', m
  CALL check \exists(root'/cache/soliton.zip') & \exists(root'/cache/soliton.zip.part') & \exists(root'/db/installed/soliton.json'), 'gui-nothing-in-cache', ''
  CALL check \exists(root'/db/lock.owner'), 'gui-lock-released', ''
  ADDRESS COMMAND 'Copy RAM:nb-pm.log RAM:nb-pm-after-break.log'
  /* the same window takes the next install; the break has disarmed */
  t0 = time('E')
  CALL ask 'INSTALL soliton'; id2 = answer
  CALL check datatype(id2, 'W') & id2 <> id, 'gui-retry-accepted', id2
  CALL ask 'WAIT' id2
  CALL say1 'retry:' translate(answer, '|', '0a'x) 'seconds' format(time('E') - t0,, 1)
  CALL check field(answer, 'op') = 'install' & field(answer, 'state') = 'done', 'gui-retry-done', answer
  CALL check exists(root'/soliton') & exists(root'/db/installed/soliton.json'), 'gui-retry-installed', ''
  CALL ask 'QUIT'
  CALL check answer = 'closing', 'gui-quit', answer
  RETURN

p_gone:
  DO i = 1 TO 40 WHILE SHOW('P', 'PKGMANAGER')
    ADDRESS COMMAND 'Wait 1'
  END
  CALL check SHOW('P', 'PKGMANAGER') = 0, 'port-gone', ''
  CALL check \exists('SYS:NbGui/db/lock.owner'), 'no-lock-left', ''
  RETURN

/* ------------------------------------------------------------ helpers */

cli: PROCEDURE EXPOSE apkg run out
  PARSE ARG name, args
  f = 'RAM:nb-'name
  ADDRESS COMMAND apkg args '>'f
  r = readall(f)
  CALL say1 '$ apkg' args
  CALL say1 translate(r, '|', '0a'x)
  ADDRESS COMMAND 'Copy' f 'RESULTS:'run'-cli-'name'.txt'
  RETURN r

same: PROCEDURE
  RETURN readall(arg(1)) == readall(arg(2)) & readall(arg(1)) <> ''

ask: PROCEDURE EXPOSE answer out
  PARSE ARG line
  DROP result
  ADDRESS PKGMANAGER line
  IF symbol('RESULT') = 'VAR' THEN answer = result
  ELSE answer = ''
  CALL say1 '  >' line
  CALL say1 '  <' translate(left(answer, min(length(answer), 200)), '|', '0a'x)
  RETURN answer <> ''

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
