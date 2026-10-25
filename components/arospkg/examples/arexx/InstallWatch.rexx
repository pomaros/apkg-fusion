/* InstallWatch.rexx -- install a package, show its progress, and optionally
 * try to cancel it.
 *
 *     rx InstallWatch.rexx <package> [cancel-after-seconds]
 *
 * Polls JOB once a second. With a number, asks for a cancel once that many
 * seconds have passed -- but only while JOB says the job can still be
 * cancelled (cancancel 1: while downloading, before anything is installed).
 * Once files are being put in place it cannot be, and the install finishes.
 * Runs unattended. */
PARSE ARG package limit .
OPTIONS RESULTS
IF package = '' THEN DO; SAY 'usage: rx InstallWatch.rexx <package> [cancel-after-seconds]'; EXIT 10; END
IF SHOW('P', 'PKGMANAGER') = 0 THEN DO; SAY 'PkgManager is not running'; EXIT 10; END

IF ask('INSTALL' quote(package)) = 0 THEN DO; SAY 'not started:' error; EXIT 10; END
job = answer
asked = 0
DO t = 0
  CALL ask 'JOB' job
  state = field(answer, 'state'); phase = field(answer, 'phase')
  done = field(answer, 'done'); total = field(answer, 'total')
  IF total > 0 THEN SAY 'job' job state phase done * 100 % total'%'
  ELSE SAY 'job' job state phase
  IF wordpos(state, 'done failed cancelled declined') > 0 THEN LEAVE
  IF limit <> '' & t >= limit & asked = 0 & field(answer, 'cancancel') = 1 THEN DO
    IF ask('CANCEL' job) THEN SAY 'cancel requested'
    ELSE SAY 'cancel refused:' error
    asked = 1
  END
  ADDRESS COMMAND 'Wait 1'
END
IF limit <> '' & asked = 0 THEN SAY 'no cancel was asked: the job was never cancellable after' limit 's'
SAY 'outcome:' state field(answer, 'error')
IF state = 'done' THEN EXIT 0
EXIT 10

/* The same few lines are in every example. Success means RESULT was set:
 * RC cannot be relied on with the Regina in current AROS distributions,
 * which puts RESULT's text into RC. LASTERROR says why a command failed. */
ask: PROCEDURE EXPOSE answer error
  DROP result
  ADDRESS PKGMANAGER arg(1)
  IF symbol('RESULT') = 'VAR' THEN DO; answer = result; error = ''; RETURN 1; END
  ADDRESS PKGMANAGER 'LASTERROR'
  answer = ''; error = result
  RETURN 0

/* A package name as one argument, whatever it contains. */
quote: PROCEDURE
  s = arg(1); r = ''
  DO i = 1 TO length(s)
    c = substr(s, i, 1)
    IF c = '"' | c = '*' THEN r = r || '*'
    r = r || c
  END
  RETURN '"' || r || '"'

/* One field out of JOB's "name value" lines. */
field: PROCEDURE
  PARSE ARG text, name
  DO WHILE text <> ''
    PARSE VAR text line '0a'x text
    IF word(line, 1) = name THEN RETURN subword(line, 2)
  END
  RETURN ''
