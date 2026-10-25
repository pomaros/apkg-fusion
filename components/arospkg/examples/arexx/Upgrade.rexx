/* Upgrade.rexx -- ask for an upgrade and wait for the user's decision.
 *
 *     rx Upgrade.rexx <package>
 *
 * Not unattended. PkgManager works out what the upgrade would change and
 * shows the plan in its own window; the user presses Proceed or Cancel
 * there. ARexx cannot answer for the user. Meanwhile JOB reports the state
 * "confirm", and WAIT -- used here -- answers only once the job has ended.
 * ROLLBACK works the same way. Returns 0 when upgraded, 5 when the user
 * declined, 10 otherwise. */
PARSE ARG package .
OPTIONS RESULTS
IF package = '' THEN DO; SAY 'usage: rx Upgrade.rexx <package>'; EXIT 10; END
IF SHOW('P', 'PKGMANAGER') = 0 THEN DO; SAY 'PkgManager is not running'; EXIT 10; END

IF ask('UPGRADE' quote(package)) = 0 THEN DO; SAY 'not started:' error; EXIT 10; END
job = answer
SAY 'job' job': confirm or cancel the upgrade of' package 'in PkgManager'

CALL ask 'WAIT' job
state = field(answer, 'state')
SELECT
  WHEN state = 'done' THEN DO; SAY package 'upgraded'; EXIT 0; END
  WHEN state = 'declined' THEN DO; SAY 'declined in PkgManager; nothing changed'; EXIT 5; END
  OTHERWISE SAY 'not upgraded:' state field(answer, 'error') '--' field(answer, 'message')
END
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
