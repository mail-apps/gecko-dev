dnl This Source Code Form is subject to the terms of the Mozilla Public
dnl License, v. 2.0. If a copy of the MPL was not distributed with this
dnl file, You can obtain one at http://mozilla.org/MPL/2.0/.

dnl For use in AC_SUBST replacement
define([MOZ_DIVERSION_SUBST], 11)

dnl Replace AC_SUBST to store values in a format suitable for python.
dnl The necessary comma after the tuple can't be put here because it
dnl can mess around with things like:
dnl    AC_SOMETHING(foo,AC_SUBST(),bar)
define([AC_SUBST],
[ifdef([AC_SUBST_SET_$1], [m4_fatal([Cannot use AC_SUBST and AC_SUBST_SET on the same variable ($1)])],
[ifdef([AC_SUBST_LIST_$1], [m4_fatal([Cannot use AC_SUBST and AC_SUBST_LIST on the same variable ($1)])],
[ifdef([AC_SUBST_$1], ,
[define([AC_SUBST_$1], )dnl
AC_DIVERT_PUSH(MOZ_DIVERSION_SUBST)dnl
    (''' $1 ''', r''' [$]$1 ''')
AC_DIVERT_POP()dnl
])])])])

dnl Like AC_SUBST, but makes the value available as a set in python,
dnl with values got from the value of the environment variable, split on
dnl whitespaces.
define([AC_SUBST_SET],
[ifdef([AC_SUBST_$1], [m4_fatal([Cannot use AC_SUBST and AC_SUBST_SET on the same variable ($1)])],
[ifdef([AC_SUBST_LIST$1], [m4_fatal([Cannot use AC_SUBST_LIST and AC_SUBST_SET on the same variable ($1)])],
[ifdef([AC_SUBST_SET_$1], ,
[define([AC_SUBST_SET_$1], )dnl
AC_DIVERT_PUSH(MOZ_DIVERSION_SUBST)dnl
    (''' $1 ''', unique_list(r''' [$]$1 '''.split()))
AC_DIVERT_POP()dnl
])])])])

dnl Like AC_SUBST, but makes the value available as a list in python,
dnl with values got from the value of the environment variable, split on
dnl whitespaces.
define([AC_SUBST_LIST],
[ifdef([AC_SUBST_$1], [m4_fatal([Cannot use AC_SUBST and AC_SUBST_LIST on the same variable ($1)])],
[ifdef([AC_SUBST_SET_$1], [m4_fatal([Cannot use AC_SUBST_SET and AC_SUBST_LIST on the same variable ($1)])],
[ifdef([AC_SUBST_LIST_$1], ,
[define([AC_SUBST_LIST_$1], )dnl
AC_DIVERT_PUSH(MOZ_DIVERSION_SUBST)dnl
    (''' $1 ''', list(r''' [$]$1 '''.split()))
AC_DIVERT_POP()dnl
])])])])

dnl Ignore AC_SUBSTs for variables we don't have use for but that autoconf
dnl itself exports.
define([AC_SUBST_CFLAGS], )
define([AC_SUBST_CPPFLAGS], )
define([AC_SUBST_CXXFLAGS], )
define([AC_SUBST_FFLAGS], )
define([AC_SUBST_DEFS], )
define([AC_SUBST_LDFLAGS], )
define([AC_SUBST_LIBS], )

