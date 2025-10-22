PHP_ARG_ENABLE(trace, whether to enable trace support,
[  --enable-trace           Enable trace support])

if test "$PHP_TRACE" != "no"; then
  PHP_NEW_EXTENSION(trace, trace.c, $ext_shared)
fi
