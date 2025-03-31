# Makefile.am - Tools directory
#     	Copyright (C) 2003, 2007 Free Software Foundation, Inc.
#
# This file is part of GnuPG.
#
# GnuPG is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; either version 3 of the License, or
# (at your option) any later version.
#
# GnuPG is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, see <https://www.gnu.org/licenses/>.

EXTRA_DIST = \
	Manifest watchgnupg.c no-libgcrypt.c \
	addgnupghome applygnupgdefaults gpg-authcode-sign.sh \
	lspgpot mail-signed-keys convert-from-106 sockprox.c \
	ccidmon.c ChangeLog-2011 \
	gpg-connect-agent-w32info.rc lcr-connect-agent.w32-manifest.in \
	gpgconf-w32info.rc           lcrconf.w32-manifest.in \
	gpgtar-w32info.rc            lcrtar.w32-manifest.in \
	gpg-check-pattern-w32info.rc lcr-check-pattern.w32-manifest.in \
	gpg-wks-client-w32info.rc    lcr-wks-client.w32-manifest.in \
	gpg-card-w32info.rc          lcr-card.w32-manifest.in

AM_CPPFLAGS =
include $(top_srcdir)/am/cmacros.am

if HAVE_W32_SYSTEM
lcr_connect_agent_rc_objs = lcr-connect-agent-w32info.o
lcrconf_rc_objs           = lcrconf-w32info.o
lcr_card_rc_objs          = lcr-card-w32info.o
lcrtar_rc_objs            = lcrtar-w32info.o
lcr_check_pattern_rc_objs = lcr-check-pattern-w32info.o
lcr_wks_client_rc_objs    = lcr-wks-client-w32info.o

lcr-connect-agent-w32info.o  : lcr-connect-agent.w32-manifest \
                                                     ../common/w32info-rc.h
lcrconf-w32info.o            : lcrconf.w32-manifest  ../common/w32info-rc.h
lcr-card-w32info.o           : lcr-card.w32-manifest ../common/w32info-rc.h
lcrtar-w32info.o             : lcrtar.w32-manifest   ../common/w32info-rc.h
lcr-check-pattern-w32info.o  : lcr-check-pattern.w32-manifest \
                                                     ../common/w32info-rc.h
lcr-wks-client-w32info.o     : lcr-wks-client.w32-manifest \
                                                     ../common/w32info-rc.h

endif

AM_CFLAGS = $(LIBGCRYPT_CFLAGS) $(GPG_ERROR_CFLAGS) $(LIBASSUAN_CFLAGS)

sbin_SCRIPTS = addgnupghome applygnupgdefaults
bin_SCRIPTS = lcr-authcode-sign.sh

if BUILD_WKS_TOOLS
  lcr_wks_server = lcr-wks-server
else
  lcr_wks_server =
endif


bin_PROGRAMS = lcrconf lcr-connect-agent lcr-card lcr-wks-client
if !HAVE_W32_SYSTEM
bin_PROGRAMS += watchgnupg gpgparsemail ${lcr_wks_server} gpgsplit \
                gpg-mail-tube
else
bin_PROGRAMS += lcrconf-w32
endif

libexec_PROGRAMS = lcr-check-pattern lcr-pair-tool
if !HAVE_W32_SYSTEM
libexec_PROGRAMS += lcr-auth
endif

noinst_PROGRAMS = clean-sat make-dns-cert

if BUILD_GPGTAR
  bin_PROGRAMS += lcrtar
else
  noinst_PROGRAMS += lcrtar
endif

common_libs = $(libcommon)
commonpth_libs = $(libcommonpth)

pwquery_libs = ../common/libsimple-pwquery.a


regexp_libs = ../regexp/libregexp.a


gpgsplit_LDADD = $(common_libs) \
	         $(LIBGCRYPT_LIBS) $(GPG_ERROR_LIBS) \
		 $(ZLIBS) $(LIBINTL) $(NETLIBS) $(LIBICONV)

lcrconf_SOURCES = gpgconf.c gpgconf.h gpgconf-comp.c

# common sucks in gpg-error, will they, nil they (some compilers
# do not eliminate the supposed-to-be-unused-inline-functions).
lcrconf_LDADD = $(common_libs) \
                $(LIBINTL) $(LIBGCRYPT_LIBS) $(GPG_ERROR_LIBS) $(NETLIBS) \
	        $(LIBICONV) $(W32SOCKLIBS) \
	        $(lcrconf_rc_objs)
lcrconf_LDFLAGS =

lcrconf_w32_SOURCES = $(lcrconf_SOURCES)
lcrconf_w32_LDADD = $(lcrconf_LDADD)
lcrconf_w32_LDFLAGS = $(lcrconf_LDFLAGS) -Wl,-subsystem,windows

gpgparsemail_SOURCES = gpgparsemail.c rfc822parse.c rfc822parse.h
gpgparsemail_LDADD =

watchgnupg_SOURCES = watchgnupg.c
watchgnupg_LDADD = $(NETLIBS)

lcr_connect_agent_SOURCES = gpg-connect-agent.c
lcr_connect_agent_LDADD = ../common/libgpgrl.a $(common_libs) \
	                  $(LIBASSUAN_LIBS) $(LIBGCRYPT_LIBS) \
                          $(GPG_ERROR_LIBS) \
                          $(LIBREADLINE) $(LIBINTL) $(NETLIBS) $(LIBICONV) \
                          $(lcr_connect_agent_rc_objs)


lcr_card_SOURCES   = \
	gpg-card.c \
	gpg-card.h     \
	card-call-scd.c \
	card-keys.c \
	card-yubikey.c \
	card-misc.c

