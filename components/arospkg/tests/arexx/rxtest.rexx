/* PkgManager ARexx acceptance, one phase per call:
 *
 *     rx rxtest.rexx <run> <phase> [job]
 *
 * Every phase writes RESULTS:<run>-<phase>.txt: the run id, the answer to
 * VERSION (which carries the binary's SHA-256), then one line per check,
 * PASS or FAIL, and a transcript of every command sent and every answer.
 * The host grader reads those files; this script decides PASS and FAIL
 * itself, against what libpkg's own state says, so the files are verdicts
 * and not just logs.
 *
 * RC is never tested. The Regina in AROS One 1.3 (and every ABIv11 system
 * built before contrib 0258b5de4) puts a successful command's RESULT text in
 * RC and leaves RC empty after a failure. What holds on every version is:
 * RESULT is set after success and dropped after failure. ask() relies on
 * that, and asks LASTERROR for the reason. */
PARSE ARG run phase job .
OPTIONS RESULTS
IF phase = 'waitjob' & job = '' THEN job = readall('RAM:rxt-job')
root = 'SYS:PkgRx'
apkg = 'RAM:rxt/apkg'
out = 'RESULTS:'run'-'phase'.txt'
IF phase = 'waitjob' THEN out = 'RESULTS:'run'-waitjob-'job'.txt'   /* one per job: never overwrite on FAT */
passes = 0; fails = 0
CALL say1 'run' run 'phase' phase
noport = wordpos(phase, 'gone gone2 norexx') > 0      /* phases that expect no port */
IF SHOW('P', 'PKGMANAGER') = 0 & \noport THEN DO
  CALL check 0, 'port-present', 'no public port PKGMANAGER'
  CALL finish
  EXIT 10
END
IF \noport THEN DO
  IF ask('VERSION') THEN CALL say1 'version' answer
END

SELECT
  WHEN phase = 'env'         THEN CALL p_env
  WHEN phase = 'parse'       THEN CALL p_parse
  WHEN phase = 'stress'      THEN CALL p_stress
  WHEN phase = 'queries0'    THEN CALL p_queries0
  WHEN phase = 'show'        THEN CALL p_show
  WHEN phase = 'select'      THEN CALL expect 'SHOW soliton', 'OK', 'select'
  WHEN phase = 'installstart' THEN CALL p_installstart
  WHEN phase = 'installcheck' THEN CALL p_installcheck
  WHEN phase = 'remove'      THEN CALL p_remove
  WHEN phase = 'rapid'       THEN CALL p_rapid
  WHEN phase = 'spin'        THEN CALL p_spin
  WHEN phase = 'nomem2'      THEN CALL p_nomem2
  WHEN phase = 'nomem'       THEN CALL p_nomem
  WHEN phase = 'update'      THEN CALL p_update
  WHEN phase = 'guibusy'     THEN CALL p_guibusy
  WHEN phase = 'clilock'     THEN CALL p_clilock
  WHEN phase = 'upgradestart' THEN CALL p_upgradestart
  WHEN phase = 'waitjob'     THEN CALL p_waitjob
  WHEN phase = 'upgradecheck' THEN CALL p_upgradecheck
  WHEN phase = 'rollbackstart' THEN CALL p_rollbackstart
  WHEN phase = 'rollbackcheck' THEN CALL p_rollbackcheck
  WHEN phase = 'cancel'      THEN CALL p_cancel
  WHEN phase = 'quitstart'   THEN CALL p_quitstart
  WHEN phase = 'gone'        THEN CALL p_gone
  WHEN phase = 'gone2'       THEN CALL p_gone
  WHEN phase = 'collide'     THEN CALL p_collide
  WHEN phase = 'closestart'  THEN CALL p_closestart
  WHEN phase = 'norexx'      THEN CALL p_norexx
  OTHERWISE CALL check 0, 'phase', 'unknown phase' phase
END
CALL finish
EXIT 0

/* ------------------------------------------------------------ phases */

