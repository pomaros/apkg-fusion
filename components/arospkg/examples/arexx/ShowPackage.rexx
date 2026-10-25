/* ShowPackage.rexx -- open a package's details in PkgManager.
 *
 *     rx ShowPackage.rexx <package>
 *
 * Brings the window to the front with the package selected: its summary,
 * size, and what it needs from this machine. From there the user decides.
 * Nothing is installed or changed. */
PARSE ARG package .
OPTIONS RESULTS
IF package = '' THEN DO; SAY 'usage: rx ShowPackage.rexx <package>'; EXIT 10; END
IF SHOW('P', 'PKGMANAGER') = 0 THEN DO; SAY 'PkgManager is not running'; EXIT 10; END

IF ask('SHOW' quote(package)) = 0 THEN DO
  SAY 'cannot show' package':' error
  EXIT 10
END
SAY 'PkgManager shows' package
EXIT 0

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