lcr_card_LDADD = \
	../common/libgpgrl.a $(common_libs) \
	$(LIBASSUAN_LIBS) $(LIBGCRYPT_LIBS) \
	$(GPG_ERROR_LIBS) \
        $(LIBREADLINE) $(LIBINTL) $(NETLIBS) $(LIBICONV) \
        $(lcr_card_rc_objs)


lcr_check_pattern_SOURCES = gpg-check-pattern.c
lcr_check_pattern_CFLAGS = $(LIBGCRYPT_CFLAGS) $(GPG_ERROR_CFLAGS) $(INCICONV)
lcr_check_pattern_LDADD = $(common_libs) $(regexp_libs) $(LIBGCRYPT_LIBS) \
			  $(GPG_ERROR_LIBS) \
			  $(LIBINTL) $(NETLIBS) $(LIBICONV) $(W32SOCKLIBS) \
			  $(LIBICONV) \
		          $(lcr_check_pattern_rc_objs)

lcrtar_SOURCES = \
	gpgtar.c gpgtar.h \
	gpgtar-create.c \
	gpgtar-extract.c \
	gpgtar-list.c
lcrtar_CFLAGS = $(LIBGCRYPT_CFLAGS) $(GPG_ERROR_CFLAGS)
lcrtar_LDADD = $(libcommon) $(LIBGCRYPT_LIBS) $(GPG_ERROR_LIBS) \
               $(LIBINTL) $(NETLIBS) $(LIBICONV) $(W32SOCKLIBS) \
	       $(lcrtar_rc_objs)

lcr_wks_server_SOURCES = \
	gpg-wks-server.c \
	gpg-wks.h \
	wks-util.c \
	wks-receive.c \
	rfc822parse.c rfc822parse.h \
	mime-parser.c mime-parser.h \
	mime-maker.c  mime-maker.h  \
	send-mail.c   send-mail.h

lcr_wks_server_CFLAGS = $(LIBGCRYPT_CFLAGS) $(GPG_ERROR_CFLAGS) $(INCICONV)
lcr_wks_server_LDADD = $(libcommon) $(LIBGCRYPT_LIBS) $(GPG_ERROR_LIBS) \
		       $(LIBINTL) $(LIBICONV)

lcr_wks_client_SOURCES = \
	gpg-wks-client.c \
	gpg-wks.h  \
	wks-util.c \
	wks-receive.c \
	rfc822parse.c rfc822parse.h \
	mime-parser.c mime-parser.h \
	mime-maker.h  mime-maker.c  \
	send-mail.c   send-mail.h   \
	call-dirmngr.c call-dirmngr.h

lcr_wks_client_CFLAGS = $(LIBASSUAN_CFLAGS) $(LIBGCRYPT_CFLAGS) \
			$(GPG_ERROR_CFLAGS) $(INCICONV)
lcr_wks_client_LDADD = $(libcommon) \
		       $(LIBASSUAN_LIBS) $(LIBGCRYPT_LIBS) $(GPG_ERROR_LIBS) \
		       $(LIBINTL) $(LIBICONV) $(NETLIBS) \
	               $(lcr_wks_client_rc_objs)

gpg_mail_tube_SOURCES = \
        gpg-mail-tube.c \
        rfc822parse.c rfc822parse.h \
	mime-maker.c  mime-maker.h

gpg_mail_tube_CFLAGS = $(LIBGCRYPT_CFLAGS) $(GPG_ERROR_CFLAGS) $(INCICONV)
gpg_mail_tube_LDADD =  $(libcommon) \
		       $(LIBGCRYPT_LIBS) $(GPG_ERROR_LIBS) \
                       $(LIBINTL) $(LIBICONV)


lcr_pair_tool_SOURCES = \
	gpg-pair-tool.c

lcr_pair_tool_CFLAGS = $(LIBGCRYPT_CFLAGS) $(GPG_ERROR_CFLAGS) $(INCICONV)
lcr_pair_tool_LDADD =  $(libcommon) \
		       $(LIBGCRYPT_LIBS) $(GPG_ERROR_LIBS) \
		       $(LIBINTL) $(LIBICONV) $(W32SOCKLIBS)

lcr_auth_SOURCES = gpg-auth.c
lcr_auth_LDADD = $(common_libs) \
	         $(LIBASSUAN_LIBS) $(LIBGCRYPT_LIBS) \
                 $(GPG_ERROR_LIBS) \
                 $(LIBINTL) $(NETLIBS) $(LIBICONV)

# Instead of a symlink we install a simple wrapper script for the new
# gpg-wks-client location.  We assume bin is a sibling of libexec.
install-exec-local:
	$(mkinstalldirs) $(DESTDIR)$(libexecdir)
	(set -e ;\
	 if [ "$(libexecdir)" != "$(bindir)" ]; then \
	   printf '#!/bin/sh\nexec "$(bindir)/lcr-wks-client" "$$@"\n' \
	          > $(DESTDIR)$(libexecdir)/lcr-wks-client ;\
	   chmod +x $(DESTDIR)$(libexecdir)/lcr-wks-client ;\
	 fi )

uninstall-local:
	(if [ "$(libexecdir)" != "$(bindir)" ]; then \
	   rm $(DESTDIR)$(libexecdir)/lcr-wks-client || true ;\
         fi )

# Make sure that all libs are build before we use them.  This is
# important for things like make -j2.
$(PROGRAMS): $(common_libs) $(pwquery_libs) ../common/libgpgrl.a