p_env:
  CALL check word(answer, 1) = 'PkgManager' & word(answer, 3) = 'ARexx' & word(answer, 4) = 1, 'version-interface', answer
  CALL check length(word(answer, 6)) = 64, 'version-sha256', word(answer, 6)
  IF ask('HELP') THEN NOP
  CALL check answer = 'VERSION HELP LIST INFO REQUIREMENTS SHOW UPDATE INSTALL REMOVE UPGRADE ROLLBACK JOB WAIT CANCEL QUIT LASTERROR', 'help', answer
  /* the helpers this script relies on, measured on this system */
  CALL say1 'exists dir' exists(root) 'file' exists(root'/db/index.json') 'none' exists(root'/nonexistent')
  CALL check exists(root) & exists(root'/db/index.json') & \exists(root'/nonexistent'), 'helper-exists', ''
  RETURN

p_parse:
  CALL expect 'FROB', 'BADCOMMAND', 'unknown-command'
  CALL expect 'IN soliton', 'BADCOMMAND', 'no-prefix-match'
  CALL expect 'INSTALL', 'BADARGS', 'missing-package'
  CALL expect 'INSTALL soliton extra', 'BADARGS', 'extra-argument'
  CALL expect 'INFO soliton FIELD colour', 'BADARGS', 'unknown-field'
  CALL expect 'INFO "soliton', 'BADARGS', 'unterminated-quote'
  CALL expect 'JOB 0', 'BADARGS', 'job-zero'
  CALL expect 'JOB', 'NOJOB', 'no-job-yet'
  CALL expect 'CANCEL 99', 'NOJOB', 'cancel-unknown-job'
  CALL expect 'INFO "sol iton"', 'NOTFOUND', 'space-in-name'
  CALL check pos('sol iton', error) > 0, 'space-in-name-echoed', error
  CALL expect 'INFO "a*"b;$x&y"', 'NOTFOUND', 'special-characters'
  CALL check pos('a"b;$x&y', error) > 0, 'special-characters-echoed', error
  CALL expect 'info soliton field STATE', 'OK', 'lower-case-and-field'
  CALL check answer = 'available', 'lower-case-and-field-value', answer
  CALL expect 'INFO PACKAGE=soliton FIELD=summary', 'OK', 'keyword-equals'
  CALL check answer = 'Klondike & Freecell card game', 'summary-with-ampersand', answer
  RETURN

/* A hundred failures and answers from inside a PROCEDURE: the pattern that
   took RexxMast down while PkgManager still set RC2 with SetRexxVar. */
p_stress:
  n = 0
  DO i = 1 TO 50
    IF ask('INFO nosuch') = 0 & code = 'NOTFOUND' THEN n = n + 1
    IF ask('INFO soliton FIELD state') = 1 & answer = 'available' THEN n = n + 1
  END
  CALL check n = 100, 'hundred-in-a-procedure', n
  CALL ask 'VERSION'
  IF ask('LASTERROR') THEN CALL check answer = '', 'lasterror-cleared-by-success', answer
  CALL ask 'INFO nosuch'
  IF ask('LASTERROR') THEN CALL check word(answer, 1) = 'NOTFOUND', 'lasterror-after-failure', answer
  IF ask('LASTERROR') THEN CALL check word(answer, 1) = 'NOTFOUND', 'lasterror-kept-by-itself', answer
  RETURN