dnl Wrap AC_DEFINE to store values in a format suitable for python.
dnl autoconf's AC_DEFINE still needs to be used to fill confdefs.h,
dnl which is #included during some compile checks.
dnl The necessary comma after the tuple can't be put here because it
dnl can mess around with things like:
dnl    AC_SOMETHING(foo,AC_DEFINE(),bar)
define([_MOZ_AC_DEFINE], defn([AC_DEFINE]))
define([AC_DEFINE],
[cat >> confdefs.pytmp <<\EOF
    (''' $1 ''', ifelse($#, 2, [r''' $2 '''], $#, 3, [r''' $2 '''], ' 1 '))
EOF
ifelse($#, 2, _MOZ_AC_DEFINE([$1], [$2]), $#, 3, _MOZ_AC_DEFINE([$1], [$2], [$3]),_MOZ_AC_DEFINE([$1]))dnl
])

dnl Wrap AC_DEFINE_UNQUOTED to store values in a format suitable for
dnl python.
define([_MOZ_AC_DEFINE_UNQUOTED], defn([AC_DEFINE_UNQUOTED]))
define([AC_DEFINE_UNQUOTED],
[cat >> confdefs.pytmp <<EOF
    (''' $1 ''', ifelse($#, 2, [r''' $2 '''], $#, 3, [r''' $2 '''], ' 1 '))
EOF
ifelse($#, 2, _MOZ_AC_DEFINE_UNQUOTED($1, $2), $#, 3, _MOZ_AC_DEFINE_UNQUOTED($1, $2, $3),_MOZ_AC_DEFINE_UNQUOTED($1))dnl
])

dnl Replace AC_OUTPUT to create and call a python config.status
define([MOZ_CREATE_CONFIG_STATUS],
[dnl Top source directory in Windows format (as opposed to msys format).
WIN_TOP_SRC=
encoding=utf-8
case "$host_os" in
mingw*)
    WIN_TOP_SRC=`cd $srcdir; pwd -W`
    encoding=mbcs
    ;;
esac
AC_SUBST(WIN_TOP_SRC)

dnl Used in all Makefile.in files
top_srcdir=$srcdir
AC_SUBST(top_srcdir)

dnl Picked from autoconf 2.13
trap '' 1 2 15
AC_CACHE_SAVE

trap 'rm -f $CONFIG_STATUS conftest*; exit 1' 1 2 15
: ${CONFIG_STATUS=./config.status}

dnl We're going to need [ ] for python syntax.
changequote(<<<, >>>)dnl
echo creating $CONFIG_STATUS

extra_python_path=${COMM_BUILD:+"'mozilla', "}

cat > $CONFIG_STATUS <<EOF
#!${PYTHON}
# coding=$encoding

import os
import types
dnl topsrcdir is the top source directory in native form, as opposed to a
dnl form suitable for make.
topsrcdir = '''${WIN_TOP_SRC:-$srcdir}'''
if not os.path.isabs(topsrcdir):
    rel = os.path.join(os.path.dirname(<<<__file__>>>), topsrcdir)
    topsrcdir = os.path.abspath(rel)
topsrcdir = os.path.normpath(topsrcdir)

topobjdir = os.path.abspath(os.path.dirname(<<<__file__>>>))

def unique_list(l):
    result = []
    for i in l:
        if l not in result:
            result.append(i)
    return result

dnl All defines and substs are stored with an additional space at the beginning
dnl and at the end of the string, to avoid any problem with values starting or
dnl ending with quotes.
defines = [(name[1:-1], value[1:-1]) for name, value in [
EOF

dnl confdefs.pytmp contains AC_DEFINEs, in the expected format, but
dnl lacks the final comma (see above).
sed 's/$/,/' confdefs.pytmp >> $CONFIG_STATUS
rm confdefs.pytmp confdefs.h

cat >> $CONFIG_STATUS <<\EOF
] ]

substs = [(name[1:-1], value[1:-1] if isinstance(value, types.StringTypes) else value) for name, value in [
EOF

dnl The MOZ_DIVERSION_SUBST output diversion contains AC_SUBSTs, in the
dnl expected format, but lacks the final comma (see above).
sed 's/$/,/' >> $CONFIG_STATUS <<EOF
undivert(MOZ_DIVERSION_SUBST)dnl
EOF

dnl Add in the output from the subconfigure script
for ac_subst_arg in $_subconfigure_ac_subst_args; do
  variable='$'$ac_subst_arg
  echo "    (''' $ac_subst_arg ''', r''' `eval echo $variable` ''')," >> $CONFIG_STATUS
done

cat >> $CONFIG_STATUS <<\EOF
] ]

dnl List of AC_DEFINEs that aren't to be exposed in ALLDEFINES
non_global_defines = [
EOF

if test -n "$_NON_GLOBAL_ACDEFINES"; then
  for var in $_NON_GLOBAL_ACDEFINES; do
    echo "    '$var'," >> $CONFIG_STATUS
  done
fi

cat >> $CONFIG_STATUS <<EOF
]

__all__ = ['topobjdir', 'topsrcdir', 'defines', 'non_global_defines', 'substs']
EOF

# We don't want js/src/config.status to do anything in gecko builds.
if test -z "$BUILDING_JS" -o -n "$JS_STANDALONE"; then

    cat >> $CONFIG_STATUS <<EOF
dnl Do the actual work
if __name__ == '__main__':
    args = dict([(name, globals()[name]) for name in __all__])
    from mozbuild.config_status import config_status
    config_status(**args)
EOF

fi

changequote([, ])

chmod +x $CONFIG_STATUS
])

define([MOZ_RUN_CONFIG_STATUS],
[

MOZ_RUN_ALL_SUBCONFIGURES()

rm -fr confdefs* $ac_clean_files
dnl Execute config.status, unless --no-create was passed to configure.
if test "$no_create" != yes && ! ${PYTHON} $CONFIG_STATUS; then
    trap '' EXIT
    exit 1
fi
])

define([m4_fatal],[
errprint([$1
])
m4exit(1)
])

define([AC_OUTPUT], [ifelse($#_$1, 1_, [MOZ_CREATE_CONFIG_STATUS()
MOZ_RUN_CONFIG_STATUS()],
[m4_fatal([Use CONFIGURE_SUBST_FILES in moz.build files to create substituted files.])]
)])

define([AC_CONFIG_HEADER],
[m4_fatal([Use CONFIGURE_DEFINE_FILES in moz.build files to produce header files.])
])

define([MOZ_BUILD_BACKEND],
[
BUILD_BACKENDS="RecursiveMake"

MOZ_ARG_ENABLE_STRING(build-backend,
[  --enable-build-backend={$($(dirname ]$[0)/$1/mach python -c "from mozbuild.backend import backends; print ','.join(sorted(backends))")}
                         Enable additional build backends],
[ BUILD_BACKENDS="RecursiveMake `echo $enableval | sed 's/,/ /g'`"])

AC_SUBST_SET([BUILD_BACKENDS])
])
