XCBPROTO_XCBINCLUDEDIR=../libxcb/xcb-proto/src

lib_LTLIBRARIES = libxcb-errors.la

xcbinclude_HEADERS = src/xcb_errors.h

libxcb_errors_la_SOURCES = src/xcb_errors.c $(OBJDIR)/extensions.c
pkgconfig_DATA = xcb-errors.pc

$(OBJDIR)/extensions.c: src/extensions.py
	python2 src/extensions.py $@ ${XCBPROTO_XCBINCLUDEDIR}/bigreq.xml ${XCBPROTO_XCBINCLUDEDIR}/composite.xml ${XCBPROTO_XCBINCLUDEDIR}/damage.xml ${XCBPROTO_XCBINCLUDEDIR}/dpms.xml ${XCBPROTO_XCBINCLUDEDIR}/dri2.xml ${XCBPROTO_XCBINCLUDEDIR}/dri3.xml ${XCBPROTO_XCBINCLUDEDIR}/ge.xml ${XCBPROTO_XCBINCLUDEDIR}/glx.xml ${XCBPROTO_XCBINCLUDEDIR}/present.xml ${XCBPROTO_XCBINCLUDEDIR}/randr.xml ${XCBPROTO_XCBINCLUDEDIR}/record.xml ${XCBPROTO_XCBINCLUDEDIR}/render.xml ${XCBPROTO_XCBINCLUDEDIR}/res.xml ${XCBPROTO_XCBINCLUDEDIR}/screensaver.xml ${XCBPROTO_XCBINCLUDEDIR}/shape.xml ${XCBPROTO_XCBINCLUDEDIR}/shm.xml ${XCBPROTO_XCBINCLUDEDIR}/sync.xml ${XCBPROTO_XCBINCLUDEDIR}/xc_misc.xml ${XCBPROTO_XCBINCLUDEDIR}/xevie.xml ${XCBPROTO_XCBINCLUDEDIR}/xf86dri.xml ${XCBPROTO_XCBINCLUDEDIR}/xf86vidmode.xml ${XCBPROTO_XCBINCLUDEDIR}/xfixes.xml ${XCBPROTO_XCBINCLUDEDIR}/xinerama.xml ${XCBPROTO_XCBINCLUDEDIR}/xinput.xml ${XCBPROTO_XCBINCLUDEDIR}/xkb.xml ${XCBPROTO_XCBINCLUDEDIR}/xprint.xml ${XCBPROTO_XCBINCLUDEDIR}/xproto.xml ${XCBPROTO_XCBINCLUDEDIR}/xselinux.xml ${XCBPROTO_XCBINCLUDEDIR}/xtest.xml ${XCBPROTO_XCBINCLUDEDIR}/xv.xml ${XCBPROTO_XCBINCLUDEDIR}/xvmc.xml

# ${XCBPROTO_XCBINCLUDEDIR}/*.xml

LIBRARY = $(lib_LTLIBRARIES:.la=)

CSRCS := $(notdir $(subst /,$/,$(libxcb_errors_la_SOURCES)))
CSRCS := $(CSRCS:%.h=)

INCLUDES += src

vpath %.c src