p_queries0:
  CALL snapshot 'before'
  CALL expect 'LIST', 'OK', 'list'
  CALL check sortwords(answer) = 'cls soliton xrick', 'list-ids', answer
  CALL expect 'LIST INSTALLED', 'OK', 'list-installed-empty'
  CALL check answer = '', 'list-installed-empty-value', answer
  CALL expect 'LIST MATCH card', 'OK', 'list-match'
  CALL check answer = 'soliton', 'list-match-value', answer
  CALL expect 'LIST MATCH "no such thing"', 'OK', 'list-match-none'
  CALL check answer = '', 'list-match-none-value', answer
  CALL expect 'INFO soliton', 'OK', 'info-all-fields'
  CALL check field(answer, 'state') = 'available' & field(answer, 'version') = '2.2' & field(answer, 'id') = 'soliton', 'info-lines', answer
  CALL check field(answer, 'canupgrade') = 0 & field(answer, 'installedversion') = '', 'info-not-installed', answer
  CALL expect 'REQUIREMENTS soliton', 'OK', 'requirements'
  CALL check pos('crt.library: satisfied', answer) > 0 & pos('stdlib.library: satisfied', answer) > 0, 'requirements-probed', answer
  CALL expect 'REQUIREMENTS nosuch', 'NOTFOUND', 'requirements-unknown'
  CALL expect 'INFO nosuch', 'NOTFOUND', 'info-unknown'
  CALL snapshot 'after'
  CALL check readall('RAM:rxt-snap-before') = readall('RAM:rxt-snap-after'), 'queries-change-nothing', ''
  RETURN

p_show:
  CALL expect 'SHOW soliton', 'OK', 'show'
  CALL expect 'SHOW nosuch', 'NOTFOUND', 'show-unknown'
  CALL expect 'SHOW "sol iton"', 'NOTFOUND', 'show-space'
  RETURN

/* INSTALL, then everything that must be refused while it runs, then leave
   without waiting: the next phase is another client. */
p_installstart:
  CALL expect 'INSTALL soliton', 'OK', 'install-accepted'
  id = answer
  CALL check datatype(id, 'W') & id > 0, 'install-job-id', id
  CALL writefile 'RAM:rxt-job', id
  CALL expect 'INSTALL cls', 'BUSY', 'second-install-busy'
  CALL expect 'UPDATE', 'BUSY', 'update-busy'
  /* the extraction pause (--slow): the library no longer takes a cancel */
  ph = waitphase(id, 'extract', 40)
  CALL check ph = 'extract', 'reached-extract', ph
  CALL expect 'JOB' id 'FIELD cancancel', 'OK', 'cancancel-in-extract'
  CALL check answer = 0, 'cancancel-0-in-extract', answer
  CALL expect 'CANCEL' id, 'NOTCANCELLABLE', 'cancel-refused-in-extract'
  /* the CLI meets the same root lock */
  ADDRESS COMMAND apkg '--root' root 'install cls >RAM:rxt-cli-conflict'
  cli = readall('RAM:rxt-cli-conflict')
  CALL say1 'cli:' cli
  CALL check pos('another instance is running', cli) > 0 & pos('PkgManager worker', cli) > 0, 'cli-refused-by-lock', cli
  CALL expect 'JOB' id 'FIELD state', 'OK', 'job-still-running'
  CALL check answer = 'running', 'job-running-at-exit', answer
  RETURN

p_installcheck:
  id = readall('RAM:rxt-job')
  CALL expect 'WAIT' id, 'OK', 'wait-after-new-client'
  CALL check field(answer, 'state') = 'done' & field(answer, 'op') = 'install' & field(answer, 'origin') = 'arexx', 'install-done', answer
  CALL expect 'INFO soliton FIELD state', 'OK', 'info-after-install'
  CALL check answer = 'installed', 'state-installed', answer
  CALL expect 'LIST INSTALLED', 'OK', 'list-installed'
  CALL check answer = 'soliton', 'list-installed-value', answer
  CALL check exists(root'/soliton') & exists(root'/db/installed/soliton.json'), 'drawer-and-registry', ''
  CALL check \exists(root'/cls') & \exists(root'/db/installed/cls.json'), 'cli-installed-nothing', ''
  CALL clilist 'RAM:rxt-cli-list1'
  CALL check clihas('RAM:rxt-cli-list1', 'soliton') & \clihas('RAM:rxt-cli-list1', 'cls'), 'cli-list-agrees', readall('RAM:rxt-cli-list1')
  CALL expect 'CANCEL' id, 'FINISHED', 'cancel-after-finish'
  CALL expect 'INSTALL soliton', 'OK', 'install-again-accepted'
  id2 = answer
  CALL expect 'WAIT' id2 'FIELD error', 'OK', 'install-again-result'
  CALL check answer = 'EXISTS', 'install-again-exists', answer
  RETURN

