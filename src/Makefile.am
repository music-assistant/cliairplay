AUTOMAKE_OPTIONS = subdir-objects
bin_PROGRAMS = cliap2

GPERF_FILES = \
	../owntone-server/src/daap_query.gperf \
	../owntone-server/src/dacp_prop.gperf \
	../owntone-server/src/dmap_fields.gperf

GPERF_SRC = $(GPERF_FILES:.gperf=_hash.h)

LEXER_SRC  = \
    ../owntone-server/src/parsers/daap_lexer.l \
    ../owntone-server/src/parsers/smartpl_lexer.l \
    ../owntone-server/src/parsers/rsp_lexer.l \
    ../owntone-server/src/parsers/mpd_lexer.l
PARSER_SRC = \
    ../owntone-server/src/parsers/daap_parser.y \
    ../owntone-server/src/parsers/smartpl_parser.y \
    ../owntone-server/src/parsers/rsp_parser.y \
    ../owntone-server/src/parsers/mpd_parser.y

# This flag is given to Bison and tells it to produce headers. Note that
# automake recognizes this flag too, and has special logic around it, so don't
# change it to compound arguments (so for instance no "-dv"). I'm also not sure
# --defines will work instead of -d.
AM_YFLAGS = -d

PAIR_AP_SRC = \
	../owntone-server/src/pair_ap/pair_fruit.c \
    ../owntone-server/src/pair_ap/pair_homekit.c \
    ../owntone-server/src/pair_ap/pair-tlv.c \
	../owntone-server/src/pair_ap/pair.c \
    ../owntone-server/src/pair_ap/pair-tlv.h \
    ../owntone-server/src/pair_ap/pair-internal.h \
    ../owntone-server/src/pair_ap/pair.h

# Local copies of logger.c and misc.c with _exit() fix instead of abort()
# to prevent hanging on pthread_mutex errors during shutdown
LOCAL_PATCHED_SRC = \
    logger.c \
    misc.c

OWNTONE_SRC = \
    ../owntone-server/src/commands.c \
    ../owntone-server/src/dmap_common.c \
    ../owntone-server/src/evthr.c \
    ../owntone-server/src/evrtsp/rtsp.c \
    ../owntone-server/src/http.c \
    ../owntone-server/src/inputs/file.c \
    ../owntone-server/src/inputs/http.c \
    ../owntone-server/src/inputs/timer.c \
    ../owntone-server/src/listener.c \
    ../owntone-server/src/misc_json.c \
    ../owntone-server/src/misc_xml.c \
    ../owntone-server/src/outputs.c \
    ../owntone-server/src/outputs/airplay_events.c \
    ../owntone-server/src/transcode.c \
    ../owntone-server/src/worker.c \
	$(GPERF_SRC) \
	$(LEXER_SRC) $(PARSER_SRC)

cliap2_SOURCES = \
    airplay.c \
    cliap2.c \
    conffile.c \
    input.c \
    mass.c \
    player.c \
    ptpd.c \
    rtp_common.c \
    wrappers.c \
    $(LOCAL_PATCHED_SRC) \
    $(OWNTONE_SRC) \
    $(PAIR_AP_SRC)

cliap2_CPPFLAGS = \
	$(OWNTONE_CPPFLAGS) \
	$(OWNTONE_OPTS_CPPFLAGS) \
	$(COMMON_CPPFLAGS) \
    -I../owntone-server/src \
    -I../owntone-server/src/outputs \
	-D_GNU_SOURCE \
    -DCONFIG_GCRYPT \
    -DCONFDIR=\".\" \
	-DSTATEDIR=\"$(localstatedir)\"

# TODO: @bradkeifer Eliminate debug flags before first production release
cliap2_CFLAGS = \
    -ggdb -Og -Wall

cliap2_LDADD = \
	$(OWNTONE_LIBS) \
	$(OWNTONE_OPTS_LIBS) \
	$(COMMON_LIBS)

# This should ensure the headers are built first. automake knows how to make
# parser headers, but doesn't know how to do that for flex. So instead we set
# the C files as target, as the AM_LFLAGS will make sure headers are produced.
BUILT_SOURCES = \
	$(GPERF_SRC) \
	$(LEXER_SRC:.l=.h) $(PARSER_SRC:.y=.h)

# automake doesn't know how to make lexer headers, nor does it automatically
# include them, so need to specify them as EXTRA_DIST.
EXTRA_DIST = \
	$(GPERF_FILES) \
	$(LEXER_SRC:.l=.h)

# gperf construction rules
%_hash.h: %.gperf
	$(AM_V_GEN)$(GPERF) --output-file=$@ $<

# Rule for generating lexer headers. $@ is an automatic variable that is equal
# to the particular target name, so a header file name.
$(LEXER_SRC:.l=.h): $(LEXER_SRC)
	$(LEX) --header-file=$@ --stdout $(@:.h=.l) > /dev/null

# Anything built by make should be cleaned by make clean, but when it comes to
# flex/bison automake's support leaves something to be desired
clean-local:
	rm -f $(LEXER_SRC:.l=.[ch]) $(PARSER_SRC:.y=.[ch])
