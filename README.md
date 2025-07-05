# ucored

ucored is a FreeBSD daemon to manage generated corefiles.  When devd takes a
coredump notification (disabled by default, but ucored(8) will turn them on
if it's enabled), it spawns off ucore-shuttle with parameters from the
notification.

ucore-shuttle, when it starts, will do any necessarily resolution (namely, jail)
to get the full path of the corefile that was generated.  It then promptly
delivers all of that over to ucored(8), which can act on it.

All of the above is implemented, but nothing else.  This is, admittedly, not
very useful.  The next step in development is to setup a lua environment.  I
think we'll use libucl to read a set of filter rules from /etc/ucored.conf at
startup time.  The lua script will return a callback to invoke that will handle
a new corefile.

Once we have all of the metadata associated with a core, we'll fork() off and
start executing the handler, which will apply the preloaded configuration.  I
suspect the config will be able to specify a few different actions based on
pattern matching: discard, move, rename, or ignore it.