p_remove:
  CALL expect 'REMOVE soliton', 'OK', 'remove-accepted'
  id = answer
  CALL expect 'WAIT' id, 'OK', 'remove-wait'
  CALL check field(answer, 'state') = 'done', 'remove-done', answer
  CALL expect 'INFO soliton FIELD state', 'OK', 'info-after-remove'
  CALL check answer = 'available', 'state-available', answer
  CALL check \exists(root'/soliton') & \exists(root'/db/installed/soliton.json'), 'drawer-and-registry-gone', ''
  CALL expect 'LIST INSTALLED', 'OK', 'list-installed-after-remove'
  CALL check answer = '', 'list-installed-empty-again', answer
  CALL expect 'REMOVE soliton', 'OK', 'remove-again-accepted'
  CALL expect 'WAIT' answer 'FIELD error', 'OK', 'remove-again-result'
  CALL check answer = 'NOTFOUND', 'remove-again-notfound', answer
  CALL expect 'REMOVE nosuch', 'NOTFOUND', 'remove-unknown'
  RETURN

/* A job, and the next one sent the moment the window will take it. The
   worker is held for 3 s between finishing and reporting (--slow), the gap
   where a new job could once be started and then be handed the old job's
   outcome. The script waits for the last phase, publish, then sends REMOVE
   once a second until it is taken. It must be taken only after the install
   is final, and both jobs must end with their own outcome. Not without a
   pause: a script and the window trading messages as fast as they can keep
   the worker, at a lower priority, from running at all (run AX3). */
p_rapid:
  ok = 0
  DO k = 1 TO 5
    CALL ask 'INSTALL cls'
    a = answer
    ph = waitphase(a, 'publish', 60)
    busy = 0; got = 0
    CALL time 'R'
    DO UNTIL got | time('E') > 60
      got = try('REMOVE cls')
      IF got = 0 THEN DO; busy = busy + 1; ADDRESS COMMAND 'Wait 1'; END
    END
    b = tried
    CALL ask 'JOB' a 'FIELD state'
    firstwhenaccepted = answer
    CALL ask 'WAIT' a
    wa = field(answer, 'state') field(answer, 'op') field(answer, 'package')
    CALL ask 'WAIT' b
    wb = field(answer, 'state') field(answer, 'op') field(answer, 'package')
    CALL say1 'round' k 'install' a '(seen in' ph') ->' wa '| remove' b '->' wb '| busy replies' busy '| install state when remove accepted:' firstwhenaccepted
    IF got & b = a + 1 & firstwhenaccepted = 'done' & wa = 'done install cls' & wb = 'done remove cls' & busy > 0 THEN ok = ok + 1
  END
  CALL check ok = 5, 'rapid-jobs-keep-their-outcomes', ok 'of 5 rounds'
  CALL expect 'INFO cls FIELD state', 'OK', 'rapid-end-state'
  CALL check answer = 'available', 'rapid-cls-removed', answer
  CALL check \exists(root'/cls') & \exists(root'/db/installed/cls.json'), 'rapid-files-agree', ''
  RETURN

/* A script that polls without any pause, which is what a careless script
   does. The install must still finish (the worker must get the CPU), and
   the window must still act on its own input meanwhile: the host searches
   for "cls" and clears the search during the loop, which the window's log
   records. */
p_spin:
  CALL ask 'INSTALL cls'
  a = answer
  n = 0; st = ''
  CALL time 'R'
  DO UNTIL rx_final(st) | time('E') > 120
    IF try('JOB' a 'FIELD state') THEN st = tried
    n = n + 1
  END
  secs = time('E')
  CALL say1 'polled' n 'times without a pause over' format(secs, , 1) 's; final state' st
  CALL check st = 'done', 'install-finishes-under-polling', st 'after' n 'polls'
  CALL check n > 100, 'really-without-pause', n 'polls'
  CALL expect 'INFO cls FIELD state', 'OK', 'spin-installed'
  CALL check answer = 'installed', 'spin-installed-value', answer
  CALL expect 'REMOVE cls', 'OK', 'spin-cleanup'
  CALL expect 'WAIT' answer 'FIELD state', 'OK', 'spin-cleanup-wait'
  CALL check answer = 'done', 'spin-cleanup-done', answer
  RETURN

