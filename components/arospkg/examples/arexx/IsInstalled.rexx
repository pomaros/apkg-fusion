/* IsInstalled.rexx -- is a package installed?
 *
 *     rx IsInstalled.rexx <package>
 *
 * Prints the answer and returns 0 when the package is installed, 5 when it
 * is not, 10 when the question could not be answered. A Shell script can
 * test that with "If WARN". Needs PkgManager running; nothing is changed. */
PARSE ARG package .
OPTIONS RESULTS
IF package = '' THEN DO; SAY 'usage: rx IsInstalled.rexx <package>'; EXIT 10; END
IF SHOW('P', 'PKGMANAGER') = 0 THEN DO; SAY 'PkgManager is not running'; EXIT 10; END

IF ask('INFO' quote(package) 'FIELD state') = 0 THEN DO
  SAY 'cannot tell:' error
  EXIT 10
END
IF answer = 'installed' THEN DO
  CALL ask 'INFO' quote(package) 'FIELD installedversion'
  SAY package 'is installed, version' answer
  EXIT 0
END
SAY package 'is not installed'
EXIT 5

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
