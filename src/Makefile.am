AUTOMAKE_OPTIONS = subdir-objects
bin_PROGRAMS = cliap2

PAIR_AP_SRC = \
	../owntone-server/src/pair_ap/pair_fruit.c \
    ../owntone-server/src/pair_ap/pair_homekit.c \
    ../owntone-server/src/pair_ap/pair-tlv.c \
	../owntone-server/src/pair_ap/pair.c \
    ../owntone-server/src/pair_ap/pair-tlv.h \
    ../owntone-server/src/pair_ap/pair-internal.h \
    ../owntone-server/src/pair_ap/pair.h

OWNTONE_SRC = \
    ../owntone-server/src/commands.c \
    ../owntone-server/src/evthr.c \
    ../owntone-server/src/evrtsp/rtsp.c \
    ../owntone-server/src/http.c \
    ../owntone-server/src/input.c \
    ../owntone-server/src/inputs/file.c \
    ../owntone-server/src/inputs/http.c \
    ../owntone-server/src/inputs/timer.c \
    ../owntone-server/src/listener.c \
    ../owntone-server/src/logger.c \
    ../owntone-server/src/misc.c \
    ../owntone-server/src/misc_json.c \
    ../owntone-server/src/misc_xml.c \
    ../owntone-server/src/outputs.c \
    ../owntone-server/src/outputs/airplay.c \
    ../owntone-server/src/outputs/airplay_events.c \
    ../owntone-server/src/outputs/rtp_common.c \
    ../owntone-server/src/transcode.c \
    ../owntone-server/src/worker.c

cliap2_SOURCES = \
    conffile.c \
    cliap2.c \
    mass.c \
    player.c \
    wrappers.c \
    $(OWNTONE_SRC) \
    $(PAIR_AP_SRC)

cliap2_CPPFLAGS = \
	$(OWNTONE_CPPFLAGS) \
	$(OWNTONE_OPTS_CPPFLAGS) \
	$(COMMON_CPPFLAGS) \
    -I../owntone-server/src \
	-D_GNU_SOURCE \
    -DCONFIG_GCRYPT \
    -DCONFDIR=\".\" \
	-DSTATEDIR=\"$(localstatedir)\"

cliap2_CFLAGS = \
    -ggdb -Og

cliap2_LDADD = \
	$(OWNTONE_LIBS) \
	$(OWNTONE_OPTS_LIBS) \
	$(COMMON_LIBS)