/* A PkgManager started with --test-nomem prepare: the answer of a command
   that changes something cannot be allocated. It must be refused with
   nothing done -- no job, nothing installed, the window still there. */
p_nomem2:
  CALL expect 'INSTALL cls', 'NOMEM', 'job-answer-unallocatable'
  CALL check pos('nothing was started', error) > 0, 'says-nothing-started', error
  CALL expect 'JOB', 'NOJOB', 'no-job-was-started'
  CALL expect 'INFO cls FIELD state', 'OK', 'nomem2-info'
  CALL check answer = 'available' & \exists(root'/cls'), 'nothing-installed', answer
  CALL expect 'QUIT', 'NOMEM', 'quit-answer-unallocatable'
  ADDRESS COMMAND 'Wait 3'
  CALL check SHOW('P', 'PKGMANAGER') = 1, 'window-did-not-quit', ''
  CALL expect 'INFO soliton FIELD state', 'OK', 'still-answering'
  RETURN

/* A PkgManager started with --test-nomem start and result: the worker's
   final message and every answer of 64 bytes or more cannot be allocated. */
p_nomem:
  CALL expect 'VERSION', 'NOMEM', 'long-answer-fails'
  CALL check pos('no memory', error) > 0, 'long-answer-says-why', error
  CALL expect 'INFO soliton FIELD state', 'OK', 'short-answer-goes-out'
  before = answer
  CALL expect 'INSTALL cls', 'CANNOTSTART', 'no-final-message-no-job'
  CALL expect 'JOB FIELD state', 'OK', 'refused-job-state'
  CALL check answer = 'failed', 'refused-job-failed', answer
  CALL expect 'JOB FIELD error', 'OK', 'refused-job-error'
  CALL check answer = 'CANNOTSTART', 'refused-job-error-value', answer
  CALL expect 'INFO cls FIELD state', 'OK', 'nothing-installed-without-memory'
  CALL check answer = 'available' & \exists(root'/cls'), 'nothing-installed-value', answer
  CALL expect 'INSTALL cls', 'CANNOTSTART', 'still-refused-not-stuck'
  RETURN

/* UPDATE, all the way: the public catalogue, fetched over the network. */
p_update:
  CALL expect 'UPDATE', 'OK', 'update-accepted'
  id = answer
  CALL expect 'WAIT' id, 'OK', 'update-wait'
  CALL check field(answer, 'state') = 'done' & field(answer, 'op') = 'update', 'update-done', answer
  CALL expect 'LIST', 'OK', 'list-after-update'
  CALL say1 'catalogue' sortwords(answer)
  CALL check words(answer) >= 20 & wordpos('grafx2', answer) > 0 & wordpos('folio', answer) > 0, 'public-catalogue-listed', words(answer) 'ids'
  CALL expect 'INFO grafx2 FIELD version', 'OK', 'info-from-new-catalogue'
  CALL check answer = '2.9', 'grafx2-version', answer
  CALL expect 'INFO soliton FIELD state', 'OK', 'installed-survives-update'
  CALL check answer = 'installed', 'installed-survives-update-value', answer
  RETURN

/* The window started an install (the host pressed the button). */
p_guibusy:
  CALL expect 'JOB', 'OK', 'job-latest'
  CALL check field(answer, 'origin') = 'window' & field(answer, 'op') = 'install' & field(answer, 'package') = 'soliton', 'window-job-visible', answer
  CALL check field(answer, 'state') = 'running', 'window-job-running', answer
  id = field(answer, 'id')
  CALL expect 'INSTALL cls', 'BUSY', 'arexx-busy-during-window-job'
  CALL check pos('job' id, error) > 0, 'busy-names-the-job', error
  CALL expect 'WAIT' id, 'OK', 'wait-window-job'
  CALL check field(answer, 'state') = 'done', 'window-job-done', answer
  CALL expect 'INFO soliton FIELD state', 'OK', 'installed-by-window'
  CALL check answer = 'installed', 'installed-by-window-value', answer
  RETURN

