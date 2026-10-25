/* Install.rexx -- install a package and report what really happened.
 *
 *     rx Install.rexx <package>
 *
 * INSTALL only starts the job and answers with its number; accepting the
 * job is not the install succeeding. WAIT answers when the job has ended,
 * with its outcome. Runs unattended: an install asks no questions. Returns
 * 0 when the package was installed, 10 otherwise. */
PARSE ARG package .
OPTIONS RESULTS
IF package = '' THEN DO; SAY 'usage: rx Install.rexx <package>'; EXIT 10; END
IF SHOW('P', 'PKGMANAGER') = 0 THEN DO; SAY 'PkgManager is not running'; EXIT 10; END

IF ask('INSTALL' quote(package)) = 0 THEN DO
  /* BUSY: another operation is running; NOTFOUND: not in the catalogue */
  SAY 'not started:' error
  EXIT 10
END
job = answer
SAY 'job' job': installing' package

CALL ask 'WAIT' job
state = field(answer, 'state')
IF state = 'done' THEN DO
  SAY package 'is installed'
  EXIT 0
END
SAY package 'was not installed:' state field(answer, 'error') '--' field(answer, 'message')
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