/* The CLI holds the root; an operation from ARexx must fail on the lock. */
p_clilock:
  ADDRESS COMMAND 'Run >RAM:rxt-cli-hold' apkg '--root' root '--slow 500 install cls'
  ADDRESS COMMAND 'Wait 3'
  CALL expect 'REMOVE soliton', 'OK', 'remove-while-cli-holds'
  id = answer
  CALL expect 'WAIT' id, 'OK', 'wait-locked'
  CALL check field(answer, 'state') = 'failed' & field(answer, 'error') = 'LOCKED', 'failed-locked', answer
  CALL check exists(root'/soliton'), 'soliton-kept', ''
  ADDRESS COMMAND 'Wait 15'
  CALL clilist 'RAM:rxt-cli-list2'
  CALL check clihas('RAM:rxt-cli-list2', 'cls'), 'cli-install-finished', readall('RAM:rxt-cli-list2')
  CALL expect 'INFO cls FIELD state', 'OK', 'arexx-sees-cli-install'
  CALL check answer = 'installed', 'arexx-sees-cli-install-value', answer
  RETURN

/* Upgrade: the plan waits for the user, BUSY meanwhile; CANCEL declines. */
p_upgradestart:
  ADDRESS COMMAND 'Copy RAM:rxt/IDX2' root'/db/index.json'
  ADDRESS COMMAND 'Copy RAM:rxt/SOLR2' root'/cache/soliton.new.zip'
  CALL expect 'INFO soliton', 'OK', 'info-before-upgrade'
  CALL check field(answer, 'canupgrade') = 1, 'canupgrade', answer
  before = field(answer, 'installedrevision')
  CALL expect 'UPGRADE soliton', 'OK', 'upgrade-accepted'
  id = answer
  st = waitstate(id, 'confirm', 60)
  CALL check st = 'confirm', 'upgrade-waits-for-confirmation', st
  CALL expect 'JOB' id 'FIELD message', 'OK', 'plan-message'
  CALL check pos('added', answer) > 0 & pos('replaced', answer) > 0, 'plan-in-message', answer
  CALL expect 'INSTALL cls', 'BUSY', 'busy-while-confirming'
  CALL expect 'CANCEL' id, 'OK', 'cancel-declines'
  CALL check answer = 'declined', 'cancel-declines-value', answer
  CALL expect 'JOB' id 'FIELD state', 'OK', 'declined-state'
  CALL check answer = 'declined', 'declined-state-value', answer
  CALL expect 'INFO soliton FIELD installedrevision', 'OK', 'nothing-changed'
  CALL check answer = before, 'revision-unchanged-after-decline', before '->' answer
  /* again, and leave it for the user: the host presses Proceed */
  CALL expect 'UPGRADE soliton', 'OK', 'upgrade-again'
  id = answer
  st = waitstate(id, 'confirm', 60)
  CALL check st = 'confirm', 'upgrade-again-confirm', st
  CALL writefile 'RAM:rxt-job', id
  RETURN

/* Started with Run: parks a WAIT until the job is final, then records it. */
p_waitjob:
  CALL expect 'WAIT' job, 'OK', 'wait-parked'
  CALL writefile 'RAM:rxt-wait-'job, field(answer, 'state') field(answer, 'error')
  CALL check rx_final(field(answer, 'state')), 'wait-returned-final', answer
  RETURN

p_upgradecheck:
  id = readall('RAM:rxt-job')
  w = readall('RAM:rxt-wait-'id)
  CALL check word(w, 1) = 'done', 'parked-wait-saw-done', w
  CALL expect 'JOB' id 'FIELD state', 'OK', 'upgrade-state'
  CALL check answer = 'done', 'upgrade-done', answer
  CALL expect 'INFO soliton', 'OK', 'info-after-upgrade'
  CALL check field(answer, 'installedrevision') = 2 & field(answer, 'canrollback') = 1, 'upgraded-to-r2', answer
  CALL check exists(root'/soliton/NEWS'), 'r2-file-present', ''
  RETURN

p_rollbackstart:
  CALL expect 'ROLLBACK soliton', 'OK', 'rollback-accepted'
  id = answer
  st = waitstate(id, 'confirm', 60)
  CALL check st = 'confirm', 'rollback-waits-for-confirmation', st
  CALL writefile 'RAM:rxt-job', id
  RETURN

p_rollbackcheck:
  id = readall('RAM:rxt-job')
  CALL expect 'WAIT' id, 'OK', 'rollback-wait'
  CALL check field(answer, 'state') = 'done', 'rollback-done', answer
  CALL expect 'INFO soliton', 'OK', 'info-after-rollback'
  CALL check field(answer, 'installedrevision') = 0 & field(answer, 'canrollback') = 0, 'back-to-r1', answer
  CALL check \exists(root'/soliton/NEWS'), 'r2-file-gone', ''
  RETURN

/* Cancel during the download, which is where the library takes it. */
p_cancel:
  CALL expect 'INSTALL xrick', 'OK', 'download-install-accepted'
  id = answer
  ph = waitphase(id, 'download', 90)
  CALL check ph = 'download', 'reached-download', ph
  IF ph <> 'download' THEN DO
    CALL expect 'WAIT' id, 'OK', 'download-failed-wait'
    CALL say1 'download did not start:' answer
    RETURN
  END
  CALL expect 'JOB' id 'FIELD cancancel', 'OK', 'cancancel-in-download'
  CALL check answer = 1, 'cancancel-1-in-download', answer
  CALL expect 'CANCEL' id, 'OK', 'cancel-accepted'
  CALL check answer = 'requested', 'cancel-requested', answer
  CALL expect 'WAIT' id, 'OK', 'cancel-wait'
  CALL check field(answer, 'state') = 'cancelled' & field(answer, 'error') = 'CANCELLED', 'cancelled', answer
  CALL expect 'INFO xrick FIELD state', 'OK', 'info-after-cancel'
  CALL check answer = 'available', 'not-installed-after-cancel', answer
  CALL check \exists(root'/xrick') & \exists(root'/cache/xrick.zip.part') & \exists(root'/cache/xrick.zip'), 'nothing-left-after-cancel', ''
  CALL expect 'CANCEL' id, 'FINISHED', 'cancel-twice'
  RETURN

/* QUIT while an install runs: the window leaves only after the job ends. */
p_quitstart:
  CALL expect 'REMOVE cls', 'OK', 'remove-cls'
  CALL expect 'WAIT' answer 'FIELD state', 'OK', 'remove-cls-wait'
  CALL expect 'INSTALL cls', 'OK', 'install-before-quit'
  id = answer
  CALL writefile 'RAM:rxt-job', id
  ADDRESS COMMAND 'Run >NIL: rx RAM:rxt/rxtest.rexx' run 'waitjob' id
  ADDRESS COMMAND 'Wait 2'
  CALL expect 'QUIT', 'OK', 'quit-accepted'
  CALL expect 'INSTALL soliton', 'CLOSING', 'refused-while-closing'
  RETURN

p_gone:
  id = readall('RAM:rxt-job')
  DO i = 1 TO 40 WHILE SHOW('P', 'PKGMANAGER')
    ADDRESS COMMAND 'Wait 1'
  END
  CALL check SHOW('P', 'PKGMANAGER') = 0, 'port-gone-after-quit', ''
  w = readall('RAM:rxt-wait-'id)
  CALL check word(w, 1) = 'done', 'wait-got-final-before-quit', w
  CALL clilist 'RAM:rxt-cli-list3'
  CALL check clihas('RAM:rxt-cli-list3', 'cls'), 'job-finished-despite-quit', readall('RAM:rxt-cli-list3')
  RETURN

/* Two windows: the second has no port, the first still answers. */
p_collide:
  CALL check word(answer, 1) = 'PkgManager', 'first-still-answers', answer
  CALL check pos('already in use', readall('RAM:rxt-pm3.log')) > 0, 'second-says-port-in-use', ''
  RETURN

/* Close gadget during an install started from ARexx. */
p_closestart:
  CALL expect 'REMOVE cls', 'OK', 'remove-cls-2'
  CALL expect 'WAIT' answer 'FIELD state', 'OK', 'remove-cls-2-wait'
  CALL expect 'INSTALL cls', 'OK', 'install-before-close'
  id = answer
  CALL writefile 'RAM:rxt-job', id
  ADDRESS COMMAND 'Run >NIL: rx RAM:rxt/rxtest.rexx' run 'waitjob' id
  RETURN

p_norexx:
  CALL check SHOW('P', 'PKGMANAGER') = 0, 'no-port-without-rexxsyslib', ''
  CALL check pos('no ARexx port', readall('RAM:rxt-pm4.log')) > 0, 'window-says-no-port', ''
  RETURN

/* ------------------------------------------------------------ helpers */

/* Send one command. 1 on success with `answer` = RESULT; 0 on failure with
   `error` = LASTERROR's "CODE text" and `code` its first word. */
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

/* A command without the transcript, for loops: 1 when accepted, with the
   answer in `tried`. */
try: PROCEDURE EXPOSE tried
  DROP result
  ADDRESS PKGMANAGER arg(1)
  IF symbol('RESULT') = 'VAR' THEN DO; tried = result; RETURN 1; END
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

/* "name value" lines -> value */
field: PROCEDURE
  PARSE ARG text, name
  DO WHILE text <> ''
    PARSE VAR text l '0a'x text
    IF word(l, 1) = name THEN RETURN subword(l, 2)
  END
  RETURN ''

sortwords: PROCEDURE
  PARSE ARG s
  n = words(s)
  DO i = 1 TO n; w.i = word(s, i); END
  DO i = 1 TO n; DO j = i + 1 TO n
    IF w.j << w.i THEN DO; t = w.i; w.i = w.j; w.j = t; END
  END; END
  r = ''
  DO i = 1 TO n; r = r w.i; END
  RETURN strip(r)

rx_final: PROCEDURE
  RETURN wordpos(arg(1), 'done failed cancelled declined') > 0

waitphase: PROCEDURE EXPOSE answer error code out
  PARSE ARG id, want, secs
  DO secs
    CALL ask 'JOB' id
    IF field(answer, 'phase') = want THEN RETURN want
    IF rx_final(field(answer, 'state')) THEN RETURN field(answer, 'state')
    ADDRESS COMMAND 'Wait 1'
  END
  RETURN field(answer, 'phase')

waitstate: PROCEDURE EXPOSE answer error code out
  PARSE ARG id, want, secs
  DO secs
    CALL ask 'JOB' id 'FIELD state'
    IF answer = want | rx_final(answer) THEN RETURN answer
    ADDRESS COMMAND 'Wait 1'
  END
  RETURN answer

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

writefile: PROCEDURE
  PARSE ARG f, v
  ADDRESS COMMAND 'Delete >NIL:' f 'QUIET'
  CALL lineout f, v
  CALL lineout f
  RETURN

/* the registry as the CLI reports it, and a listing of the root */
clilist: PROCEDURE EXPOSE apkg root
  ADDRESS COMMAND apkg '--root' root 'list >'arg(1)
  RETURN

clihas: PROCEDURE
  PARSE ARG f, id
  t = readall(f)
  DO WHILE t <> ''
    PARSE VAR t l '0a'x t
    IF word(l, 1) = id THEN RETURN 1
  END
  RETURN 0

snapshot: PROCEDURE EXPOSE root
  ADDRESS COMMAND 'List >RAM:rxt-snap-'arg(1) root 'ALL LFORMAT="%p%n %l"'
  RETURN
